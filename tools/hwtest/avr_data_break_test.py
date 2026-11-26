#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hardware test of the AVR JTAG data breakpoints with the EDBG commands.

No host tool sends data breakpoints today. This script sends them as an
EDBG host does, over the CMSIS-DAP v1 (HID) interface of an image with
MUPP_CMSIS_DAP_AVR. See docs/avr.md, "JTAG data breakpoints", and
docs/cmsis-dap.md, "AVR8 scope on JTAG".

Load a test program into the target first (avrdude or Bloom). The program
must read and write a known SRAM address. Program OCDEN. For each mode the
script does these steps:

  1. RESET: the core stops at the reset vector.
  2. HW_BREAK_SET with type 2 (data), the number, the address and the mode.
  3. RUN, then AVR_EVT until a break event comes, or the timeout.
  4. Show the PC and the cause of the break event.
  5. HW_BREAK_CLEAR.

A mode passes when a break event with cause 1 comes in the time limit.
The PC is the word after the instruction that makes the access. Compare
it with the disassembly of the test program (avr-objdump -d).

The script starts with the sign-on, the AVR8 parameters (mega, debugging,
JTAG, a TCK of --clock-khz), the device context of an ATmega32U4,
ACTIVATE_PHYSICAL and ATTACH. It ends with DETACH (the program runs),
DEACTIVATE_PHYSICAL and the sign-off. The device context has the values of
the ATmega32U4 only (and --ocd-revision). For another device, change
device_context().

Usage:
    python3 avr_data_break_test.py --address 0x0100
    python3 avr_data_break_test.py --address 0x0210 --modes write --number 3
    python3 avr_data_break_test.py --medbg --address 0x0100

