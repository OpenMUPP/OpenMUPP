#!/usr/bin/env python3
"""Loopback test for the DAP_UART_* commands of the CMSIS-DAP personality.

Connect the UART TX pin to the UART RX pin of the dongle (PA9 to PA10 on the
Mini). The script takes the UART from the CDC-ACM serial
port with DAP_UART_Transport, and sends a byte pattern at each frame format
with DAP_UART_Transfer. It checks that the same bytes come back unchanged
and in order, and that the status reports no lost data and no framing or
parity error. At the end it gives the UART back to the serial port.

Each test also checks the owner switch: while the DAP commands own the UART,
the serial port gets no data (use --serial-port to check this).

The script talks to interface 0 (CMSIS-DAP v2, bulk endpoints 0x03 and
0x82) of the dongle with VID:PID 1209:0001. See docs/cmsis-dap.md, "UART
commands".

Usage:
    python3 dap_uart_loopback_test.py
    python3 dap_uart_loopback_test.py --bauds 115200 --formats 8N1 --repeat 100
    python3 dap_uart_loopback_test.py --serial 0123456789AB --serial-port /dev/ttyACM0

Needs pyusb (and libusb), and pyserial for --serial-port. On Windows,
interface 0 uses WinUSB without a driver installation. The unit tests in
test_dap_uart_loopback.py need no pyusb and no hardware.
"""

import argparse
import struct
import sys
import time

VID, PID = 0x1209, 0x0001
INTERFACE = 0
OUT_ENDPOINT = 0x03
IN_ENDPOINT = 0x82
PACKET_SIZE = 64
TIMEOUT_MS = 1000

DAP_INFO = 0x00
DAP_UART_TRANSPORT = 0x1F
DAP_UART_CONFIGURE = 0x20
DAP_UART_TRANSFER = 0x21
DAP_UART_CONTROL = 0x22
DAP_UART_STATUS = 0x23
DAP_INVALID = 0xFF

INFO_CAPABILITIES = 0xF0
INFO_UART_RX_BUFFER_SIZE = 0xFB
INFO_UART_TX_BUFFER_SIZE = 0xFC
CAPABILITY_UART = 0x80

TRANSPORT_NONE, TRANSPORT_COM_PORT, TRANSPORT_DAP = 0, 1, 2

RX_ENABLE, RX_DISABLE, RX_FLUSH = 0x01, 0x02, 0x04
TX_ENABLE, TX_DISABLE, TX_FLUSH = 0x10, 0x20, 0x40

STATUS_RX_DATA_LOST = 0x02
STATUS_FRAMING_ERROR = 0x04
STATUS_PARITY_ERROR = 0x08
STATUS_ERRORS = STATUS_RX_DATA_LOST | STATUS_FRAMING_ERROR | STATUS_PARITY_ERROR

# The most transmit data in one DAP_UART_Transfer request, and the most
# receive data in one response
MAX_TX = PACKET_SIZE - 5
MAX_RX = PACKET_SIZE - 6

DEFAULT_BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000]
# The formats of the UART: data bits, parity, stop bits
DEFAULT_FORMATS = ["8N1", "8E1", "8O1", "7E1", "7O1", "8N2", "8N1.5"]
# The time for the echo of the last byte to come back
SETTLE_TIMEOUT_S = 2.0

PASS, FAIL, ERROR = "PASS", "FAIL", "ERROR"


class DapError(Exception):
    pass


def parse_format(text):
    """'8E1' -> (8, 'E', 1). Raises ValueError for a format that is not valid."""
    if len(text) < 3 or text[0] not in "78" or text[1].upper() not in "NEO" or text[2:] not in ("1", "1.5", "2"):
        raise ValueError("invalid format %r (for example 8N1, 7E1, 8O2 or 8N1.5)" % text)
    stop_bits = float(text[2:]) if text[2:] == "1.5" else int(text[2:])
    return int(text[0]), text[1].upper(), stop_bits


def control_byte(data_bits, parity, stop_bits):
    """The control byte of DAP_UART_Configure (CMSIS-Driver USART values)."""
    data_field = 0 if data_bits == 8 else data_bits
    parity_field = {"N": 0, "E": 1, "O": 2}[parity]
    stop_field = {1: 0, 2: 1, 1.5: 2}[stop_bits]
    return data_field | parity_field << 4 | stop_field << 6


