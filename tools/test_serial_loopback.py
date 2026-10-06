"""Host tests for serial_loopback_test.py. They use a fake port, so they need
no pyserial and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial_loopback_test as slt  # noqa: E402


class FakePort:
    """A loopback port. It rejects some settings and can corrupt the data."""

    def __init__(self, reject_stop_bits=(), corrupt_at=None, drop_after=None):
        self._reject_stop_bits = reject_stop_bits
        self._corrupt_at = corrupt_at
        self._drop_after = drop_after
        self._buffer = bytearray()
        self._total = 0
        self.settings = []
        self.baudrate = self.bytesize = self.parity = None
        self.dtr = self.rts = None
        self.closed = False
        self._stopbits = 1

    @property
    def stopbits(self):
        return self._stopbits

    @stopbits.setter
    def stopbits(self, value):
        if value in self._reject_stop_bits:
            raise ValueError("Not a valid stop bit size: %r" % value)
        self._stopbits = value
        self.settings.append((self.baudrate, self.bytesize, self.parity, value))

    def write(self, data):
        for byte in data:
            if self._drop_after is not None and self._total >= self._drop_after:
                continue
            if self._corrupt_at is not None and self._total == self._corrupt_at:
                byte ^= 0xFF
            self._total += 1
            self._buffer.append(byte)
        return len(data)

    def read(self, size=1):
        data = bytes(self._buffer[:size])
        del self._buffer[:size]
        return data

    def reset_input_buffer(self):
        self._buffer.clear()

    def close(self):
        self.closed = True


def setUpModule():
    # The tests need no settle time of a real port.
    slt.time = type("NoSleep", (), {"sleep": staticmethod(lambda seconds: None)})


class PatternTest(unittest.TestCase):
    def test_eight_bits_use_all_values(self):
        self.assertEqual(max(slt.pattern(512)), 255)

    def test_seven_bits_keep_top_bit_clear(self):
        self.assertTrue(all(byte < 0x80 for byte in slt.pattern(512, 7)))

    def test_shift_changes_pattern(self):
        self.assertNotEqual(slt.pattern(16, 8, 0), slt.pattern(16, 8, 1))


class FormatTest(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(slt.parse_format("7e"), (7, "E"))

    def test_invalid(self):
        for text in ("9N", "8X", "8", "8NN"):
            with self.assertRaises(ValueError):
                slt.parse_format(text)


class CaseTest(unittest.TestCase):
    def test_default_matrix_size(self):
        cases = slt.build_cases(slt.DEFAULT_BAUDS, slt.DEFAULT_FORMATS, slt.DEFAULT_STOP_BITS)
        self.assertEqual(len(cases), 10 * 5 * 3)
        self.assertEqual(min(slt.DEFAULT_BAUDS), 9600)
        self.assertEqual(max(slt.DEFAULT_BAUDS), 2000000)

    def test_pass_sets_line_coding(self):
        port = FakePort()
        self.assertEqual(slt.run_case(port, 115200, "7O", 2, 600), (slt.PASS, "ok"))
        self.assertEqual(port.settings, [(115200, 7, "O", 2)])

    def test_mismatch(self):
        port = FakePort(corrupt_at=300)
        result, message = slt.run_case(port, 9600, "8N", 1, 600)
        self.assertEqual(result, slt.FAIL)
        self.assertIn("mismatch at byte 300", message)

    def test_lost_bytes(self):
        port = FakePort(drop_after=100)
        result, message = slt.run_case(port, 9600, "8N", 1, 600)
        self.assertEqual(result, slt.FAIL)
        self.assertIn("received 100 of 600 bytes", message)

    def test_unsupported_setting_is_skipped(self):
        port = FakePort(reject_stop_bits=(1.5,))
        result, _ = slt.run_case(port, 9600, "8N", 1.5, 100)
        self.assertEqual(result, slt.SKIP)


class MainTest(unittest.TestCase):
    def run_main(self, port, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = slt.main(["fake", "--size", "300", *args], opener=lambda url, timeout: port)
        return code, out.getvalue()

    def test_pass_exit_code_and_cleanup(self):
        port = FakePort()
        code, out = self.run_main(port, "--bauds", "9600", "2000000", "--repeat", "2")
        self.assertEqual(code, 0)
        self.assertIn("60 passed, 0 failed, 0 skipped", out)
        self.assertTrue(port.closed)

    def test_failure_exit_code(self):
        code, out = self.run_main(FakePort(corrupt_at=10), "--bauds", "9600")
        self.assertEqual(code, 1)
        self.assertIn("FAIL", out)

    def test_skip_does_not_fail(self):
        code, out = self.run_main(FakePort(reject_stop_bits=(1.5,)), "--bauds", "9600")
        self.assertEqual(code, 0)
        self.assertIn("5 skipped", out)

    def test_nothing_tested_fails(self):
        code, _ = self.run_main(FakePort(reject_stop_bits=(1,)), "--bauds", "9600", "--stop-bits", "1")
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
