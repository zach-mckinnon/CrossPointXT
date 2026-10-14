"""SD storage-capacity USB regression tests; reads cannot modify manuscripts."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "e213-sync"))
import plot_jot_sync as bridge


class SpaceSerial:
    def __init__(self, reply):
        self.reply = reply
        self.writes = []
        self.replies = []

    def write(self, raw):
        command = raw.decode("ascii").strip()
        self.writes.append(command)
        if command == "E213SYNC1|SPACE":
            self.replies.append((bridge.PREFIX + self.reply + "\n").encode())
        else:
            raise AssertionError("Storage lookup must use only SPACE")
        return len(raw)

    def flush(self):
        pass

    def readline(self):
        return self.replies.pop(0) if self.replies else b""


class StorageTests(unittest.TestCase):
    def bridge(self, response):
        serial = SpaceSerial(response)
        usb = bridge.USB.__new__(bridge.USB)
        usb.serial = serial
        return usb, serial

    def test_returns_exact_int_bytes_and_only_readonly_command(self):
        total, used = 31_912_345_600, 2_525_123_456
        usb, serial = self.bridge(f"SPACE|{total}|{used}")
        self.assertEqual(usb.storage(), (total, used))
        self.assertEqual(serial.writes, ["E213SYNC1|SPACE"])

    def test_zero_used_is_valid(self):
        usb, _ = self.bridge("SPACE|1000000|0")
        self.assertEqual(usb.storage(), (1000000, 0))

    def test_rejects_overallocated_volume(self):
        usb, _ = self.bridge("SPACE|100|101")
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            usb.storage()

    def test_rejects_non_decimal_or_negative_values(self):
        for result in ("SPACE|100|-2", "SPACE|100|1x", "SPACE|1.5|0"):
            with self.subTest(result=result):
                usb, _ = self.bridge(result)
                with self.assertRaisesRegex(RuntimeError, "nonnumeric"):
                    usb.storage()

    def test_rejects_unknown_command_with_firmware_hint(self):
        usb, _ = self.bridge("ERR|unknown-command")
        with self.assertRaisesRegex(RuntimeError, "latest dev"):
            usb.storage()

    def test_rejects_unexpected_response_instead_of_stale_card_stats(self):
        usb, _ = self.bridge("READY")
        with self.assertRaisesRegex(RuntimeError, "invalid SD capacity response"):
            usb.storage()


if __name__ == "__main__":
    unittest.main()
