# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for avr_data_break_test.py. They need no pyusb and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import io
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hwtest"))
import avr_data_break_test as dut  # noqa: E402


class FakeProbe:
    """The EDBG transport and an AVR8 scope with a data breakpoint model.
    hits: the mode names that give a break event at pc after RUN."""

    def __init__(self, hits=("read", "write", "access"), pc=0x42, cause=1, fail=None):
        self.hits = hits
        self.pc = pc
        self.cause = cause
        self.fail = fail
        self.frame = b""
        self.response = None
        self.events = []
        self.commands = []
        self.mode = None

    def __call__(self, report):
        report = bytes(report)
        if report[0] == dut.AVR_CMD:
            m, n = report[1] >> 4, report[1] & 0x0F
            size = report[2] << 8 | report[3]
            if m == 1:
                self.frame = b""
            self.frame += report[4:4 + size]
            if m < n:
                return bytes([dut.AVR_CMD, 0x00])
            self.response = self.handle(self.frame)
            return bytes([dut.AVR_CMD, 0x01])
        if report[0] == dut.AVR_RSP:
            data, self.response = self.response, None
            return bytes([dut.AVR_RSP, 0x11]) + struct.pack(">H", len(data)) + data
        if report[0] == dut.AVR_EVT:
            if not self.events:
                return bytes([dut.AVR_EVT, 0, 0])
            event = self.events.pop(0)
            return bytes([dut.AVR_EVT]) + struct.pack(">H", len(event)) + event
        raise AssertionError("unknown report")

    def break_event(self, pc, cause):
        return bytes([dut.SOF, 0, 0, 0, dut.AVR8, dut.EVT_BREAK]) + struct.pack("<I", pc) + bytes([cause, 1, 0])

    def handle(self, frame):
        sequence = frame[2:4]
        handler, payload = frame[4], frame[5:]
        self.commands.append((handler, payload))
        head = bytes([dut.SOF]) + sequence + bytes([handler])
        if handler != dut.AVR8:
            return head + bytes([dut.RSP_OK, 0])
        command = payload[0]
        if command == self.fail:
            return head + bytes([dut.RSP_FAILED, 0, 0x81])
        if command == dut.ACTIVATE_PHYSICAL:
            return head + bytes([0x84, 0x01]) + struct.pack("<I", 0x4958703F) + b"\0"
        if command in (dut.ATTACH, dut.RESET):
            self.events.append(self.break_event(0x3800, 0))
        if command == dut.HW_BREAK_SET:
            self.mode = payload[8]
        if command == dut.RUN:
            names = {v: k for k, v in dut.MODES.items()}
            if names[self.mode] in self.hits:
                self.events.append(self.break_event(self.pc, self.cause))
        return head + bytes([dut.RSP_OK, 0])


def args_for(*argv):
    return dut.parse_args(["--address", "0x0210", "--timeout", "0"] + list(argv))


class FrameTest(unittest.TestCase):
    def test_frame_and_fragments(self):
        data = dut.frame(0x1234, dut.AVR8, [0x40, 0x00])
        self.assertEqual(data, bytes([0x0E, 0x00, 0x34, 0x12, 0x12, 0x40, 0x00]))
        self.assertEqual(dut.fragments(data), [bytes([0x80, 0x11, 0x00, 0x07]) + data])
        parts = dut.fragments(bytes(130))
        self.assertEqual([p[1] for p in parts], [0x13, 0x23, 0x33])
        self.assertEqual([p[2] << 8 | p[3] for p in parts], [60, 60, 10])

    def test_break_set_encoding(self):
        # [0x40][version][type 2][number][address u32][mode]
        self.assertEqual(dut.break_set_command(3, 0x0210, dut.MODES["write"]),
                         bytes([0x40, 0x00, 0x02, 0x03, 0x10, 0x02, 0x00, 0x00, 0x01]))
        self.assertEqual(dut.MODES, {"read": 0, "write": 1, "access": 2})

    def test_parse_event(self):
        event = FakeProbe().break_event(0x12345, 1)
        report = bytes([dut.AVR_EVT]) + struct.pack(">H", len(event)) + event
        self.assertEqual(dut.parse_event(report), (0x12345, 1))
        self.assertIsNone(dut.parse_event(bytes([dut.AVR_EVT, 0, 0])))
        with self.assertRaises(dut.EdbgError):
            dut.parse_event(bytes([0x81, 0, 0]))

    def test_device_context(self):
        desc = dut.device_context(4)
        self.assertEqual(len(desc), 31)
        self.assertEqual(struct.unpack_from("<HI", desc), (128, 32768))
        self.assertEqual(desc[0x13], 4)
        self.assertEqual(desc[0x18], 0x31)


class RunTest(unittest.TestCase):
    def test_all_modes_pass(self):
        probe = FakeProbe()
        out = io.StringIO()
        self.assertEqual(dut.run(dut.Edbg(probe), args_for(), out), 0)
        text = out.getvalue()
        self.assertIn("IDCODE 0x4958703F", text)
        for mode in ("read", "write", "access"):
            self.assertIn("%-7s PASS  PC word 0x00042 (byte 0x00084), cause 1" % mode, text)
        sets = [p for h, p in probe.commands if h == dut.AVR8 and p[0] == dut.HW_BREAK_SET]
        self.assertEqual([p[8] for p in sets], [0, 1, 2])
        clears = [p for h, p in probe.commands if h == dut.AVR8 and p[0] == dut.HW_BREAK_CLEAR]
        self.assertEqual(len(clears), 3)
        self.assertEqual(probe.commands[-1], (dut.HOUSEKEEPING, bytes([dut.SIGN_OFF, 0, 0])))

    def test_a_mode_without_a_break_fails(self):
        probe = FakeProbe(hits=("read",))
        out = io.StringIO()
        self.assertEqual(dut.run(dut.Edbg(probe), args_for("--modes", "read,write", "--number", "3"), out), 1)
        self.assertIn("write   FAIL  no break event", out.getvalue())

    def test_cause_0_fails(self):
        out = io.StringIO()
        self.assertEqual(dut.run(dut.Edbg(FakeProbe(cause=0)), args_for("--modes", "read"), out), 1)
        self.assertIn("read    FAIL", out.getvalue())

    def test_a_failed_command_raises_and_closes(self):
        probe = FakeProbe(fail=dut.HW_BREAK_SET)
        with self.assertRaisesRegex(dut.EdbgError, "0x40 failed with code 0x81"):
            dut.run(dut.Edbg(probe), args_for(), io.StringIO())
        self.assertEqual(probe.commands[-1][0], dut.HOUSEKEEPING)

    def test_args(self):
        self.assertEqual(args_for().modes, ["read", "write", "access"])
        with self.assertRaises(SystemExit):
            dut.parse_args(["--address", "0x10000"])
        with self.assertRaises(SystemExit):
            dut.parse_args(["--address", "0x100", "--modes", "mask"])
        with self.assertRaises(SystemExit):
            dut.parse_args(["--address", "0x100", "--number", "1"])


if __name__ == "__main__":
    unittest.main()
