#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host control of the probe: the USB personality of a multi image
(MUPP_PERSONALITIES), the bootloader, the RISC-V bridge and the target power.

"switch" switches the personality or starts the bootloader:
openmupp-bootloader on the STM32F1 boards, the ROM USB DFU on the STM32H5
boards. The script finds the dongle by its VID:PID and sends the switch request on
EP0, the control endpoint (bmRequestType 0x40, bRequest 0x0E, wValue code,
wIndex 0, wLength 0). Each personality of a multi image accepts it.

Firmware from before the EP0 request stalls it. Then the script sends the
switch command of the active personality on interface 0:

    Versaloon personality (0483:a038):  SWITCH_PERSONALITY [0x0E][code]
                                        reply [status], 0 = accepted
    CMSIS-DAP personality (1209:0001):  ID_DAP_Vendor0 [0x80][code]
                                        response [0x80][status], 0 = accepted

The WCH-Link personality (1209:0001 or 1a86:8010) has no switch command, so
a stall there means that the firmware refused the switch.

The personality code is 0 for Versaloon, 1 for CMSIS-DAP and 2 for
WCH-Link. After the request, the dongle resets and enumerates as the target
device. The script then waits for the target device. See docs/usb-device.md,
"Multi image".

"switch bootloader" sends the bootloader code 0xB0 instead of a personality
code. Each personality of an app-8k image or of an STM32H5 image accepts it,
also in an image with one personality. After a stall, the Versaloon
personality gets VERSALOON_FW_UPDATE [0x0F][0xAA], which older firmware also
knows. The script then waits for openmupp-bootloader (0483:5740, product
"Bootloader") or for the ROM DFU of the STM32H5 (0483:df11). See
docs/firmware-update.md, "Bootloader code".

"bridge" sets the mode of the RISC-V bridge of the CMSIS-DAP personality. An
image with MUPP_CMSIS_DAP_RV_BRIDGE (the default) has the bridge. The script
sends the vendor command ID_DAP_Vendor1 on interface 0:

    [0x81][0] off, [0x81][1] on, [0x81][2] auto, [0x81][3] the stored mode
    -> response [0x81][status], 0 = done

With --store, bit 7 of the mode is set ([0x81][0x82] for auto). The firmware
then also stores the mode in its settings page: it is the mode at power-up
and after a USB reset.

An image without the bridge replies ID_DAP_Invalid (0xFF). Auto is the stored
mode until a --store: the probe finds the target at each DAP_Connect to JTAG,
so probe-rs needs no command. Use "on" or "off" to force the port. Without
--store, the mode stays until a USB reset, a power cycle or the next command.
OpenOCD sends the command itself (cmsis-dap cmd 0x81 0x01). See
docs/cmsis-dap.md, "Bridge switch".

"power" switches the 3.3 V supply of the target and reads the target voltage.
The script sends the vendor command ID_DAP_Vendor2 of the CMSIS-DAP personality
on interface 0:

    [0x82][0] off, [0x82][1] on, [0x82][2] only read the voltage
    -> response [0x82][status][mV u16], 0 = done, 1 = refused,
       0xFF = the board has no power switch

The firmware refuses "on" when the target has its own supply, or when the
rail has a short circuit or an overload. After "off", the rail can stay above
1.8 V for some time and "on" is refused too: wait until "status" shows less.
The supply stays on after the script ends. See docs/cmsis-dap.md,
"Target power".

Usage:
    python3 mupp_probe.py show
    python3 mupp_probe.py switch cmsis-dap
    python3 mupp_probe.py switch wch-link
    python3 mupp_probe.py switch versaloon --serial 0123456789AB
    python3 mupp_probe.py switch bootloader
    python3 mupp_probe.py --id 1209:0001 show
    python3 mupp_probe.py --id 1234:5678 --active wch-link switch versaloon
    python3 mupp_probe.py bridge on
    python3 mupp_probe.py bridge auto --serial 0123456789AB
    python3 mupp_probe.py bridge off --store
    python3 mupp_probe.py power on
    python3 mupp_probe.py power off --serial 0123456789AB
    python3 mupp_probe.py power status

