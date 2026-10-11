#!/usr/bin/env python3
"""Shows and switches the USB personality of a dual image (VSL_PROTOCOL=dual).

The script finds the dongle by its VID:PID and sends the switch command on
interface 0:

    Versaloon personality (0483:a038):  SWITCH_PERSONALITY [0x0E][code]
                                        reply [status], 0 = accepted
    CMSIS-DAP personality (0403:9b3c):  ID_DAP_Vendor0 [0x80][code]
                                        response [0x80][status], 0 = accepted

The personality code is 0 for Versaloon and 1 for CMSIS-DAP. After the
reply, the dongle resets and enumerates as the other device. The script
then waits for the other device. See docs/usb-device.md, "Dual image".

Usage:
    python3 vsl_personality.py show
    python3 vsl_personality.py switch cmsis-dap
    python3 vsl_personality.py switch versaloon --serial 0123456789AB

Needs pyusb (and libusb). On Windows, interface 0 needs the WinUSB driver.
The unit tests in test_vsl_personality.py need no pyusb and no hardware.
"""

import argparse
import sys
import time

# Personality name -> (code, VID, PID)
PERSONALITIES = {
    "versaloon": (0, 0x0483, 0xA038),
    "cmsis-dap": (1, 0x0403, 0x9B3C),
}

OUT_ENDPOINT = 0x03
IN_ENDPOINT = 0x82
SWITCH_PERSONALITY = 0x0E
DAP_VENDOR0 = 0x80
DAP_INVALID = 0xFF
TIMEOUT_MS = 1000
ENUMERATION_TIMEOUT_S = 10.0


class PersonalityError(Exception):
    pass


def personality_of(vid, pid):
    """The personality name of a VID:PID, or None."""
    for name, (_code, p_vid, p_pid) in PERSONALITIES.items():
        if (vid, pid) == (p_vid, p_pid):
            return name
    return None


def switch_request(active, target):
    """The command that switches from the active personality to target."""
    code = PERSONALITIES[target][0]
    if active == "versaloon":
        return bytes([SWITCH_PERSONALITY, code])
    return bytes([DAP_VENDOR0, code])


def check_reply(active, reply):
    """Raises PersonalityError when the reply does not accept the switch."""
    reply = bytes(reply)
    if active == "versaloon":
        if len(reply) != 1:
            raise PersonalityError("unexpected reply %s" % reply.hex())
        if reply[0] != 0:
            raise PersonalityError("the firmware refused the switch: the image has one personality")
        return
    if reply[:1] == bytes([DAP_INVALID]):
        raise PersonalityError("the firmware does not know ID_DAP_Vendor0: the image has one personality")
    if len(reply) < 2 or reply[0] != DAP_VENDOR0:
        raise PersonalityError("unexpected response %s" % reply.hex())
    if reply[1] != 0:
        raise PersonalityError("the firmware refused the switch (status %d)" % reply[1])


def import_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        sys.exit("pyusb is missing: install the pyusb package")
    return usb


def find_devices(usb, serial=None):
    """(personality, device) for each dongle, optionally with one serial number."""
    result = []
    for name, (_code, vid, pid) in PERSONALITIES.items():
        for device in usb.core.find(find_all=True, idVendor=vid, idProduct=pid):
            if serial is not None and usb.util.get_string(device, device.iSerialNumber) != serial:
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


def command_show(usb, args):
    devices = find_devices(usb, args.serial)
    if not devices:
        print("No dongle found")
        return 1
    for name, device in devices:
        serial = usb.util.get_string(device, device.iSerialNumber)
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
    serial = usb.util.get_string(device, device.iSerialNumber)
    try:
        if device.is_kernel_driver_active(0):
            device.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    usb.util.claim_interface(device, 0)
    try:
        device.write(OUT_ENDPOINT, switch_request(active, args.personality), TIMEOUT_MS)
        check_reply(active, device.read(IN_ENDPOINT, 64, TIMEOUT_MS))
    except PersonalityError as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        usb.util.release_interface(device, 0)
        usb.util.dispose_resources(device)
    print("Switching from %s to %s ..." % (active, args.personality))
    if not wait_for(usb, args.personality, serial, ENUMERATION_TIMEOUT_S):
        print("The dongle did not enumerate as %s" % args.personality, file=sys.stderr)
        return 1
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
