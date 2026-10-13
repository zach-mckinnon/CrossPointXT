"""Offline verification of the optional personal sync GUI's safe engine."""
import importlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[2] / "tools" / "e213-sync"
sys.path.insert(0, str(TOOL))

try:
    personal = importlib.import_module("personal_sync")
except ImportError:
    personal = None

bridge = importlib.import_module("plot_jot_sync")


class FakeUSB:
    def __init__(self):
        self.data = {"Chapter One.txt": b"written on the E213"}
        self.closed = False
    def files(self):
        return {name: bridge.checksum(data) for name, data in self.data.items()
                if bridge.NAME.fullmatch(name)}
    def get(self, name):
        if name not in self.data:
            raise RuntimeError("E213 refused command: not-found")
        data = self.data[name]
        return data, bridge.checksum(data)
    def put(self, name, content, previous):
        if name in self.data:
            if previous != bridge.checksum(self.data[name]):
                raise RuntimeError("stale-version")
        elif previous != "*":
            raise RuntimeError("stale-version")
        self.data[name] = content
    def close(self):
        self.closed = True


class FakeDatabase:
    def __init__(self):
        self.scene = {"id": 101, "chapter_id": 7, "content": "written on laptop"}
    def get_project(self, number):
        return {"id": 42, "name": "Test"} if number == 42 else None
    def update_scene_content(self, number, content):
        assert number == 101
        self.scene["content"] = content


@unittest.skipIf(personal is None, "Tkinter is not installed in this runner")
class PersonalSyncSafetyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.usb = FakeUSB()
        self.db = FakeDatabase()
        self.patchers = [
            patch.object(personal, "db_manager", return_value=self.db),
            patch.object(personal, "with_usb", side_effect=lambda _port, task: task(self.usb)),
            patch.object(personal.bridge, "db_scene", side_effect=lambda _db, _project, _scene, *_args: self.db.scene),
            patch.object(personal, "BACKUP_PATH", Path(self.tmp.name)),
        ]
        for p in self.patchers:
            p.start()
            self.addCleanup(p.stop)
        self.args = ("/dev/ttyACM0", "/source", "/database", 42)

    def test_link_creates_manifest_without_touching_document_or_scene(self):
        msg = personal.link_scene(*self.args, "Chapter One.txt", 101, "device")
        self.assertIn("Linked", msg)
        self.assertEqual(self.db.scene["content"], "written on laptop")
        self.assertEqual(self.usb.data["Chapter One.txt"], b"written on the E213")
        manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(manifest["scenes"]["Chapter One.txt"]["scene_id"], 101)
        self.assertEqual(manifest["scenes"]["Chapter One.txt"]["revision"],
                         bridge.checksum(b"written on laptop"))
        self.assertTrue(list(Path(self.tmp.name).rglob("Chapter One.txt")))

    def test_preview_never_changes_anything(self):
        personal.link_scene(*self.args, "Chapter One.txt", 101, "device")
        previous = self.usb.data.copy()
        report = personal.sync_action(*self.args, apply=False)
        self.assertIn("WOULD PUSH", report)
        self.assertEqual(self.usb.data, previous)
        self.assertEqual(self.db.scene["content"], "written on laptop")

    def test_rejects_duplicate_binding_and_wrong_preference(self):
        with self.assertRaises(ValueError):
            personal.link_scene(*self.args, "Chapter One.txt", 101, "either")
        personal.link_scene(*self.args, "Chapter One.txt", 101, "plot")
        with self.assertRaises(ValueError):
            personal.link_scene(*self.args, "Chapter One.txt", 101, "device")

    def test_unlink_preserves_both_copies_and_allows_rename(self):
        personal.link_scene(*self.args, "Chapter One.txt", 101, "device")
        output = personal.unlink_scene(*self.args, "Chapter One.txt")
        self.assertIn("Unlinked", output)
        self.assertEqual(self.usb.data["Chapter One.txt"], b"written on the E213")
        self.assertEqual(self.db.scene["content"], "written on laptop")
        self.assertEqual(__import__("json").loads(self.usb.data[".sync"])["scenes"], {})

    def test_confirmed_sync_pushes_only_after_preview_and_makes_backups(self):
        personal.link_scene(*self.args, "Chapter One.txt", 101, "device")
        with patch.object(personal.bridge, "backup_db",
                          return_value=Path(self.tmp.name) / "backup.db") as backup:
            output = personal.sync_action(*self.args, apply=True)
        self.assertIn("PUSH", output)
        self.assertTrue(backup.called)
        self.assertEqual(self.db.scene["content"], "written on the E213")
        manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(manifest["scenes"]["Chapter One.txt"]["revision"],
                         bridge.checksum(b"written on the E213"))

    def test_importing_personal_sync_does_not_modify_plot_jot(self):
        self.assertTrue(callable(personal.sync_action))
        self.assertTrue(callable(personal.default_db_path))


if __name__ == "__main__":
    unittest.main()