def pattern(size, data_bits=8, shift=0):
    """Bytes that count up. With 7 data bits the top bit stays clear."""
    mask = 0xFF if data_bits == 8 else 0x7F
    return bytes(((index + shift) & 0xFF) & mask for index in range(size))


class DapUart:
    """The DAP_UART_* commands on a link: a function from a request packet
    to its response packet."""

    def __init__(self, link):
        self.link = link

    def command(self, request):
        response = bytes(self.link(bytes(request)))
        if not response or response[0] != request[0]:
            if response[:1] == bytes([DAP_INVALID]):
                raise DapError("the firmware does not know command 0x%02x" % request[0])
            raise DapError("unexpected response %s to command 0x%02x" % (response.hex(), request[0]))
        return response

    def info(self, info_id):
        response = self.command([DAP_INFO, info_id])
        length = response[1] if len(response) > 1 else 0
        return response[2:2 + length]

    def has_uart(self):
        capabilities = self.info(INFO_CAPABILITIES)
        return bool(capabilities) and (capabilities[0] & CAPABILITY_UART) != 0

    def buffer_sizes(self):
        """(rx, tx) buffer sizes of DAP_Info."""
        return tuple(struct.unpack("<I", bytes(self.info(i)))[0]
                     for i in (INFO_UART_RX_BUFFER_SIZE, INFO_UART_TX_BUFFER_SIZE))

    def transport(self, transport):
        if self.command([DAP_UART_TRANSPORT, transport])[1] != 0:
            raise DapError("DAP_UART_Transport %d failed" % transport)

    def configure(self, control, baud):
        """Returns the real baud rate. Raises DapError for an error."""
        response = self.command(bytes([DAP_UART_CONFIGURE, control]) + struct.pack("<I", baud))
        status, real = response[1], struct.unpack("<I", response[2:6])[0]
        if status != 0 or real == 0:
            raise DapError("DAP_UART_Configure failed: status 0x%02x, baud rate %d" % (status, real))
        return real

    def control(self, bits):
        if self.command([DAP_UART_CONTROL, bits])[1] != 0:
            raise DapError("DAP_UART_Control 0x%02x failed" % bits)

    def status(self):
        """(status, rx count, tx count)"""
        response = self.command([DAP_UART_STATUS])
        return (response[1], ) + struct.unpack("<II", response[2:10])

    def transfer(self, data, rx_count):
        """Returns (status, bytes taken from data, received bytes)."""
        request = bytes([DAP_UART_TRANSFER]) + struct.pack("<HH", rx_count, len(data)) + bytes(data)
        response = self.command(request)
        status = response[1]
        tx_taken, rx_length = struct.unpack("<HH", response[2:6])
        return status, tx_taken, response[6:6 + rx_length]


def exchange(uart, data, window, timeout_s=SETTLE_TIMEOUT_S, clock=time.monotonic):
    """Sends data and reads the echo. At most window bytes are on the way, so
    the receive buffer cannot overflow. Returns (received bytes, status bits
    that the transfers reported)."""
    received = bytearray()
    status_bits = 0
    sent = 0
    deadline = clock() + timeout_s
    while len(received) < len(data):
        room = min(MAX_TX, window - (sent - len(received)), len(data) - sent)
        chunk = data[sent:sent + max(room, 0)]
        status, taken, echo = uart.transfer(chunk, MAX_RX)
        status_bits |= status
        sent += taken
        received += echo
        if taken or echo:
            deadline = clock() + timeout_s
        elif clock() > deadline:
            break
    return bytes(received), status_bits & STATUS_ERRORS


def compare(data, received, status_bits):
    """Returns (True, 'ok') or (False, text that says what is wrong)."""
    if status_bits:
        names = [name for bit, name in ((STATUS_RX_DATA_LOST, "data lost"), (STATUS_FRAMING_ERROR, "framing error"),
                                        (STATUS_PARITY_ERROR, "parity error")) if status_bits & bit]
        return False, "status: " + ", ".join(names)
    if received == data:
        return True, "ok"
    if len(received) != len(data):
        return False, "received %d of %d bytes" % (len(received), len(data))
    first = next(index for index in range(len(data)) if received[index] != data[index])
    return False, "mismatch at byte %d: sent 0x%02x, received 0x%02x" % (first, data[first], received[first])


