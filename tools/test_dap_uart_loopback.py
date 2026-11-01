# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for dap_uart_loopback_test.py. They need no pyusb and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import io
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dap_uart_loopback_test as dut  # noqa: E402


class FakeFirmware:
    """The DAP_UART_* commands of the firmware with TX connected to RX.
    lose_byte_at drops one looped byte. error_bits go into the next status."""

    def __init__(self, capacity=63, lose_byte_at=None, error_bits=0):
        self.capacity = capacity
        self.transport = dut.TRANSPORT_COM_PORT
        self.rx = bytearray()
        self.configured = False
        self.lose_byte_at = lose_byte_at
        self.error_bits = error_bits
        self.looped = 0
        self.requests = []

    def __call__(self, request):
        self.requests.append(request)
        command = request[0]
        if command == dut.DAP_INFO:
            if request[1] == dut.INFO_CAPABILITIES:
                return bytes([0, 2, 0x91, 0x01])
            return bytes([0, 4]) + struct.pack("<I", self.capacity)
        if command == dut.DAP_UART_TRANSPORT:
            self.transport = request[1]
            return bytes([command, 0])
        dap = self.transport == dut.TRANSPORT_DAP
        if command == dut.DAP_UART_CONFIGURE:
            baud = struct.unpack("<I", request[2:6])[0]
            self.configured = dap
            return bytes([command, 0 if dap else 7]) + struct.pack("<I", baud if dap else 0)
        if command == dut.DAP_UART_CONTROL:
            return bytes([command, 0 if self.configured else 0xFF])
        if command == dut.DAP_UART_STATUS:
            status, self.error_bits = 0x11 | self.error_bits, 0
            return bytes([command, status]) + struct.pack("<II", len(self.rx), 0)
        if command == dut.DAP_UART_TRANSFER:
            rx_count, tx_count = struct.unpack("<HH", request[1:5])
            assert len(request) == 5 + tx_count <= dut.PACKET_SIZE
            data = request[5:]
            for byte in data:
                if self.looped == self.lose_byte_at:
                    pass
                elif len(self.rx) < self.capacity:
                    self.rx.append(byte)
                else:
                    raise AssertionError("the receive buffer overflowed")
                self.looped += 1
            echo = bytes(self.rx[:min(rx_count, dut.MAX_RX)])
            del self.rx[:len(echo)]
            return bytes([command, 0x11, len(data), 0, len(echo), 0]) + echo
        return bytes([dut.DAP_INVALID])


class ParseFormatTest(unittest.TestCase):
    def test_formats(self):
        self.assertEqual(dut.parse_format("8N1"), (8, "N", 1))
        self.assertEqual(dut.parse_format("7e2"), (7, "E", 2))
        self.assertEqual(dut.parse_format("8O1.5"), (8, "O", 1.5))

    def test_invalid(self):
        for text in ("8N", "9N1", "8X1", "8N3", ""):
            with self.assertRaises(ValueError):
                dut.parse_format(text)


class ControlByteTest(unittest.TestCase):
    def test_fields(self):
        self.assertEqual(dut.control_byte(8, "N", 1), 0x00)
        self.assertEqual(dut.control_byte(7, "E", 2), 0x07 | 0x10 | 0x40)
        self.assertEqual(dut.control_byte(8, "O", 1.5), 0x20 | 0x80)


class DapUartTest(unittest.TestCase):
    def test_transfer_request_and_response(self):
        firmware = FakeFirmware()
        uart = dut.DapUart(firmware)
        uart.transport(dut.TRANSPORT_DAP)
        self.assertEqual(uart.transfer(b"\x01\x02", 10), (0x11, 2, b"\x01\x02"))
        self.assertEqual(firmware.requests[-1], bytes([0x21, 10, 0, 2, 0, 1, 2]))

    def test_invalid_command(self):
        uart = dut.DapUart(lambda request: bytes([dut.DAP_INVALID]))
        with self.assertRaisesRegex(dut.DapError, "does not know"):
            uart.transport(dut.TRANSPORT_DAP)

    def test_configure_error(self):
        uart = dut.DapUart(FakeFirmware())
        with self.assertRaisesRegex(dut.DapError, "status 0x07"):
            uart.configure(0, 9600)

    def test_buffer_sizes(self):
        self.assertEqual(dut.DapUart(FakeFirmware(capacity=1023)).buffer_sizes(), (1023, 1023))


class RunTest(unittest.TestCase):
    def run_tests(self, firmware, **kwargs):
        out = io.StringIO()
        code = dut.run(dut.DapUart(firmware), kwargs.get("bauds", [115200]), kwargs.get("formats", ["8N1", "7E1"]),
                       kwargs.get("size", 300), 1, out=out)
        return code, out.getvalue()

    def test_pass(self):
        firmware = FakeFirmware()
        code, text = self.run_tests(firmware)
        self.assertEqual(code, 0, text)
        self.assertIn("2 passed, 0 failed", text)
        # The UART goes back to the serial port.
        self.assertEqual(firmware.transport, dut.TRANSPORT_COM_PORT)

    def test_window_keeps_the_receive_buffer_from_overflow(self):
        # FakeFirmware raises when its buffer overflows.
        code, text = self.run_tests(FakeFirmware(capacity=63), size=2000)
        self.assertEqual(code, 0, text)

    def test_lost_byte_fails(self):
        code, text = self.run_tests(FakeFirmware(lose_byte_at=10), formats=["8N1"])
        self.assertEqual(code, 1)
        self.assertIn("FAIL", text)

    def test_status_error_fails(self):
        firmware = FakeFirmware()

        def link(request):
            response = firmware(request)
            if request[0] == dut.DAP_UART_TRANSFER:
                response = response[:1] + bytes([response[1] | dut.STATUS_PARITY_ERROR]) + response[2:]
            return response

        code, text = self.run_tests(link, formats=["8N1"])
        self.assertEqual(code, 1)
        self.assertIn("parity error", text)

    def test_no_uart(self):
        def link(request):
            if request[0] == dut.DAP_INFO:
                return bytes([0, 1, 0x11])
            raise AssertionError("no UART command expected")

        code, text = self.run_tests(link)
        self.assertEqual(code, 1)
        self.assertIn("no UART", text)


class CompareTest(unittest.TestCase):
    def test_mismatch(self):
        self.assertEqual(dut.compare(b"\x01\x02", b"\x01\x03", 0),
                         (False, "mismatch at byte 1: sent 0x02, received 0x03"))

    def test_short(self):
        self.assertEqual(dut.compare(b"\x01\x02", b"\x01", 0), (False, "received 1 of 2 bytes"))


class ExchangeTest(unittest.TestCase):
    def test_timeout_without_echo(self):
        now = [0.0]

        def clock():
            now[0] += 0.5
            return now[0]

        class Silent:
            def transfer(self, data, rx_count):
                return 0x11, len(data), b""

        received, status = dut.exchange(Silent(), b"\x01" * 10, 100, timeout_s=2.0, clock=clock)
        self.assertEqual(received, b"")
        self.assertEqual(status, 0)


if __name__ == "__main__":
    unittest.main()
