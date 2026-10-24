#!/usr/bin/env python3
"""Shows and switches the USB personality of a multi image (VSL_PERSONALITIES),
and starts the bootloader: versaloon-bootloader on the STM32F1 boards, the
ROM USB DFU on the STM32H5 boards.

The script finds the dongle by its VID:PID and sends the switch request on
EP0, the control endpoint (bmRequestType 0x40, bRequest 0x0E, wValue code,
wIndex 0, wLength 0). Each personality of a multi image accepts it.

Firmware from before the EP0 request stalls it. Then the script sends the
switch command of the active personality on interface 0:

    Versaloon personality (0483:a038):  SWITCH_PERSONALITY [0x0E][code]
                                        reply [status], 0 = accepted
    CMSIS-DAP personality (0403:9b3c):  ID_DAP_Vendor0 [0x80][code]
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
knows. The script then waits for versaloon-bootloader (0483:5740, product
"Bootloader") or for the ROM DFU of the STM32H5 (0483:df11). See
docs/firmware-update.md, "Bootloader code".

Usage:
    python3 vsl_personality.py show
    python3 vsl_personality.py switch cmsis-dap
    python3 vsl_personality.py switch wch-link
    python3 vsl_personality.py switch versaloon --serial 0123456789AB
    python3 vsl_personality.py switch bootloader

Needs pyusb (and libusb). On Windows, interface 0 needs the WinUSB driver.
The unit tests in test_vsl_personality.py need no pyusb and no hardware.
"""

import argparse
import errno
import sys
import time

# Personality name -> (code, ((VID, PID), ...)). "bootloader" is not a
# personality: its code starts versaloon-bootloader (0483:5740) or the ROM
# DFU of the STM32H5 (0483:df11), and it has no switch command.
BOOTLOADER = "bootloader"
PERSONALITIES = {
    "versaloon": (0, ((0x0483, 0xA038),)),
    "cmsis-dap": (1, ((0x0403, 0x9B3C),)),
    "wch-link": (2, ((0x1209, 0x0001), (0x1A86, 0x8010))),
    BOOTLOADER: (0xB0, ((0x0483, 0x5740), (0x0483, 0xDF11))),
}

# Other devices use these USB IDs too (the pid.codes test ID, the real
# WCH-LinkE, the ST virtual COM port), so the product string must also match.
PRODUCT_STRINGS = {
    (0x1209, 0x0001): "Versaloon WCH-Link",
    (0x1A86, 0x8010): "Versaloon WCH-Link",
    (0x0483, 0x5740): "Bootloader",
}

# The ROM DFU has its own serial number, not the chip UID of the firmware,
# so the serial check skips it. Limit: with more than one STM32H5 dongle in
# DFU mode, --serial cannot select one.
NO_SERIAL_IDS = {(0x0483, 0xDF11)}

SWITCH_REQUEST_TYPE = 0x40  # vendor, host to device, recipient device
SWITCH_REQUEST = 0x0E
OUT_ENDPOINT = 0x03
IN_ENDPOINT = 0x82
SWITCH_PERSONALITY = 0x0E
FW_UPDATE = 0x0F
FW_UPDATE_KEY = 0xAA
DAP_VENDOR0 = 0x80
DAP_INVALID = 0xFF
LIBUSB_ERROR_PIPE = -9
# libusb errors after which the request can have gone out: IO, NO_DEVICE,
# TIMEOUT. The dongle can disconnect before the status stage ends.
IN_PROGRESS_ERRORS = {
    -1: errno.EIO,
    -4: errno.ENODEV,
    -7: errno.ETIMEDOUT,
}
TIMEOUT_MS = 1000
ENUMERATION_TIMEOUT_S = 10.0


class PersonalityError(Exception):
    pass


def personality_of(vid, pid):
    """The personality name of a VID:PID, or None."""
    for name, (_code, ids) in PERSONALITIES.items():
        if (vid, pid) in ids:
            return name
    return None


def switch_request(active, target):
    """The in-protocol command that switches from the active personality to target."""
    code = PERSONALITIES[target][0]
    if active == "versaloon":
        if target == BOOTLOADER:
            return bytes([FW_UPDATE, FW_UPDATE_KEY])
        return bytes([SWITCH_PERSONALITY, code])
    if active == "cmsis-dap":
        return bytes([DAP_VENDOR0, code])
    raise PersonalityError("the %s personality has no switch command" % active)


def refusal(target):
    """The reason why the firmware refuses a switch to target."""
    if target == BOOTLOADER:
        return "the image has no bootloader entry (only the app-8k image and the STM32H5 images have one)"
    return "the image has one personality, or it does not have the %s personality" % target


