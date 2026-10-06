#!/usr/bin/env python3
"""Loopback test for the serial port (UART bridge) of the Versaloon dongle.

Connect the UART TX pin to the UART RX pin of the dongle (PA9 to PA10 on the
Mini and the OtherUse). The script sends a byte pattern at each line coding
and checks that the same bytes come back unchanged and in order.

The default run covers all baud rates from 9600 to 2000000 baud, the data
bits and parity settings of the UART (8N, 8E, 8O, 7E, 7O) and the stop bit
settings 1, 1.5 and 2. A setting that the host serial driver cannot make
is reported as SKIP, and does not fail the run. Only Windows can make 1.5
stop bits. On other hosts pyserial sets 2 stop bits instead, so the script
skips 1.5 there. A port error (for example an unplugged dongle) stops the
run with ERROR and a non-zero exit code.

The script also checks the time of each test: the data must not arrive
faster than the baud rate allows. This finds a bridge that ignores the
baud rate. A loopback cannot check the data bits and the parity, because TX
and RX use the same setting. A second, reference serial adapter on the
UART pins is necessary for that check.

Usage:
    python3 serial_loopback_test.py /dev/ttyACM0       (Linux)
    py serial_loopback_test.py COM5                    (Windows)
    python3 serial_loopback_test.py /dev/ttyACM0 --bauds 115200 --repeat 100

Run it again while OpenOCD does a long JTAG or SWD transfer, to test that
the serial port works during the transfer. Use --repeat to make the script
run as long as the transfer.

To test the script without hardware, use the pyserial loopback URL:
    python3 serial_loopback_test.py loop://
The unit tests in test_serial_loopback.py need no pyserial.

Requires the pyserial package to open a port.
"""

import argparse
import os
import sys
import time

DEFAULT_BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000]
# (data bits, parity). The UART has no 7-bit format without parity.
DEFAULT_FORMATS = ["8N", "8E", "8O", "7E", "7O"]
DEFAULT_STOP_BITS = [1, 1.5, 2]
# The UART bridge has 1 KB ring buffers. A chunk smaller than this, sent
# only after the echo of the earlier chunk, never overruns a ring buffer.
CHUNK_SIZE = 256

# pyserial sets 1.5 stop bits like 2 stop bits when the host is not Windows.
ONE_AND_HALF_STOP_BITS_SUPPORTED = os.name == "nt"
# The echo of all data takes at least size frames. A faster test shows a wrong
# baud rate. The margin is for the baud rate error of the host and the UART.
MIN_TIME_FACTOR = 0.8

PASS, FAIL, SKIP, ERROR = "PASS", "FAIL", "SKIP", "ERROR"


def parse_format(text):
    """'8N' -> (8, 'N'). Raises ValueError for a format that is not valid."""
    if len(text) != 2 or text[0] not in "78" or text[1].upper() not in "NEO":
        raise ValueError("invalid format %r (use 8N, 8E, 8O, 7E or 7O)" % text)
    return int(text[0]), text[1].upper()


def pattern(size, data_bits=8, shift=0):
    """Bytes that count up, so that a lost or doubled byte shows at once.

    With 7 data bits the top bit is not sent, so the pattern keeps it clear.
    """
    mask = 0xFF if data_bits == 8 else 0x7F
    return bytes(((index + shift) & 0xFF) & mask for index in range(size))


def frame_bits(data_bits, parity, stop_bits):
    """Bits of one frame on the wire: start bit, data, parity and stop bits."""
    return 1 + data_bits + (0 if parity == "N" else 1) + stop_bits


def drain(port, settle_seconds=0.1):
    """Discards data of an earlier test that the dongle still sends."""
    time.sleep(settle_seconds)
    while port.read(4096):
        pass
    port.reset_input_buffer()


def set_line_coding(port, baud, data_bits, parity, stop_bits):
    """Sets the line coding. Returns None, or the reason why the host cannot."""
    try:
        port.baudrate = baud
        port.bytesize = data_bits
        port.parity = parity
        port.stopbits = stop_bits
    except (ValueError, OSError) as error:
        # The setters only reconfigure the port. A failure means that the
        # host driver cannot make the setting (pyserial raises
        # SerialException, an OSError, on Windows). A port that is really gone
        # gives an ERROR in the next exchange.
        return str(error)
    return None


def exchange(port, data):
    """Sends data in chunks. Returns the bytes that came back."""
    received = bytearray()
    sent = 0
    while sent < len(data):
        chunk = data[sent:sent + CHUNK_SIZE]
        port.write(chunk)
        sent += len(chunk)
        while len(received) < sent:
            part = port.read(sent - len(received))
            if not part:
                return bytes(received)
            received += part
    return bytes(received)


def compare(data, received):
    """Returns (True, 'ok') or (False, text that says what is wrong)."""
    if received == data:
        return True, "ok"
    if len(received) != len(data):
        return False, "received %d of %d bytes" % (len(received), len(data))
    first = next(index for index in range(len(data)) if received[index] != data[index])
    return False, "mismatch at byte %d: sent 0x%02x, received 0x%02x" % (first, data[first], received[first])


