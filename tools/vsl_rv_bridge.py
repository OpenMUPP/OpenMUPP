#!/usr/bin/env python3
"""Sets the mode of the RISC-V bridge of the CMSIS-DAP personality.

An image with VSL_CMSIS_DAP_RV_BRIDGE (the default) has the bridge. The
script sends the vendor command ID_DAP_Vendor1 on interface 0:

    [0x81][0] off, [0x81][1] on, [0x81][2] auto
    -> response [0x81][status], 0 = done

An image without the bridge replies ID_DAP_Invalid (0xFF). Auto is the mode
after a USB reset and a power cycle: the probe finds the target at each
DAP_Connect to JTAG, so probe-rs needs no command. Use "on" or "off" to
force the port. The mode stays until a USB reset, a power cycle or the next
command. OpenOCD sends the command itself (cmsis-dap cmd 0x81 0x01). See
docs/cmsis-dap.md, "Bridge switch".

Usage:
    python3 vsl_rv_bridge.py on
    python3 vsl_rv_bridge.py auto --serial 0123456789AB

Needs pyusb (and libusb). On Windows, interface 0 needs the WinUSB driver.
The unit tests in test_vsl_rv_bridge.py need no pyusb and no hardware.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vsl_personality as vp  # noqa: E402

DAP_VENDOR1 = 0x81
# The modes of ID_DAP_Vendor1
MODES = {"off": 0, "on": 1, "auto": 2}


class BridgeError(Exception):
    pass


def bridge_request(mode):
    return bytes([DAP_VENDOR1, MODES[mode]])


def check_reply(reply):
    """Raises BridgeError when the response does not confirm the switch."""
    reply = bytes(reply)
    if reply[:1] == bytes([vp.DAP_INVALID]):
        raise BridgeError("the firmware does not know ID_DAP_Vendor1: the image has no RISC-V bridge (VSL_CMSIS_DAP_RV_BRIDGE)")
    if len(reply) < 2 or reply[0] != DAP_VENDOR1:
        raise BridgeError("unexpected response %s" % reply.hex())
    if reply[1] != 0:
        raise BridgeError("the firmware refused the mode (status %d)" % reply[1])


def send(usb, device, mode):
    """Sends the command on interface 0 and checks the response.

    Raises BridgeError for a bad response, and usb.core.USBError when the
    command does not go out or no response comes.
    """
    try:
        if device.is_kernel_driver_active(0):
            device.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    try:
        usb.util.claim_interface(device, 0)
        device.write(vp.OUT_ENDPOINT, bridge_request(mode), vp.TIMEOUT_MS)
        check_reply(device.read(vp.IN_ENDPOINT, 64, vp.TIMEOUT_MS))
    finally:
        try:
            usb.util.release_interface(device, 0)
        except usb.core.USBError:
            pass


def main(argv=None, usb=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", help="the serial number of the dongle (the chip UID)")
    parser.add_argument("mode", choices=tuple(MODES))
    args = parser.parse_args(argv)
    if usb is None:
        usb = vp.import_usb()
    devices = [device for name, device in vp.find_devices(usb, args.serial) if name == "cmsis-dap"]
    if not devices:
        print("No dongle with the CMSIS-DAP personality found", file=sys.stderr)
        return 1
    if len(devices) > 1:
        print("More than one dongle: select one with --serial", file=sys.stderr)
        return 1
    device = devices[0]
    try:
        send(usb, device, args.mode)
    except BridgeError as error:
        print(error, file=sys.stderr)
        return 1
    except (usb.core.USBError, NotImplementedError) as error:
        print("Cannot send the command: %s" % error, file=sys.stderr)
        return 1
    finally:
        usb.util.dispose_resources(device)
    print("The RISC-V bridge is %s" % args.mode)
    return 0


if __name__ == "__main__":
    sys.exit(main())