A device with a known ID, but with strings that the tool cannot read, is
reported with its bus and address. On Linux this is a missing udev rule, and
--id does not help: the tool cannot open the device either.
--id VID:PID selects the dongle by this ID, without the product string check,
for a dongle with an ID or a product string that the tool does not know.
An ID that the tool does not know, or that two personalities share (1209:0001),
has an unknown personality: --active names
it. Without it, the EP0 request still works, but not the fallback command,
and "bridge" and "power" do not find the dongle.
The wait after a switch looks for the standard IDs of the target.

Needs pyusb (and libusb). On Windows, interface 0 needs the WinUSB driver.
The unit tests in test_mupp_probe.py need no pyusb and no hardware.
"""

import argparse
import contextlib
import errno
import sys
import time

# Personality name -> (code, ((VID, PID), ...)). "bootloader" is not a
# personality: its code starts openmupp-bootloader (0483:5740) or the ROM
# DFU of the STM32H5 (0483:df11), and it has no switch command.
BOOTLOADER = "bootloader"
PERSONALITIES = {
    "versaloon": (0, ((0x0483, 0xA038),)),
    # 0403:9b3c: firmware before the pid.codes ID
    "cmsis-dap": (1, ((0x1209, 0x0001), (0x0403, 0x9B3C))),
    "wch-link": (2, ((0x1209, 0x0001), (0x1A86, 0x8010))),
    BOOTLOADER: (0xB0, ((0x0483, 0x5740), (0x0483, 0xDF11))),
}

# Other devices use these USB IDs too (the pid.codes test ID, the real
# WCH-LinkE, the ST virtual COM port), and the cmsis-dap and wch-link
# personalities share the pid.codes test ID, so the product string must also
# match. Firmware before the OpenMUPP rename has the product "Versaloon
# WCH-Link". The key is (personality, VID, PID).
PRODUCT_STRINGS = {
    ("cmsis-dap", 0x1209, 0x0001): ("OpenMUPP CMSIS-DAP",),
    ("wch-link", 0x1209, 0x0001): ("OpenMUPP WCH-Link", "Versaloon WCH-Link"),
    ("wch-link", 0x1A86, 0x8010): ("OpenMUPP WCH-Link", "Versaloon WCH-Link", "WCH-Link"),
    (BOOTLOADER, 0x0483, 0x5740): ("Bootloader",),
}

# The WCH ID build has the product "WCH-Link" of the real WCH-LinkE, because
# probe-rs needs it. The manufacturer string tells the dongle from the real
# probe. The key is (personality, VID, PID, product).
MANUFACTURER_STRINGS = {
    ("wch-link", 0x1A86, 0x8010, "WCH-Link"): "OpenMUPP",
}

# The ROM DFU has its own serial number, not the chip UID of the firmware,
# so the wait for the bootloader skips the serial check for it. Limit: any
# STM32 in ROM DFU mode ends the wait, also another board.
NO_SERIAL_IDS = {(0x0483, 0xDF11)}

SWITCH_REQUEST_TYPE = 0x40  # vendor, host to device, recipient device
SWITCH_REQUEST = 0x0E
OUT_ENDPOINT = 0x03
IN_ENDPOINT = 0x82
SWITCH_PERSONALITY = 0x0E
FW_UPDATE = 0x0F
FW_UPDATE_KEY = 0xAA
DAP_VENDOR0 = 0x80
DAP_VENDOR1 = 0x81
DAP_VENDOR2 = 0x82
DAP_INVALID = 0xFF
# The modes of ID_DAP_Vendor1
BRIDGE_MODES = {"off": 0, "on": 1, "auto": 2, "stored": 3}
# This bit of the mode also stores the mode
BRIDGE_STORE_FLAG = 0x80
# The operations of ID_DAP_Vendor2
POWER_OPS = {"off": 0, "on": 1, "status": 2}
POWER_REFUSED = 1
POWER_NOT_SUPPORTED = 0xFF
LIBUSB_ERROR_PIPE = -9
LIBUSB_ERROR_ACCESS = -3
# libusb errors after which the request can have gone out: IO, NO_DEVICE,
# TIMEOUT. The dongle can disconnect before the status stage ends.
IN_PROGRESS_ERRORS = {
    -1: errno.EIO,
    -4: errno.ENODEV,
    -7: errno.ETIMEDOUT,
}
TIMEOUT_MS = 1000
ENUMERATION_TIMEOUT_S = 10.0


UNKNOWN = "unknown"


class ProbeError(Exception):
    pass


def personality_of(vid, pid):
    """The personality name of a VID:PID, or None when it is unknown or shared."""
    names = [name for name, (_code, ids) in PERSONALITIES.items() if (vid, pid) in ids]
    return names[0] if len(names) == 1 else None


def switch_request(active, target):
    """The in-protocol command that switches from the active personality to target."""
    code = PERSONALITIES[target][0]
    if active == "versaloon":
        if target == BOOTLOADER:
            return bytes([FW_UPDATE, FW_UPDATE_KEY])
        return bytes([SWITCH_PERSONALITY, code])
    if active == "cmsis-dap":
        return bytes([DAP_VENDOR0, code])
    raise ProbeError("the %s personality has no switch command" % active)


def refusal(target):
    """The reason why the firmware refuses a switch to target."""
    if target == BOOTLOADER:
        return "the image has no bootloader entry (only the app-8k image and the STM32H5 images have one)"
    return "the image has one personality, or it does not have the %s personality" % target


def check_reply(active, reply, target=None):
    """Raises ProbeError when the reply does not accept the switch."""
    reply = bytes(reply)
    if active == "versaloon":
        if len(reply) != 1:
            raise ProbeError("unexpected reply %s" % reply.hex())
        if reply[0] != 0:
            raise ProbeError("the firmware refused the switch: %s" % refusal(target))
        return
    if reply[:1] == bytes([DAP_INVALID]):
        # Only an image with one personality and no bootloader entry
        raise ProbeError("the firmware does not know ID_DAP_Vendor0: the image has one personality and no bootloader entry")
    if len(reply) < 2 or reply[0] != DAP_VENDOR0:
        raise ProbeError("unexpected response %s" % reply.hex())
    if reply[1] != 0:
        raise ProbeError("the firmware refused the switch (status %d): %s" % (reply[1], refusal(target)))


def bridge_request(mode, store=False):
    return bytes([DAP_VENDOR1, BRIDGE_MODES[mode] | (BRIDGE_STORE_FLAG if store else 0)])


def check_bridge_reply(reply):
    """Raises ProbeError when the response does not confirm the bridge mode."""
    reply = bytes(reply)
    if reply[:1] == bytes([DAP_INVALID]):
        raise ProbeError("the firmware does not know ID_DAP_Vendor1: the image has no RISC-V bridge (MUPP_CMSIS_DAP_RV_BRIDGE)")
    if len(reply) < 2 or reply[0] != DAP_VENDOR1:
        raise ProbeError("unexpected response %s" % reply.hex())
    if reply[1] != 0:
        raise ProbeError(
            "the firmware refused the mode (status %d): a mode that it does not know, or a failed store. A firmware without the stored mode refuses --store" % reply[1]
        )


def check_power_reply(reply):
    """Raises ProbeError when the response does not confirm the power operation."""
    reply = bytes(reply)
    if reply[:1] == bytes([DAP_INVALID]):
        raise ProbeError("the firmware does not know ID_DAP_Vendor2: update it to v0.13.0 or later")
    if len(reply) < 4 or reply[0] != DAP_VENDOR2:
        raise ProbeError("unexpected response %s" % reply.hex())
    millivolts = power_millivolts(reply)
    if reply[1] == POWER_REFUSED:
        raise ProbeError(
            "the firmware refused the switch-on, the target voltage is %d mV. "
            "Above 1.8 V: the target has its own supply, or the rail did not discharge yet. "
            "Below it: a short circuit, an overload or a failed measurement" % millivolts
        )
    if reply[1] == POWER_NOT_SUPPORTED:
        raise ProbeError("the firmware refused the operation: the board has no power switch")
    if reply[1] != 0:
        raise ProbeError("unexpected response %s" % reply.hex())


def power_millivolts(reply):
    """The target voltage of an ID_DAP_Vendor2 response. 0 without a Vref sense."""
    return int.from_bytes(bytes(reply)[2:4], "little")


def is_access_denied(error):
    """True when a USBError is a missing permission (EACCES)."""
    return getattr(error, "errno", None) == errno.EACCES or getattr(error, "backend_error_code", None) == LIBUSB_ERROR_ACCESS


def is_stall(error):
    """True when a USBError is a stall (EPIPE) of the device."""
    return error.errno == errno.EPIPE or getattr(error, "backend_error_code", None) == LIBUSB_ERROR_PIPE


def can_be_in_progress(error):
    """True when the request can have gone out after this USBError."""
    code = getattr(error, "backend_error_code", None)
    return code in IN_PROGRESS_ERRORS or error.errno in IN_PROGRESS_ERRORS.values()


def import_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        sys.exit("pyusb is missing: install the pyusb package")
    return usb


def get_string(usb, device, index):
    """A string descriptor of the device, or None when the host cannot read it."""
    try:
        return usb.util.get_string(device, index)
    except (usb.core.USBError, ValueError, NotImplementedError):
        return None


def find_devices(usb, serial=None, any_serial_ids=(), usb_id=None, unreadable=None):
    """(personality, device) for each dongle, optionally with one serial number.

    The devices with an ID in any_serial_ids match each serial number.
    usb_id = (VID, PID) selects the devices with this ID, without the product
    string check; the personality is "unknown" for an ID that is not known.
    The list unreadable gets each device whose product string the host cannot
    read: it can be a dongle without access.
    """
    if usb_id is None:
        candidates = [(name, vid, pid) for name, (_code, ids) in PERSONALITIES.items() for vid, pid in ids]
    else:
        candidates = [(personality_of(*usb_id) or UNKNOWN, *usb_id)]
    result = []
    for name, vid, pid in candidates:
        for device in usb.core.find(find_all=True, idVendor=vid, idProduct=pid):
            products = PRODUCT_STRINGS.get((name, vid, pid))
            if products is not None and usb_id is None:
                found = get_string(usb, device, device.iProduct)
                if found is None and unreadable is not None and device not in unreadable:
                    unreadable.append(device)
                if found not in products:
                    continue
                manufacturer = MANUFACTURER_STRINGS.get((name, vid, pid, found))
                if manufacturer is not None and get_string(usb, device, device.iManufacturer) != manufacturer:
                    continue
            if serial is not None and (vid, pid) not in any_serial_ids:
                found = get_string(usb, device, device.iSerialNumber)
                if found != serial:
                    if found is None and unreadable is not None and device not in unreadable:
                        unreadable.append(device)
                    continue
            result.append((name, device))
    return result


def wait_for(usb, target, serial, timeout_s):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        any_serial_ids = NO_SERIAL_IDS if target == BOOTLOADER else ()
        if any(name == target for name, _ in find_devices(usb, serial, any_serial_ids)):
            return True
        time.sleep(0.2)
    return False


def send_ep0_request(usb, device, target):
    """Sends the switch request on EP0.

    Returns True when the firmware accepted it, or when the result is not
    known and the switch can be in progress. Returns False on a stall.
    Raises usb.core.USBError when the request did not go out.
    """
    try:
        device.ctrl_transfer(SWITCH_REQUEST_TYPE, SWITCH_REQUEST, PERSONALITIES[target][0], 0, None, TIMEOUT_MS)
    except usb.core.USBError as error:
        if is_stall(error):
            return False
        if not can_be_in_progress(error):
            raise
        # The request can have gone out, so the switch can be in progress:
        # wait for the target device.
        print("No status stage (%s), waiting for the device" % error, file=sys.stderr)
    return True


@contextlib.contextmanager
def claimed_interface(usb, device):
    """Claims interface 0 for the commands in the with block."""
    try:
        if device.is_kernel_driver_active(0):
            device.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    try:
        usb.util.claim_interface(device, 0)
        yield
    finally:
        try:
            usb.util.release_interface(device, 0)
        except usb.core.USBError:
            pass


def send_command(usb, device, active, target):
    """Sends the in-protocol switch command on interface 0.

    Raises ProbeError when the firmware refuses the switch, and
    usb.core.USBError when the command does not go out.
    """
    request = switch_request(active, target)
    with claimed_interface(usb, device):
        device.write(OUT_ENDPOINT, request, TIMEOUT_MS)
        try:
            check_reply(active, device.read(IN_ENDPOINT, 64, TIMEOUT_MS), target)
        except usb.core.USBError as error:
            # The command went out, so the switch can be in progress: wait
            # for the target device.
            print("No reply (%s), waiting for the device" % error, file=sys.stderr)


def parse_usb_id(text):
    """(VID, PID) of a "VID:PID" text in hexadecimal."""
    try:
        vid, pid = (int(part, 16) for part in text.split(":"))
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a VID:PID in hexadecimal, for example 1209:0001" % text)
    if not (0 <= vid <= 0xFFFF and 0 <= pid <= 0xFFFF):
        raise argparse.ArgumentTypeError("%r is out of range" % text)
    return vid, pid


def report_unreadable(unreadable):
    """Tells about the devices with a known ID whose strings the host cannot read."""
    for device in unreadable:
        print(
            "%04x:%04x on bus %s address %s: cannot read its strings, so it is not checked. "
            "Without access, install the udev rules in etc/ (70-openmupp-*.rules)"
            % (device.idVendor, device.idProduct, device.bus, device.address),
            file=sys.stderr,
        )


def find_selected(usb, args):
    """The dongles for the command line. Reports the devices without access."""
    unreadable = []
    devices = find_devices(usb, args.serial, usb_id=args.id, unreadable=unreadable)
    report_unreadable(unreadable)
    return devices


def report_send_error(error):
    print("Cannot send the request: %s" % error, file=sys.stderr)
    if is_access_denied(error):
        print("No access to the device: install the udev rules in etc/ (70-openmupp-*.rules)", file=sys.stderr)


def find_cmsis_dap(usb, args):
    """The one selected dongle with the CMSIS-DAP personality, or None after a message."""
    found = find_selected(usb, args)
    devices = [device for name, device in found if name == "cmsis-dap" or (name == UNKNOWN and args.active == "cmsis-dap")]
    if not devices:
        if args.active is None and any(name == UNKNOWN for name, _ in found):
            print("The personality of this ID is unknown: select it with --active", file=sys.stderr)
            return None
        print("No dongle with the CMSIS-DAP personality found", file=sys.stderr)
        return None
    if len(devices) > 1:
        print("More than one dongle: select one with --serial", file=sys.stderr)
        return None
    return devices[0]


def run_dap_command(usb, args, request, check):
    """Sends a CMSIS-DAP vendor command to the selected dongle.

    Returns the response after check(response) accepts it, or None after a
    message.
    """
    device = find_cmsis_dap(usb, args)
    if device is None:
        return None
    try:
        with claimed_interface(usb, device):
            device.write(OUT_ENDPOINT, request, TIMEOUT_MS)
            try:
                reply = device.read(IN_ENDPOINT, 64, TIMEOUT_MS)
            except usb.core.USBError as error:
                raise ProbeError("No response (%s): the command can have run, so the state is not known" % error)
        check(reply)
        return reply
    except ProbeError as error:
        print(error, file=sys.stderr)
        return None
    except (usb.core.USBError, NotImplementedError) as error:
        report_send_error(error)
        return None
    finally:
        usb.util.dispose_resources(device)


def command_show(usb, args):
    devices = find_selected(usb, args)
    if not devices:
        print("No dongle found")
        return 1
    for name, device in devices:
        serial = get_string(usb, device, device.iSerialNumber)
        print("%s  %04x:%04x  serial %s" % (name, device.idVendor, device.idProduct, serial))
    return 0


def command_switch(usb, args):
    devices = find_selected(usb, args)
    if not devices:
        print("No dongle found", file=sys.stderr)
        return 1
    if len(devices) > 1:
        print("More than one dongle: select one with --serial", file=sys.stderr)
        return 1
    active, device = devices[0]
    if active == UNKNOWN and args.active is not None:
        active = args.active
    if active == args.personality:
        print("The dongle has the %s personality already" % active)
        return 0
    if active == BOOTLOADER:
        print("The dongle runs the bootloader: reset it to start the firmware", file=sys.stderr)
        return 1
    serial = get_string(usb, device, device.iSerialNumber)
    try:
        if not send_ep0_request(usb, device, args.personality):
            if active == "wch-link":
                print("The firmware refused the switch: %s" % refusal(args.personality), file=sys.stderr)
                return 1
            if active == UNKNOWN:
                print("The request stalled and the personality of this ID is unknown: select it with --active", file=sys.stderr)
                return 1
            # A stall: the firmware refused the switch, or it is from before
            # the EP0 request. The in-protocol command tells which one.
            send_command(usb, device, active, args.personality)
    except ProbeError as error:
        print(error, file=sys.stderr)
        return 1
    except (usb.core.USBError, NotImplementedError) as error:
        report_send_error(error)
        return 1
    finally:
        usb.util.dispose_resources(device)
    print("Switching from %s to %s ..." % (active, args.personality))
    if not wait_for(usb, args.personality, serial, ENUMERATION_TIMEOUT_S):
        print("The dongle did not enumerate as %s" % args.personality, file=sys.stderr)
        unreadable = []
        find_devices(usb, serial, unreadable=unreadable)
        report_unreadable(unreadable)
        if args.personality == BOOTLOADER:
            # The tool reads the product string and the serial number, which needs access to the device
            print("If the bootloader runs, check the access to 0483:5740: etc/70-openmupp-bootloader.rules", file=sys.stderr)
        return 1
    if args.personality == BOOTLOADER:
        print("The dongle runs the bootloader")
    else:
        print("The dongle has the %s personality" % args.personality)
    return 0


def command_bridge(usb, args):
    if run_dap_command(usb, args, bridge_request(args.mode, args.store), check_bridge_reply) is None:
        return 1
    print("The RISC-V bridge is %s%s" % (args.mode, ", stored" if args.store else ""))
    return 0


def command_power(usb, args):
    reply = run_dap_command(usb, args, bytes([DAP_VENDOR2, POWER_OPS[args.operation]]), check_power_reply)
    if reply is None:
        return 1
    if args.operation != "status":
        print("The target power is %s" % args.operation)
    print("Target voltage: %d mV" % power_millivolts(reply))
    return 0


def add_selection_options(parser, default):
    parser.add_argument("--serial", default=default, help="the serial number of the dongle (the chip UID)")
    parser.add_argument("--id", default=default, type=parse_usb_id, metavar="VID:PID", help="select the dongle by this USB ID (hexadecimal), without the product string check")
    parser.add_argument("--active", default=default, choices=sorted(set(PERSONALITIES) - {BOOTLOADER}), help="the personality of a dongle with an unknown --id")


def main(argv=None, usb=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_selection_options(parser, None)
    commands = parser.add_subparsers(dest="command", required=True)
    show = commands.add_parser("show", help="list the dongles and their personality")
    switch = commands.add_parser("switch", help="switch to a personality")
    switch.add_argument("personality", choices=sorted(PERSONALITIES))
    bridge = commands.add_parser("bridge", help="set the mode of the RISC-V bridge (CMSIS-DAP personality)")
    bridge.add_argument("mode", choices=tuple(BRIDGE_MODES))
    bridge.add_argument("--store", action="store_true", help="also store the mode: the mode at power-up and after a USB reset")
    power = commands.add_parser("power", help="switch the 3.3 V target supply or read the target voltage (CMSIS-DAP personality)")
    power.add_argument("operation", choices=tuple(POWER_OPS))
    # The options also work after the command. SUPPRESS keeps a value from
    # before the command.
    for subparser in (show, switch, bridge, power):
        add_selection_options(subparser, argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.active is not None and (args.id is None or personality_of(*args.id) is not None):
        parser.error("--active needs --id with an ID that is unknown or shared")
    if args.command == "bridge" and args.store and args.mode == "stored":
        parser.error("the stored mode is stored already: use --store with off, on or auto")
    if usb is None:
        usb = import_usb()
    return {"show": command_show, "switch": command_switch, "bridge": command_bridge, "power": command_power}[args.command](usb, args)


if __name__ == "__main__":
    sys.exit(main())
