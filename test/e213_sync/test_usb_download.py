"""GET2 regression: reliable host-acknowledged USB downloads."""
import base64
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "e213-sync"))
import plot_jot_sync as bridge


class DownloadSerial:
    def __init__(self, filename, content, *, corrupt_at=None, legacy=False,
                 stale_header_once=False, invalid_header_always=False,
                 download_timeout_once_at=None, download_timeout_always_at=None,
                 stale_unknown_during_reset=False):
        self.filename = filename
        self.content = content
        self.corrupt_at = corrupt_at
        self.legacy = legacy
        self.stale_header_once = stale_header_once
        self.invalid_header_always = invalid_header_always
        self.download_timeout_once_at = download_timeout_once_at
        self.download_timeout_always_at = download_timeout_always_at
        self.sent_download_timeout = False
        self.stale_unknown_during_reset = stale_unknown_during_reset
        self.requests = 0
        self.resets = 0
        self.offset = 0
        self.acks = 0
        self.sent = []
        self.response_frames = []
        self.lines = []
        self.active = False

    def write(self, data):
        line = data.decode("ascii").strip()
        self.sent.append(line)
        cmd, *args = line[len(bridge.PREFIX):].split("|")
        if cmd == "HELLO":
            self.resets += 1
            self.active = False
            if self.stale_unknown_during_reset and self.sent_download_timeout:
                self.lines.append(b"E213SYNC1|ERR|unknown-command\n")
            self.lines.append(b"E213SYNC1|READY\n")
        elif cmd == "GET2":
            self.requests += 1
            if self.legacy:
                self.lines.append(b"E213SYNC1|ERR|unknown-command\n")
            elif args == [self.filename]:
                self.active = True
                self.offset = 0
                self.acks = 0
                digest = hashlib.sha256(self.content).hexdigest()
                if self.invalid_header_always or (
                    self.stale_header_once and self.requests == 1
                ):
                    self.lines.append(b"E213SYNC1|READY\n")
                else:
                    self.lines.append(
                        f"E213SYNC1|BEGIN|{len(self.content)}|{digest}\n".encode()
                    )
            else:
                self.lines.append(b"E213SYNC1|ERR|not-found\n")
        elif cmd == "ACK":
            if not self.active:
                raise AssertionError("ACK without active download")
            self.acks += 1
            if (self.download_timeout_always_at == self.acks or
                    (self.download_timeout_once_at == self.acks and
                     not self.sent_download_timeout)):
                self.sent_download_timeout = True
                self.active = False
                self.lines.append(b"E213SYNC1|ERR|download-timeout\n")
                return len(data)
            if self.offset == len(self.content):
                self.lines.append(b"E213SYNC1|END\n")
                self.active = False
            else:
                raw = self.content[self.offset:self.offset + 24]
                payload = base64.b64encode(raw).decode("ascii")
                if self.corrupt_at == self.acks:
                    payload = "a" * 113  # Reproduce the user's malformed Base64
                packet = f"E213SYNC1|DATA|{payload}\n".encode("ascii")
                self.response_frames.append(len(packet))
                self.lines.append(packet)
                self.offset += len(raw)
        else:
            raise AssertionError(f"unexpected command {cmd}")
        return len(data)

    def flush(self):
        pass

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