The data address is a data space address (SRAM starts at 0x0100 on the
ATmega32U4). Needs pyusb (and libusb). On Linux the script detaches the
usbhid driver from the interface. The unit tests in
tools/tests/test_avr_data_break_test.py need no pyusb and no hardware.
"""

import argparse
import struct
import sys
import time

DEFAULT_ID = (0x1209, 0x0001)
DEFAULT_INTERFACE = 3
# MUPP_AVR_USB_ID=medbg (docs/avr.md, "The medbg build")
MEDBG_ID = (0x03EB, 0x2145)
MEDBG_INTERFACE = 0
REPORT_SIZE = 64
TIMEOUT_MS = 2000

AVR_CMD, AVR_RSP, AVR_EVT = 0x80, 0x81, 0x82
SOF = 0x0E
HOUSEKEEPING, AVR8 = 0x01, 0x12
SIGN_ON, SIGN_OFF = 0x10, 0x11

# AVR8 generic commands (DS50002630A, chapter 7.1)
SET = 0x01
ACTIVATE_PHYSICAL, DEACTIVATE_PHYSICAL = 0x10, 0x11
ATTACH, DETACH = 0x13, 0x14
RESET, RUN = 0x30, 0x32
HW_BREAK_SET, HW_BREAK_CLEAR = 0x40, 0x41
RSP_OK, RSP_FAILED = 0x80, 0xA0
EVT_BREAK = 0x40

# HW_BREAK_SET type 2 (data) and the modes. DS50002630A gives only type 1
# and mode 3 (program). These are the values of AVaRICE (src/jtag3bp.cc)
# and of the JTAGICE mkII CMND_SET_BREAK.
DATA_TYPE = 0x02
MODES = {"read": 0x00, "write": 0x01, "access": 0x02}

PASS, FAIL = "PASS", "FAIL"


class EdbgError(Exception):
    pass


def frame(sequence, handler, payload):
    """A command frame: [0x0E][version 0][sequence u16][handler][payload]"""
    return bytes([SOF, 0x00]) + struct.pack("<H", sequence) + bytes([handler]) + bytes(payload)


def fragments(data, size=REPORT_SIZE):
    """The AVR_CMD reports of a frame: [0x80][m << 4 | n][size u16][data]"""
    chunk = size - 4
    parts = [data[i:i + chunk] for i in range(0, len(data), chunk)] or [b""]
    if len(parts) > 15:
        raise EdbgError("frame too long")
    n = len(parts)
    return [bytes([AVR_CMD, (m + 1) << 4 | n]) + struct.pack(">H", len(part)) + part for m, part in enumerate(parts)]


def parse_event(report):
    """The (pc, cause) of a break event in an AVR_EVT report, or None"""
    if len(report) < 3 or report[0] != AVR_EVT:
        raise EdbgError("bad AVR_EVT report")
    size = report[1] << 8 | report[2]
    event = bytes(report[3:3 + size])
    if size < 11 or event[0] != SOF or event[4] != AVR8 or event[5] != EVT_BREAK:
        return None
    return struct.unpack_from("<I", event, 6)[0], event[10]


def break_set_command(number, address, mode):
    """HW_BREAK_SET [version][type 2][number][address u32][mode]"""
    return bytes([HW_BREAK_SET, 0x00, DATA_TYPE, number]) + struct.pack("<I", address) + bytes([mode])


def device_context(ocd_revision=3):
    """The device context (context 2) of the ATmega32U4, as avrdude
    struct mega_device_desc"""
    page, flash = 128, 32768
    desc = struct.pack("<HIII", page, flash, 0, (flash - page * 4) // 2)
    desc += bytes([0x00, 0x01, 0x00, 0x04, 4, ocd_revision, 1, 0, 0, 0, 0x31, 0x22, 0x21, 0x1F, 0x20, 0x57, 0x00])
    return desc


def avr8_set(context, address, value):
    return bytes([SET, 0x00, context, address, len(value)]) + bytes(value)


class Edbg:
    """The EDBG commands over a HID report function: xfer(report) sends
    one report and returns the reply report."""

    def __init__(self, xfer):
        self.xfer = xfer
        self.sequence = 0

    def command(self, handler, payload):
        self.sequence = (self.sequence + 1) & 0xFFFF
        for report in fragments(frame(self.sequence, handler, payload)):
            reply = self.xfer(report)
            if reply[0] != AVR_CMD or reply[1] == 0xFF:
                raise EdbgError("fragment rejected")
        data = b""
        while True:
            reply = self.xfer(bytes([AVR_RSP]))
            if reply[0] != AVR_RSP or reply[1] == 0:
                raise EdbgError("no response")
            size = reply[2] << 8 | reply[3]
            data += bytes(reply[4:4 + size])
            if reply[1] >> 4 == reply[1] & 0x0F:
                break
        if len(data) < 5 or data[0] != SOF or struct.unpack_from("<H", data, 1)[0] != self.sequence:
            raise EdbgError("bad response frame")
        return data[4:]

    def avr8(self, payload, expect=RSP_OK):
        response = self.command(AVR8, payload)
        if response[0] == RSP_FAILED:
            code = response[2] if len(response) > 2 else 0
            raise EdbgError("AVR8 command 0x%02X failed with code 0x%02X" % (payload[0], code))
        if expect is not None and response[0] != expect:
            raise EdbgError("AVR8 command 0x%02X: response 0x%02X" % (payload[0], response[0]))
        return response

    def event(self):
        return parse_event(self.xfer(bytes([AVR_EVT])))

    def wait_event(self, timeout_s, clock=time.monotonic, sleep=time.sleep):
        end = clock() + timeout_s
        while True:
            event = self.event()
            if event is not None:
                return event
            if clock() >= end:
                return None
            sleep(0.01)

    def open(self, clock_khz, ocd_revision):
        self.command(HOUSEKEEPING, [SIGN_ON, 0x00])
        self.avr8(avr8_set(0, 0, [0x02]))  # variant mega
        self.avr8(avr8_set(0, 1, [0x02]))  # function debugging
        self.avr8(avr8_set(1, 0, [0x04]))  # physical JTAG
        self.avr8(avr8_set(1, 0x21, struct.pack("<H", clock_khz)))
        self.avr8(avr8_set(1, 0x01, [0, 0, 0, 0]))
        self.avr8(avr8_set(2, 0, device_context(ocd_revision)))
        response = self.avr8([ACTIVATE_PHYSICAL, 0x00, 0x00], expect=None)
        self.avr8([ATTACH, 0x00, 0x00])
        if self.wait_event(1.0) is None:
            raise EdbgError("no break event after ATTACH (OCDEN?)")
        return response

    def close(self):
        self.avr8([DETACH, 0x00])
        self.avr8([DEACTIVATE_PHYSICAL, 0x00])
        self.command(HOUSEKEEPING, [SIGN_OFF, 0x00, 0x00])


def run_mode(edbg, number, address, mode_name, timeout_s, **wait):
    """One mode: (verdict, pc, cause). pc and cause are None without a
    break event."""
    edbg.avr8([RESET, 0x00, 0x01])
    if edbg.wait_event(1.0, **wait) is None:
        raise EdbgError("no break event after RESET")
    edbg.avr8(break_set_command(number, address, MODES[mode_name]))
    try:
        edbg.avr8([RUN, 0x00])
        event = edbg.wait_event(timeout_s, **wait)
        if event is None:
            # The RESET of the next mode stops the core.
            return FAIL, None, None
        pc, cause = event
        return (PASS if cause == 1 else FAIL), pc, cause
    finally:
        edbg.avr8([HW_BREAK_CLEAR, 0x00, number])


def report_line(mode_name, verdict, pc, cause):
    if pc is None:
        return "%-7s %s  no break event" % (mode_name, verdict)
    return "%-7s %s  PC word 0x%05X (byte 0x%05X), cause %d" % (mode_name, verdict, pc, pc * 2, cause)


def run(edbg, args, out=sys.stdout):
    idcode = edbg.open(args.clock_khz, args.ocd_revision)
    if len(idcode) >= 6:
        print("IDCODE 0x%08X" % struct.unpack_from("<I", idcode, 2)[0], file=out)
    failures = 0
    try:
        for mode_name in args.modes:
            verdict, pc, cause = run_mode(edbg, args.number, args.address, mode_name, args.timeout)
            failures += verdict != PASS
            print(report_line(mode_name, verdict, pc, cause), file=out)
    finally:
        edbg.close()
    return failures


def open_link(vid, pid, interface):
    try:
        import usb.core
        import usb.util
    except ImportError:
        sys.exit("needs pyusb: pip install pyusb")
    device = usb.core.find(idVendor=vid, idProduct=pid)
    if device is None:
        sys.exit("no probe with VID:PID %04x:%04x" % (vid, pid))
    try:
        if device.is_kernel_driver_active(interface):
            device.detach_kernel_driver(interface)
    except (NotImplementedError, usb.core.USBError):
        pass
    usb.util.claim_interface(device, interface)
    intf = device.get_active_configuration()[(interface, 0)]
    out_ep = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    in_ep = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

    def xfer(report):
        out_ep.write(bytes(report).ljust(REPORT_SIZE, b"\0"), TIMEOUT_MS)
        return bytes(in_ep.read(REPORT_SIZE, TIMEOUT_MS))

    return xfer


def parse_modes(text):
    modes = [m.strip() for m in text.split(",") if m.strip()]
    for mode in modes:
        if mode not in MODES:
            raise argparse.ArgumentTypeError("unknown mode %r (read, write, access)" % mode)
    return modes


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--address", type=lambda t: int(t, 0), required=True, help="data space address")
    parser.add_argument("--modes", type=parse_modes, default=list(MODES), help="read,write,access (default all)")
    parser.add_argument("--number", type=int, choices=(2, 3), default=2, help="breakpoint number (default 2)")
    parser.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for each break (default 3)")
    parser.add_argument("--clock-khz", type=int, default=200, help="TCK in kHz (default 200)")
    parser.add_argument("--ocd-revision", type=int, default=3, help="OCD revision (default 3, ATmega32U4)")
    parser.add_argument("--medbg", action="store_true", help="the medbg build (03eb:2145, interface 0)")
    args = parser.parse_args(argv)
    if not 0 <= args.address <= 0xFFFF:
        parser.error("--address must be below 0x10000")
    return args


def main(argv=None):
    args = parse_args(argv)
    vid, pid = MEDBG_ID if args.medbg else DEFAULT_ID
    edbg = Edbg(open_link(vid, pid, MEDBG_INTERFACE if args.medbg else DEFAULT_INTERFACE))
    try:
        failures = run(edbg, args)
    except EdbgError as error:
        sys.exit("error: %s" % error)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
