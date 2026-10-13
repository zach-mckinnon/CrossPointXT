"""USB transfer regression: stale sessions and exact stalled-file diagnosis."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/e213-sync"))
import plot_jot_sync as bridge


class ScriptedSerial:
    def __init__(self, fail_data_at=None, fail_commit=False):
        self.sent = []
        self.data_frame_sizes = []
        self.to_read = []
        self.fail_data_at = fail_data_at
        self.fail_commit = fail_commit
        self.data_count = 0
        self.uploaded = bytearray()

    def write(self, raw):
        line = raw.decode("ascii").strip()
        self.sent.append(line)
        fields = line[len(bridge.PREFIX):].split("|")
        cmd = fields[0]
        reply = None
        if cmd == "PUT":
            reply = "READY"
        elif cmd == "DATA":
            self.data_frame_sizes.append(len(raw))
            # Simulate a constrained ESP32 CDC RX queue; oversized DATA
            # messages would stall waiting for bytes that were discarded.
            if len(raw) > 64:
                raise AssertionError(f"USB DATA frame exceeds 64 bytes: {len(raw)}")
            self.data_count += 1
            if self.data_count == self.fail_data_at:
                reply = "ERR|timeout"
            else:
                import base64
                self.uploaded.extend(base64.b64decode(fields[1]))
                reply = "ACK"
        elif cmd == "END":
            reply = "ERR|timeout" if self.fail_commit else "SAVED"
        if reply:
            self.to_read.append((bridge.PREFIX + reply + "\n").encode("ascii"))
        return len(raw)

    def flush(self):
        pass

    def readline(self):
        return self.to_read.pop(0) if self.to_read else b""


class UsbTimeoutTests(unittest.TestCase):
    def client(self, serial):
        client = bridge.USB.__new__(bridge.USB)
        client.serial = serial
        return client

    def test_data_stall_names_file_phase_and_completed_bytes(self):
        serial = ScriptedSerial(fail_data_at=2)
        client = self.client(serial)
        with self.assertRaisesRegex(RuntimeError, r"E213 upload Scene 2\.txt failed during data \(24/400 bytes acknowledged\): E213 refused command: timeout"):
            client.put("Scene 2.txt", b"z" * 400, "*")
        self.assertEqual(serial.data_count, 2)
        self.assertFalse(any(v.endswith("|END") for v in serial.sent))

    def test_manifest_stall_identifies_commit_phase(self):
        serial = ScriptedSerial(fail_commit=True)
        client = self.client(serial)
        with self.assertRaisesRegex(RuntimeError, r"E213 upload \.sync failed during commit \(3/3 bytes acknowledged\): E213 refused command: timeout"):
            client.put(".sync", b"abc", "*")

    def test_successful_framed_upload_preserves_payload(self):
        serial = ScriptedSerial()
        client = self.client(serial)
        content = b"test manuscript " * 40
        client.put("Scene 3.txt", content, "*")
        self.assertEqual(bytes(serial.uploaded), content)
        self.assertTrue(all(0 < n <= 64 for n in serial.data_frame_sizes))
        self.assertTrue(serial.sent[-1].endswith("|END"))

    def test_previous_first_frame_overflow_reproduced_and_fixed(self):
        # Old encoding: 192 raw bytes -> 256 Base64 + 16-byte framing
        # = 272-byte DATA line. The first packet could exceed the CDC
        # RX queue and cause precisely 0 bytes acknowledged / timeout.
        import base64
        old_frame = (bridge.PREFIX + "DATA|" +
                     base64.b64encode(b"x" * 192).decode() + "\n").encode()
        self.assertEqual(len(old_frame), 272)
        client_serial = ScriptedSerial()
        self.client(client_serial).put("Browsing the Web.txt", b"x" * 3005, "*")
        self.assertEqual(len(client_serial.uploaded), 3005)
        self.assertEqual(len(client_serial.data_frame_sizes), 126)
        self.assertTrue(all(n <= 64 for n in client_serial.data_frame_sizes))

    def test_rejects_bad_names_before_usb_transfer(self):
        serial = ScriptedSerial()
        client = self.client(serial)
        with self.assertRaises(ValueError):
            client.put("../secret.txt", b"payload", "*")
        self.assertEqual(serial.sent, [])


if __name__ == "__main__":
    unittest.main()