class UsbGet2Tests(unittest.TestCase):
    @staticmethod
    def client(serial):
        device = bridge.USB.__new__(bridge.USB)
        device.serial = serial
        return device

    def test_3005_byte_scene_round_trip_and_every_data_line_below_64_bytes(self):
        text = b"Browsing the Web\n" + b"a" * (3005 - len(b"Browsing the Web\n"))
        serial = DownloadSerial("Browsing the Web.txt", text)
        received, digest = self.client(serial).get("Browsing the Web.txt")
        self.assertEqual(received, text)
        self.assertEqual(digest, bridge.checksum(text))
        self.assertEqual(serial.acks, 127)
        self.assertEqual(len(serial.response_frames), 126)
        self.assertTrue(all(0 < n <= 64 for n in serial.response_frames))
        self.assertIn("E213SYNC1|GET2|Browsing the Web.txt", serial.sent)
        self.assertNotIn("E213SYNC1|GET|Browsing the Web.txt", serial.sent)

    def test_stale_ready_header_recovers_without_changing_device_text(self):
        content = b"diary stays safe" * 70
        serial = DownloadSerial("Chats and Skimms.txt", content,
                                stale_header_once=True)
        actual, digest = self.client(serial).get("Chats and Skimms.txt")
        self.assertEqual(actual, content)
        self.assertEqual(digest, bridge.checksum(content))
        self.assertEqual(serial.requests, 2)
        self.assertEqual(serial.resets, 1)

    def test_persistent_invalid_header_reports_response_without_exposing_prose(self):
        serial = DownloadSerial("Chats and Skimms.txt", b"diary prose",
                                invalid_header_always=True)
        with self.assertRaisesRegex(
            RuntimeError,
            r"invalid header for Chats and Skimms\.txt "
            r"\(received READY, 1 field\(s\); retry also failed\)"
        ):
            self.client(serial).get("Chats and Skimms.txt")
        self.assertEqual(serial.requests, 2)
        self.assertEqual(serial.resets, 1)
        self.assertEqual(serial.content, b"diary prose")

    def test_download_timeout_one_restarts_get2_without_touching_manuscript(self):
        original = b"an unsynced diary entry" * 50
        serial = DownloadSerial("Diary.txt", original,
                                download_timeout_once_at=3)
        downloaded, revision = self.client(serial).get("Diary.txt")
        self.assertEqual(downloaded, original)
        self.assertEqual(revision, bridge.checksum(original))
        self.assertEqual(serial.requests, 2)
        self.assertEqual(serial.resets, 1)
        self.assertEqual(serial.content, original)

    def test_stale_unknown_command_before_ready_is_ignored_only_during_reset(self):
        # Recreates the E213 message seen during Refresh Chapter Order.
        original = b"my diary is not linked" * 55
        serial = DownloadSerial(
            "Voss meets up with the Girls.txt", original,
            download_timeout_once_at=2, stale_unknown_during_reset=True)
        text, digest = self.client(serial).get("Voss meets up with the Girls.txt")
        self.assertEqual(text, original)
        self.assertEqual(digest, bridge.checksum(original))
        self.assertEqual(serial.requests, 2)
        self.assertEqual(serial.resets, 1)

    def test_repeated_download_timeout_stops_after_one_clean_retry(self):
        serial = DownloadSerial("Diary.txt", b"original diary content",
                                download_timeout_always_at=2)
        with self.assertRaisesRegex(RuntimeError, "download-timeout"):
            self.client(serial).get("Diary.txt")
        self.assertEqual(serial.requests, 2)
        self.assertEqual(serial.resets, 1)
        self.assertEqual(serial.content, b"original diary content")

    def test_empty_scene_returns_empty_and_one_end_ack(self):
        serial = DownloadSerial("Empty.txt", b"")
        self.assertEqual(self.client(serial).get("Empty.txt")[0], b"")
        self.assertEqual(serial.acks, 1)

    def test_corrupt_base64_fails_closed_and_never_returns_malformed_bytes(self):
        serial = DownloadSerial("Scene.txt", b"original" * 400, corrupt_at=3)
        with self.assertRaisesRegex(RuntimeError, r"Scene.txt: corrupt Base64 DATA after 48/3200 bytes"):
            self.client(serial).get("Scene.txt")
        self.assertEqual(serial.acks, 3)

    def test_old_firmware_yields_flash_instruction_not_corrupt_legacy_fallback(self):
        serial = DownloadSerial("Scene.txt", b"original", legacy=True)
        with self.assertRaisesRegex(RuntimeError, "firmware is too old"):
            self.client(serial).get("Scene.txt")
        self.assertFalse(any("|GET|" in frame for frame in serial.sent))

    def test_large_document_uses_flow_control_without_overwriting(self):
        content = (b"The writing continues.\n" * 6200)[:128 * 1024]
        serial = DownloadSerial("Novel.txt", content)
        copy, digest = self.client(serial).get("Novel.txt")
        self.assertEqual(copy, content)
        self.assertEqual(digest, bridge.checksum(content))
        self.assertTrue(max(serial.response_frames) <= 64)

    def test_rejects_bad_checksum_after_complete_download(self):
        serial = DownloadSerial("Draft.txt", b"draft content")
        original_write = serial.write

        def forged_write(data):
            rv = original_write(data)
            if data.decode("ascii").startswith("E213SYNC1|GET2|"):
                serial.lines[0] = (
                    f"E213SYNC1|BEGIN|{len(serial.content)}|{'0'*64}\n"
                ).encode()
            return rv

        serial.write = forged_write
        with self.assertRaisesRegex(RuntimeError, "SHA-256 or length verification failed"):
            self.client(serial).get("Draft.txt")


if __name__ == "__main__":
    unittest.main()
