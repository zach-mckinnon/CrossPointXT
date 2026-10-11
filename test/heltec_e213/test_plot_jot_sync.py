import importlib.util
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("sync", Path(__file__).parents[2] / "scripts/plot_jot_sync.py")
sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync)


class SyncTest(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).parents[2] / ".pio" / "sync-tests"
        test_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=test_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root / "writer.sqlite3"
        with closing(sqlite3.connect(self.db)) as conn:
            # Fields and FK relationships from Plot & Jot's actual schema.
            conn.executescript("""
                CREATE TABLE projects(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT);
                CREATE TABLE chapters(id INTEGER PRIMARY KEY AUTOINCREMENT,project_id INTEGER REFERENCES projects(id),title TEXT,position INTEGER);
                CREATE TABLE scenes(id INTEGER PRIMARY KEY AUTOINCREMENT,chapter_id INTEGER REFERENCES chapters(id),title TEXT,position INTEGER,content TEXT,summary TEXT DEFAULT '',updated_at TEXT DEFAULT CURRENT_TIMESTAMP);
                INSERT INTO projects VALUES(1,'Novel');
                INSERT INTO chapters VALUES(1,1,'Chapter',1);
                INSERT INTO scenes(chapter_id,title,position,content,summary) VALUES(1,'Scene',1,'Original\n','Keep metadata');
            """)
        self.conn = sync.connect(self.db)
        self.addCleanup(self.conn.close)
        self.batch = sync.export_project(self.conn, 1, self.root)
        self.meta = self.batch / "scene-1.txt.plot"
        self.text = self.batch / "scene-1.txt"

    def edit(self, text):
        self.text.write_bytes(text.encode("utf-8"))

    def desktop_edit(self, text):
        with self.conn:
            self.conn.execute("UPDATE scenes SET content=? WHERE id=1", (text,))

    def content(self):
        return self.conn.execute("SELECT content FROM scenes WHERE id=1").fetchone()[0]

    def test_preview_then_round_trip_repeat_and_continued_writing(self):
        self.edit("Deck edit\n")
        self.assertEqual(sync.import_transfer(self.conn, self.meta)["result"], "update")
        self.assertEqual(self.content(), "Original\n")
        self.assertEqual(sync.import_transfer(self.conn, self.meta, True)["result"], "update")
        self.assertEqual(self.content(), "Deck edit\n")
        self.assertEqual(sync.import_transfer(self.conn, self.meta, True)["result"], "already-imported")
        self.edit("More writing\n")
        self.assertEqual(sync.import_transfer(self.conn, self.meta, True)["result"], "update")
        self.assertEqual(self.content(), "More writing\n")
        self.assertEqual(self.conn.execute("SELECT summary FROM scenes WHERE id=1").fetchone()[0], "Keep metadata")

    def test_conflict_keeps_both_once(self):
        self.edit("Deck edit")
        self.desktop_edit("Desktop edit")
        result = sync.import_transfer(self.conn, self.meta, True)
        self.assertEqual(result["result"], "conflict-copy")
        self.assertEqual(self.content(), "Desktop edit")
        self.assertEqual(self.conn.execute("SELECT content FROM scenes WHERE id=?", (result["target_scene_id"],)).fetchone()[0], "Deck edit")
        sync.import_transfer(self.conn, self.meta, True)
        self.assertEqual(self.conn.execute("SELECT COUNT(*) FROM scenes").fetchone()[0], 2)

    def test_unchanged_deck_never_rolls_back_desktop(self):
        self.desktop_edit("Desktop edit")
        self.assertEqual(sync.import_transfer(self.conn, self.meta, True)["result"], "unchanged-on-deck")
        self.assertEqual(self.content(), "Desktop edit")

    def test_identical_and_repeat_after_desktop_edit(self):
        self.edit("Same on both")
        self.desktop_edit("Same on both")
        self.assertEqual(sync.import_transfer(self.conn, self.meta, True)["result"], "identical")
        self.desktop_edit("Later desktop edit")
        sync.import_transfer(self.conn, self.meta, True)
        self.assertEqual(self.content(), "Later desktop edit")

    def test_reject_forged_ids_wrong_database_and_version(self):
        original = json.loads(self.meta.read_text())
        for field, value in [("scene_id", 22), ("project_id", 2), ("database_id", "elsewhere"),
                             ("version", 2), ("version", True), ("transfer_id", "f" * 36), ("base_sha256", "0" * 64)]:
            with self.subTest(field=field):
                meta = dict(original, **{field: value})
                self.meta.write_text(json.dumps(meta))
                with self.assertRaises(ValueError):
                    sync.import_transfer(self.conn, self.meta, True)
        self.assertEqual(self.content(), "Original\n")

    def test_unicode_empty_crlf_round_trip(self):
        for text in ["Café — 日本語 🌿\r\n", ""]:
            self.edit(text)
            sync.import_transfer(self.conn, self.meta, True)
            self.assertEqual(self.content(), text)

    def test_oversized_binary_and_invalid_utf8_rejected(self):
        for data in [b"x" * 65537, b"bad\0text", b"\xff"]:
            self.text.write_bytes(data)
            with self.assertRaises(ValueError):
                sync.import_transfer(self.conn, self.meta, True)
        self.assertEqual(self.content(), "Original\n")

    def test_deleted_scene_does_not_get_recreated_implicitly(self):
        with self.conn:
            self.conn.execute("DELETE FROM scenes")
        with self.assertRaises(ValueError):
            sync.import_transfer(self.conn, self.meta, True)

    def test_export_never_overwrites_unsynced_files(self):
        self.edit("Unsynced")
        second = sync.export_project(self.conn, 1, self.root)
        self.assertNotEqual(second, self.batch)
        self.assertEqual(self.text.read_text(), "Unsynced")

    def test_unlinked_adoption_explicit_destination_idempotent(self):
        draft = self.root / "new.txt"
        draft.write_text("New scene")
        with self.assertRaises(ValueError):
            sync.adopt(self.conn, draft, 99, 1, True)
        sync.adopt(self.conn, draft, 1, 1)
        self.assertEqual(self.conn.execute("SELECT COUNT(*) FROM scenes").fetchone()[0], 1)
        result = sync.adopt(self.conn, draft, 1, 1, True)
        self.assertEqual(result["result"], "adopted")
        self.assertEqual(sync.adopt(self.conn, draft, 1, 1, True)["result"], "already-imported")
        self.assertEqual(self.conn.execute("SELECT COUNT(*) FROM scenes").fetchone()[0], 2)

    def test_backup_is_readable_and_complete(self):
        backup = sync.backup_database(self.conn, self.db)
        with closing(sqlite3.connect(backup)) as conn:
            self.assertEqual(conn.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            self.assertEqual(conn.execute("SELECT content FROM scenes WHERE id=1").fetchone()[0], "Original\n")

    def test_bad_database_path_never_creates_database(self):
        missing = self.root / "missing.db"
        with self.assertRaises(sqlite3.OperationalError):
            sync.connect(missing)
        self.assertFalse(missing.exists())


if __name__ == "__main__":
    unittest.main()