def check_reply(active, reply, target=None):
    """Raises PersonalityError when the reply does not accept the switch."""
    reply = bytes(reply)
    if active == "versaloon":
        if len(reply) != 1:
            raise PersonalityError("unexpected reply %s" % reply.hex())
        if reply[0] != 0:
            raise PersonalityError("the firmware refused the switch: %s" % refusal(target))
        return
    if reply[:1] == bytes([DAP_INVALID]):
        # Only an image with one personality and no bootloader entry
        raise PersonalityError("the firmware does not know ID_DAP_Vendor0: the image has one personality and no bootloader entry")
    if len(reply) < 2 or reply[0] != DAP_VENDOR0:
        raise PersonalityError("unexpected response %s" % reply.hex())
    if reply[1] != 0:
        raise PersonalityError("the firmware refused the switch (status %d): %s" % (reply[1], refusal(target)))


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


def find_devices(usb, serial=None):
    """(personality, device) for each dongle, optionally with one serial number."""
    result = []
    for name, (_code, ids) in PERSONALITIES.items():
        for vid, pid in ids:
            for device in usb.core.find(find_all=True, idVendor=vid, idProduct=pid):
                product = PRODUCT_STRINGS.get((vid, pid))
                if product is not None and get_string(usb, device, device.iProduct) != product:
                    continue
                if serial is not None and (vid, pid) not in NO_SERIAL_IDS and get_string(usb, device, device.iSerialNumber) != serial:
                    continue
                result.append((name, device))
    return result


def wait_for(usb, target, serial, timeout_s):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if any(name == target for name, _ in find_devices(usb, serial)):
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


def send_command(usb, device, active, target):
    """Sends the in-protocol switch command on interface 0.

    Raises PersonalityError when the firmware refuses the switch, and
    usb.core.USBError when the command does not go out.
    """
    request = switch_request(active, target)
    try:
        if device.is_kernel_driver_active(0):
            device.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    try:
        usb.util.claim_interface(device, 0)
        device.write(OUT_ENDPOINT, request, TIMEOUT_MS)
        try:
            check_reply(active, device.read(IN_ENDPOINT, 64, TIMEOUT_MS), target)
        except usb.core.USBError as error:
            # The command went out, so the switch can be in progress: wait
            # for the target device.
            print("No reply (%s), waiting for the device" % error, file=sys.stderr)
    finally:
        try:
            usb.util.release_interface(device, 0)
        except usb.core.USBError:
            pass


def command_show(usb, args):
    devices = find_devices(usb, args.serial)
    if not devices:
        print("No dongle found")
        return 1
    for name, device in devices:
        serial = get_string(usb, device, device.iSerialNumber)
        print("%s  %04x:%04x  serial %s" % (name, device.idVendor, device.idProduct, serial))
    return 0


def command_switch(usb, args):
    devices = find_devices(usb, args.serial)
    if not devices:
        print("No dongle found", file=sys.stderr)
        return 1
    if len(devices) > 1:
        print("More than one dongle: select one with --serial", file=sys.stderr)
        return 1
    active, device = devices[0]
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
            # A stall: the firmware refused the switch, or it is from before
            # the EP0 request. The in-protocol command tells which one.
            send_command(usb, device, active, args.personality)
    except PersonalityError as error:
        print(error, file=sys.stderr)
        return 1
    except (usb.core.USBError, NotImplementedError) as error:
        print("Cannot send the request: %s" % error, file=sys.stderr)
        return 1
    finally:
        usb.util.dispose_resources(device)
    print("Switching from %s to %s ..." % (active, args.personality))
    if not wait_for(usb, args.personality, serial, ENUMERATION_TIMEOUT_S):
        print("The dongle did not enumerate as %s" % args.personality, file=sys.stderr)
        if args.personality == BOOTLOADER:
            # The tool reads the product string and the serial number, which needs access to the device
            print("If the bootloader runs, check the access to 0483:5740: etc/70-versaloon-bootloader.rules", file=sys.stderr)
        return 1
    if args.personality == BOOTLOADER:
        print("The dongle runs the bootloader")
    else:
        print("The dongle has the %s personality" % args.personality)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", help="the serial number of the dongle (the chip UID)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("show", help="list the dongles and their personality")
    switch = commands.add_parser("switch", help="switch to a personality")
    switch.add_argument("personality", choices=sorted(PERSONALITIES))
    args = parser.parse_args(argv)
    usb = import_usb()
    if args.command == "show":
        return command_show(usb, args)
    return command_switch(usb, args)


if __name__ == "__main__":
    sys.exit(main())
