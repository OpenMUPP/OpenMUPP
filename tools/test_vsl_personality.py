"""Host tests for vsl_personality.py. They need no pyusb and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vsl_personality as vp  # noqa: E402


class PersonalityOfTest(unittest.TestCase):
    def test_known_ids(self):
        self.assertEqual(vp.personality_of(0x0483, 0xA038), "versaloon")
        self.assertEqual(vp.personality_of(0x0403, 0x9B3C), "cmsis-dap")

    def test_unknown_id(self):
        self.assertIsNone(vp.personality_of(0x0483, 0x5740))


class SwitchRequestTest(unittest.TestCase):
    def test_versaloon_sends_switch_personality(self):
        self.assertEqual(vp.switch_request("versaloon", "cmsis-dap"), bytes([0x0E, 0x01]))

    def test_cmsis_dap_sends_vendor0(self):
        self.assertEqual(vp.switch_request("cmsis-dap", "versaloon"), bytes([0x80, 0x00]))


class CheckReplyTest(unittest.TestCase):
    def test_versaloon_accepted(self):
        vp.check_reply("versaloon", [0x00])

    def test_versaloon_refused(self):
        with self.assertRaisesRegex(vp.PersonalityError, "one personality"):
            vp.check_reply("versaloon", [0x01])

    def test_versaloon_wrong_size(self):
        with self.assertRaises(vp.PersonalityError):
            vp.check_reply("versaloon", [])

    def test_cmsis_dap_accepted(self):
        vp.check_reply("cmsis-dap", [0x80, 0x00])

    def test_cmsis_dap_refused(self):
        with self.assertRaisesRegex(vp.PersonalityError, "refused"):
            vp.check_reply("cmsis-dap", [0x80, 0xFF])

    def test_cmsis_dap_invalid(self):
        with self.assertRaisesRegex(vp.PersonalityError, "one personality"):
            vp.check_reply("cmsis-dap", [0xFF])

    def test_cmsis_dap_other_response(self):
        with self.assertRaises(vp.PersonalityError):
            vp.check_reply("cmsis-dap", [0x00, 0x00])


if __name__ == "__main__":
    unittest.main()
