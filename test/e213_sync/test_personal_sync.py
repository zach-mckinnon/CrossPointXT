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
        self.other_scene = {"id": 102, "chapter_id": 7, "content": "another scene"}
        self.layout = None
    def get_scene(self, scene_id):
        return {101: self.scene, 102: self.other_scene}.get(scene_id)
    def get_full_manuscript_data(self, project_id):
        assert project_id == 42
        return self.layout if self.layout is not None else [{
            "title": "Chapter 1", "scenes": [
                {"id": 101, "title": "Scene 1"},
                {"id": 102, "title": "Scene 2"}
            ]
        }]
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
            patch.object(personal.bridge, "db_scene", side_effect=lambda _db, _project, _scene, *_args: self.db.get_scene(_scene)),
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

    def test_import_existing_scene_creates_txt_on_card_not_on_desktop(self):
        before = self.db.scene["content"]
        msg = personal.import_scene(*self.args, 101, "The Arrival.txt")
        self.assertIn("IMPORTED", msg)
        self.assertEqual(self.usb.data["The Arrival.txt"], before.encode("utf-8"))
        self.assertEqual(self.db.scene["content"], before)
        manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(manifest["scenes"]["The Arrival.txt"]["scene_id"], 101)
        self.assertEqual(manifest["scenes"]["The Arrival.txt"]["revision"],
                         bridge.checksum(before.encode("utf-8")))
        self.assertTrue(list(Path(self.tmp.name).rglob("Chapter One.txt")))

    def test_import_refuses_existing_filename_and_never_overwrites_it(self):
        initial = self.usb.data["Chapter One.txt"]
        with self.assertRaisesRegex(ValueError, "already exists"):
            personal.import_scene(*self.args, 101, "Chapter One.txt")
        with self.assertRaisesRegex(ValueError, "already exists"):
            personal.import_scene(*self.args, 101, "chapter one.txt")
        self.assertEqual(self.usb.data["Chapter One.txt"], initial)
        self.assertNotIn(".sync", self.usb.data)

    def test_import_prevents_two_files_linked_to_the_same_scene(self):
        personal.import_scene(*self.args, 101, "The Arrival.txt")
        with self.assertRaisesRegex(ValueError, "already linked"):
            personal.import_scene(*self.args, 101, "Second File.txt")
        self.assertNotIn("Second File.txt", self.usb.data)

    def test_import_rejects_oversized_scenes_before_communications(self):
        self.db.scene["content"] = "x" * (bridge.MAX_FILE + 1)
        before = self.usb.data.copy()
        with self.assertRaisesRegex(ValueError, "per-document limit"):
            personal.import_scene(*self.args, 101, "Long Scene.txt")
        self.assertEqual(self.usb.data, before)

    def test_import_verifies_sd_before_writing_manifest(self):
        def corrupt(name, content, previous):
            self.usb.data[name] = b"wrong content"
        with patch.object(self.usb, "put", side_effect=corrupt):
            with self.assertRaisesRegex(RuntimeError, "readback failed"):
                personal.import_scene(*self.args, 101, "Corrupt.txt")
        self.assertNotIn(".sync", self.usb.data)
        self.assertEqual(self.db.scene["content"], "written on laptop")

    def test_multi_import_two_scenes_and_preserves_existing_draft(self):
        original = self.usb.data["Chapter One.txt"]
        report = personal.import_scenes(*self.args, [101, 102])
        self.assertIn("2 linked", report)
        self.assertEqual(self.usb.data["Scene 1.txt"], b"written on laptop")
        self.assertEqual(self.usb.data["Scene 2.txt"], b"another scene")
        self.assertEqual(self.usb.data["Chapter One.txt"], original)
        manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(manifest["scenes"]["Scene 1.txt"]["scene_id"], 101)
        self.assertEqual(manifest["scenes"]["Scene 2.txt"]["scene_id"], 102)
        self.assertEqual(self.db.scene["content"], "written on laptop")
        self.assertEqual(self.db.other_scene["content"], "another scene")

    def test_multi_import_recovers_identical_orphan_without_overwriting(self):
        self.usb.data["Scene 1.txt"] = b"written on laptop"
        before = self.usb.data["Scene 1.txt"]
        report = personal.import_scenes(*self.args, [101])
        self.assertIn("RECOVERED", report)
        self.assertIn("1 linked", report)
        self.assertEqual(self.usb.data["Scene 1.txt"], before)
        self.assertIn("Scene 1.txt", __import__("json").loads(self.usb.data[".sync"])["scenes"])

    def test_multi_import_refuses_orphan_with_different_contents(self):
        self.usb.data["Scene 1.txt"] = b"local edits must be protected"
        report = personal.import_scenes(*self.args, [101])
        self.assertIn("DIFFERENT CONTENT", report)
        self.assertIn("0 linked, 1 skipped", report)
        self.assertEqual(self.usb.data["Scene 1.txt"], b"local edits must be protected")
        self.assertNotIn(".sync", self.usb.data)

    def test_multi_import_can_resume_after_successful_import(self):
        personal.import_scenes(*self.args, [101])
        report = personal.import_scenes(*self.args, [101, 102])
        self.assertIn("ALREADY LINKED", report)
        self.assertIn("1 linked, 1 skipped", report)
        self.assertEqual(__import__("json").loads(self.usb.data[".sync"])["scenes"]["Scene 2.txt"]["scene_id"], 102)

    def test_multi_import_reports_actual_manifest_rejection(self):
        old_put = self.usb.put
        def fail_manifest(name, content, expected):
            if name == ".sync":
                raise RuntimeError("E213 refused command: invalid-manifest")
            return old_put(name, content, expected)
        with patch.object(self.usb, "put", side_effect=fail_manifest):
            with self.assertRaisesRegex(RuntimeError, "invalid-manifest"):
                personal.import_scenes(*self.args, [101])
        self.assertEqual(self.usb.data["Scene 1.txt"], b"written on laptop")
        self.assertNotIn(".sync", self.usb.data)

    def test_single_import_reports_underlying_manifest_error(self):
        old_put = self.usb.put
        def fail_manifest(name, content, expected):
            if name == ".sync":
                raise RuntimeError("E213 refused command: write-failed")
            return old_put(name, content, expected)
        with patch.object(self.usb, "put", side_effect=fail_manifest):
            with self.assertRaisesRegex(RuntimeError, "write-failed"):
                personal.import_scene(*self.args, 101, "Named Scene.txt")

    def test_same_scene_title_across_chapters_gets_distinct_filename_even_later(self):
        self.db.layout = [
            {"title": "Chapter 1", "scenes": [{"id": 101, "title": "Scene 1"}]},
            {"title": "Chapter 2", "scenes": [{"id": 102, "title": "Scene 1"}]}
        ]
        personal.import_scenes(*self.args, [101])
        result = personal.import_scenes(*self.args, [102])
        self.assertIn("1 linked", result)
        manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(manifest["chapters"], [
            {"title": "Chapter 1", "files": ["Scene 1.txt"]},
            {"title": "Chapter 2", "files": ["Scene 1 - 102.txt"]}
        ])
        self.assertEqual(self.usb.data["Scene 1.txt"], b"written on laptop")
        self.assertEqual(self.usb.data["Scene 1 - 102.txt"], b"another scene")

    def test_import_persists_desktop_chapter_scene_order_not_filename_order(self):
        self.db.layout = [
            {"title": "Chapter 2", "scenes": [{"id": 102, "title": "Scene 2"}]},
            {"title": "Chapter 1", "scenes": [{"id": 101, "title": "Scene 1"}]}
        ]
        personal.import_scenes(*self.args, [101, 102])
        mapping = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(mapping["chapters"], [
            {"title": "Chapter 2", "files": ["Scene 2.txt"]},
            {"title": "Chapter 1", "files": ["Scene 1.txt"]}
        ])
        self.assertEqual(self.usb.data["Scene 1.txt"], b"written on laptop")

    def test_reorder_metadata_preserves_revisions_and_prose(self):
        personal.import_scenes(*self.args, [101, 102])
        previous = self.usb.data.copy()
        previous_manifest = __import__("json").loads(previous[".sync"])
        self.db.layout = [{
            "title": "Reordered chapter", "scenes": [
                {"id": 102, "title": "Scene 2"},
                {"id": 101, "title": "Scene 1"}
            ]
        }]
        result = personal.refresh_chapter_order(*self.args)
        self.assertIn("Updated 1 chapter", result)
        after_manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(after_manifest["chapters"], [{
            "title": "Reordered chapter",
            "files": ["Scene 2.txt", "Scene 1.txt"]
        }])
        self.assertEqual(after_manifest["scenes"], previous_manifest["scenes"])
        self.assertEqual(self.usb.data["Scene 1.txt"], previous["Scene 1.txt"])
        self.assertEqual(self.usb.data["Scene 2.txt"], previous["Scene 2.txt"])
        self.assertIn("already up to date", personal.refresh_chapter_order(*self.args))

    def test_existing_link_can_add_outline_without_importing_again(self):
        personal.link_scene(*self.args, "Chapter One.txt", 101, "device")
        manifest = __import__("json").loads(self.usb.data[".sync"])
        self.assertEqual(manifest["chapters"], [
            {"title": "Chapter 1", "files": ["Chapter One.txt"]}
        ])

    def test_sync_refreshes_order_even_when_manuscripts_unchanged(self):
        personal.import_scenes(*self.args, [101, 102])
        previous = self.usb.data.copy()
        self.db.layout = [
            {"title": "New First", "scenes": [{"id": 102, "title": "Scene 2"}]},
            {"title": "New Second", "scenes": [{"id": 101, "title": "Scene 1"}]}
        ]
        output = personal.sync_action(*self.args, apply=True)
        self.assertIn("Refreshed E213 chapter and scene order", output)
        self.assertEqual(self.usb.data["Scene 1.txt"], previous["Scene 1.txt"])
        self.assertEqual(self.usb.data["Scene 2.txt"], previous["Scene 2.txt"])
        self.assertEqual(__import__("json").loads(self.usb.data[".sync"])["chapters"][0]["title"], "New First")

    def test_scene_title_to_safe_filename(self):
        self.assertEqual(personal.suggest_filename("Part One / Café -- Arrival"),
                         "Cafe -- Arrival.txt")
        self.assertTrue(bridge.NAME.fullmatch(personal.suggest_filename("Project / " + "A" * 70)))
        self.assertEqual(personal.suggest_filename("No symbols / 🌙"), "New Scene.txt")

    def test_importing_personal_sync_does_not_modify_plot_jot(self):
        self.assertTrue(callable(personal.sync_action))
        self.assertTrue(callable(personal.default_db_path))


if __name__ == "__main__":
    unittest.main()
