"""Host tests for vsl_combine_images.py.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vsl_combine_images as vc  # noqa: E402


def parse_ihex(text):
    """{address: byte} of an Intel HEX text. Checks each checksum."""
    memory = {}
    upper = 0
    for line in text.splitlines():
        assert line.startswith(":"), line
        record = bytes.fromhex(line[1:])
        assert sum(record) & 0xFF == 0, f"bad checksum: {line}"
        length, address, record_type = record[0], int.from_bytes(record[1:3], "big"), record[3]
        data = record[4:4 + length]
        if record_type == 0x04:
            upper = int.from_bytes(data, "big") << 16
        elif record_type == 0x00:
            for i, byte in enumerate(data):
                memory[upper + address + i] = byte
        elif record_type == 0x01:
            break
    return memory


class CombineTest(unittest.TestCase):
    def test_gap_is_erased_flash(self):
        image = vc.combine(b"\x01\x02\x03", b"\xaa\xbb", 8)
        self.assertEqual(image, b"\x01\x02\x03" + b"\xff" * 5 + b"\xaa\xbb")

    def test_application_starts_at_offset(self):
        bootloader = bytes(range(256)) * 28  # 7168 bytes, as the 8 KB bootloader
        app = b"\x00\x50\x00\x20" + b"\x11" * 100
        image = vc.combine(bootloader, app, 0x2000)
        self.assertEqual(image[:len(bootloader)], bootloader)
        self.assertEqual(image[0x2000:], app)
        self.assertEqual(set(image[len(bootloader):0x2000]), {0xFF})

    def test_bootloader_fills_area(self):
        self.assertEqual(vc.combine(b"\x01" * 8, b"\x02", 8), b"\x01" * 8 + b"\x02")

    def test_bootloader_too_large(self):
        with self.assertRaisesRegex(vc.CombineError, "larger than the application offset"):
            vc.combine(b"\x01" * 9, b"\x02", 8)

    def test_image_too_large(self):
        with self.assertRaisesRegex(vc.CombineError, "larger than the flash"):
            vc.combine(b"\x01", b"\x02" * 9, 8, flash_size=16)

    def test_image_fills_flash(self):
        self.assertEqual(len(vc.combine(b"\x01", b"\x02" * 8, 8, flash_size=16)), 16)

    def test_empty_inputs(self):
        with self.assertRaises(vc.CombineError):
            vc.combine(b"", b"\x02", 8)
        with self.assertRaises(vc.CombineError):
            vc.combine(b"\x01", b"", 8)


class IhexTest(unittest.TestCase):
    def test_round_trip_at_flash_base(self):
        image = bytes((i * 7) & 0xFF for i in range(0x2345))
        memory = parse_ihex(vc.to_ihex(image))
        self.assertEqual(len(memory), len(image))
        self.assertEqual(bytes(memory[vc.FLASH_BASE + i] for i in range(len(image))), image)

    def test_extended_address_record_on_64k_boundary(self):
        image = b"\x5a" * 0x10010
        text = vc.to_ihex(image)
        self.assertEqual(text.count(":020000040800"), 1)
        self.assertEqual(text.count(":020000040801"), 1)
        memory = parse_ihex(text)
        self.assertEqual(memory[0x08010000], 0x5A)
        self.assertEqual(memory[0x0801000F], 0x5A)

    def test_known_records(self):
        text = vc.to_ihex(b"\x00\x50\x00\x20")
        self.assertEqual(text, ":020000040800F2\n:04000000005000208C\n:00000001FF\n")


class MainTest(unittest.TestCase):
    def test_writes_bin_and_hex(self):
        with tempfile.TemporaryDirectory() as tmp:
            bootloader = os.path.join(tmp, "bl.bin")
            app = os.path.join(tmp, "app.bin")
            out_bin = os.path.join(tmp, "out.bin")
            out_hex = os.path.join(tmp, "out.hex")
            with open(bootloader, "wb") as f:
                f.write(b"\x01" * 100)
            with open(app, "wb") as f:
                f.write(b"\x02" * 50)
            self.assertEqual(vc.main(["--bootloader", bootloader, "--app", app, "--app-offset", "0x2000",
                                      "--bin", out_bin, "--hex", out_hex]), 0)
            with open(out_bin, "rb") as f:
                combined = f.read()
            self.assertEqual(len(combined), 0x2000 + 50)
            with open(out_hex) as f:
                memory = parse_ihex(f.read())
            self.assertEqual(bytes(memory[vc.FLASH_BASE + i] for i in range(len(combined))), combined)

    def test_fails_when_bootloader_too_large(self):
        with tempfile.TemporaryDirectory() as tmp:
            bootloader = os.path.join(tmp, "bl.bin")
            app = os.path.join(tmp, "app.bin")
            with open(bootloader, "wb") as f:
                f.write(b"\x01" * 0x2001)
            with open(app, "wb") as f:
                f.write(b"\x02")
            self.assertEqual(vc.main(["--bootloader", bootloader, "--app", app, "--app-offset", "0x2000",
                                      "--bin", os.path.join(tmp, "out.bin")]), 1)


if __name__ == "__main__":
    unittest.main()