def run_case(uart, baud, fmt, size, window, shift=0, serial_port=None):
    """Tests one frame format. Returns (PASS|FAIL, text)."""
    data_bits, parity, stop_bits = parse_format(fmt)
    try:
        real = uart.configure(control_byte(data_bits, parity, stop_bits), baud)
    except DapError as error:
        return FAIL, str(error)
    uart.control(RX_ENABLE | TX_ENABLE | RX_FLUSH | TX_FLUSH)
    # Clear the errors of an earlier test.
    uart.status()
    data = pattern(size, data_bits, shift)
    received, status_bits = exchange(uart, data, window)
    ok, text = compare(data, received, status_bits)
    if ok and serial_port is not None:
        leaked = serial_port.read(4096)
        if leaked:
            ok, text = False, "the serial port got %d bytes" % len(leaked)
    if ok and real != baud:
        text = "ok (real baud rate %d)" % real
    return (PASS if ok else FAIL), text


def run(uart, bauds, formats, size, repeat, serial_port=None, out=sys.stdout):
    """Runs all tests. Returns the exit code: 0 when all pass."""
    if not uart.has_uart():
        print("The firmware reports no UART (DAP_Info capabilities)", file=out)
        return 1
    rx_size, tx_size = uart.buffer_sizes()
    print("UART buffers: %d bytes RX, %d bytes TX" % (rx_size, tx_size), file=out)
    window = min(rx_size, tx_size) // 2
    counts = {PASS: 0, FAIL: 0}
    uart.transport(TRANSPORT_DAP)
    try:
        for run_index in range(repeat):
            for baud in bauds:
                for fmt in formats:
                    result, text = run_case(uart, baud, fmt, size, window, run_index, serial_port)
                    counts[result] += 1
                    print("%-4s %8d %-5s %s" % (result, baud, fmt, text), file=out)
    finally:
        uart.transport(TRANSPORT_COM_PORT)
    print("%d passed, %d failed" % (counts[PASS], counts[FAIL]), file=out)
    return 0 if counts[FAIL] == 0 and counts[PASS] > 0 else 1


def open_link(serial):
    try:
        import usb.core
        import usb.util
    except ImportError:
        sys.exit("pyusb is missing: install the pyusb package")
    devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID))
    if serial is not None:
        devices = [d for d in devices if usb.util.get_string(d, d.iSerialNumber) == serial]
    if not devices:
        sys.exit("No CMSIS-DAP dongle (%04x:%04x) found" % (VID, PID))
    if len(devices) > 1:
        sys.exit("More than one dongle: select one with --serial")
    device = devices[0]
    try:
        if device.is_kernel_driver_active(INTERFACE):
            device.detach_kernel_driver(INTERFACE)
    except (NotImplementedError, usb.core.USBError):
        pass
    usb.util.claim_interface(device, INTERFACE)

    def link(request):
        device.write(OUT_ENDPOINT, request, TIMEOUT_MS)
        return device.read(IN_ENDPOINT, PACKET_SIZE, TIMEOUT_MS)

    return link


def open_serial_port(name):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is missing: install the pyserial package")
    return serial.Serial(name, 115200, timeout=0.05)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", help="the serial number of the dongle (the chip UID)")
    parser.add_argument("--bauds", type=int, nargs="+", default=DEFAULT_BAUDS)
    parser.add_argument("--formats", nargs="+", default=DEFAULT_FORMATS)
    parser.add_argument("--size", type=int, default=1024, help="bytes for each test (1 or more)")
    parser.add_argument("--repeat", type=int, default=1, help="number of runs (1 or more)")
    parser.add_argument("--serial-port", help="the CDC-ACM port of the dongle: check that it gets no data")
    args = parser.parse_args(argv)
    if args.size < 1 or args.repeat < 1:
        parser.error("--size and --repeat must be 1 or more")
    for fmt in args.formats:
        try:
            parse_format(fmt)
        except ValueError as error:
            parser.error(str(error))
    serial_port = open_serial_port(args.serial_port) if args.serial_port else None
    uart = DapUart(open_link(args.serial))
    try:
        return run(uart, args.bauds, args.formats, args.size, args.repeat, serial_port)
    except DapError as error:
        print("%s: %s" % (ERROR, error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