def run_case(port, baud, fmt, stop_bits, size, shift=0):
    """Tests one line coding. Returns (PASS|FAIL|SKIP|ERROR, text).

    ERROR means that the port failed (OSError, which includes the pyserial
    exceptions). The port is not usable then, so the caller stops.
    """
    data_bits, parity = parse_format(fmt)
    if stop_bits == 1.5 and not ONE_AND_HALF_STOP_BITS_SUPPORTED:
        return SKIP, "this host cannot make 1.5 stop bits"
    try:
        reason = set_line_coding(port, baud, data_bits, parity, stop_bits)
        if reason is not None:
            return SKIP, "host cannot set it: %s" % reason
        drain(port)
        data = pattern(size, data_bits, shift)
        start = time.monotonic()
        received = exchange(port, data)
        elapsed = time.monotonic() - start
        passed, message = compare(data, received)
        minimum = MIN_TIME_FACTOR * size * frame_bits(data_bits, parity, stop_bits) / baud
        if passed and elapsed < minimum:
            passed = False
            message = "too fast: %.4f s, at least %.4f s at this baud rate (does the dongle use the baud rate?)" % (
                elapsed,
                minimum,
            )
        if not passed:
            drain(port)
    except OSError as error:
        return ERROR, "port error: %s" % error
    return (PASS if passed else FAIL), message


def build_cases(bauds, formats, stop_bits):
    return [(baud, fmt, stop) for baud in bauds for fmt in formats for stop in stop_bits]


def open_serial(url, timeout):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is missing: install the pyserial package")
    # Open without DTR, then set it explicitly: the bridge drops the UART
    # data while the host has not configured the port, and not every
    # backend sets DTR on open. serial_for_url() also takes loop://.
    port = serial.serial_for_url(url, do_not_open=True)
    port.baudrate = 115200
    port.timeout = timeout
    port.write_timeout = timeout
    port.dtr = False
    port.open()
    port.dtr = True
    port.rts = True
    return port


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="serial port, for example /dev/ttyACM0, COM5 or loop://")
    parser.add_argument("--bauds", type=int, nargs="+", default=DEFAULT_BAUDS, help="baud rates")
    parser.add_argument("--formats", nargs="+", default=DEFAULT_FORMATS, help="data bits and parity: 8N 8E 8O 7E 7O")
    parser.add_argument("--stop-bits", type=float, nargs="+", default=DEFAULT_STOP_BITS, help="stop bits: 1 1.5 2")
    parser.add_argument("--size", type=int, default=1024, help="bytes for each line coding (default 1024)")
    parser.add_argument("--repeat", type=int, default=1, help="number of runs of all line codings")
    parser.add_argument("--timeout", type=float, default=2.0, help="read timeout in seconds")
    args = parser.parse_args(argv)
    try:
        for fmt in args.formats:
            parse_format(fmt)
    except ValueError as error:
        parser.error(str(error))
    if args.size < 1:
        parser.error("--size must be 1 or more")
    if args.repeat < 1:
        parser.error("--repeat must be 1 or more")
    if args.timeout <= 0:
        parser.error("--timeout must be more than 0")
    if any(baud < 1 for baud in args.bauds):
        parser.error("--bauds must be 1 or more")
    if any(stop not in DEFAULT_STOP_BITS for stop in args.stop_bits):
        parser.error("--stop-bits must be 1, 1.5 or 2")
    args.stop_bits = [int(stop) if stop == int(stop) else stop for stop in args.stop_bits]
    return args


def main(argv=None, opener=open_serial):
    args = parse_args(argv)
    cases = build_cases(args.bauds, args.formats, args.stop_bits)
    port = opener(args.port, args.timeout)
    counts = {PASS: 0, FAIL: 0, SKIP: 0, ERROR: 0}
    try:
        for run in range(args.repeat):
            for index, (baud, fmt, stop) in enumerate(cases):
                # A new shift for each test makes two tests differ.
                result, message = run_case(port, baud, fmt, stop, args.size, shift=run * len(cases) + index)
                counts[result] += 1
                print("run %d, %7d baud %s%s: %s %s" % (run + 1, baud, fmt, stop, result, message))
                if result == ERROR:
                    break
            if counts[ERROR]:
                print("stopped: the port failed")
                break
    finally:
        try:
            port.dtr = False
            port.close()
        except OSError:
            pass  # The port is already gone.

    print("%d passed, %d failed, %d skipped, %d port errors" % (counts[PASS], counts[FAIL], counts[SKIP], counts[ERROR]))
    if counts[PASS] == 0 and counts[FAIL] == 0 and counts[ERROR] == 0:
        print("no line coding was tested")
        return 1
    return 1 if counts[FAIL] or counts[ERROR] else 0


if __name__ == "__main__":
    sys.exit(main())
