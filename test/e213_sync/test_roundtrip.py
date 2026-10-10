"""Simulated device/desktop round trips, including safety of conflicting work."""
import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

target = Path(__file__).resolve().parents[2] / "tools/e213-sync/plot_jot_sync.py"
spec = importlib.util.spec_from_file_location("bridge", target)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class DB:
    def __init__(self, filename, content):
        self.db_path = Path(filename)
        with self.get_connection() as con:
            con.executescript("""
                CREATE TABLE chapters (id INTEGER PRIMARY KEY, project_id INTEGER);
                CREATE TABLE scenes (id INTEGER PRIMARY KEY, chapter_id INTEGER, content TEXT);
                INSERT INTO chapters (id,project_id) VALUES (7,42);
            """)
            con.execute("INSERT INTO scenes VALUES (101,7,?)", (content,))

    def get_connection(self):
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def get_scene(self, scene_id):
        with self.get_connection() as con:
            return con.execute("SELECT * FROM scenes WHERE id=?", (scene_id,)).fetchone()

    def update_scene_content(self, scene_id, content):
        with self.get_connection() as con:
            con.execute("UPDATE scenes SET content=? WHERE id=?", (content, scene_id))


class USB:
    def __init__(self, docs):
        self.docs = dict(docs)

    def get(self, name):
        if name not in self.docs:
            raise RuntimeError("E213 refused command: not-found")
        data = self.docs[name]
        return data, bridge.checksum(data)

    def put(self, name, data, expected):
        if name in self.docs:
            if bridge.checksum(self.docs[name]) != expected:
                raise RuntimeError("E213 refused command: stale-version")
        elif expected != "*":
            raise RuntimeError("E213 refused command: stale-version")
        self.docs[name] = data


def manifest(original):
    return {
        "version": 1, "provider": "plot-and-jot", "project_id": 42,
        "project_name": "Novel", "scenes": {
            "Draft-001.txt": {
                "scene_id": 101, "chapter_id": 7,
                "revision": bridge.checksum(original.encode())
            }
        }
    }


class FullSyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "db.sqlite"

    def test_device_only_edit_updates_scene_with_backup(self):
        db = DB(self.path, "old")
        m = manifest("old")
        usb = USB({"Draft-001.txt": b"new on E213"})
        summary = bridge.synchronize(usb, db, 42, m, "*")
        self.assertEqual(summary["push"], 1)
        self.assertEqual(db.get_scene(101)["content"], "new on E213")
        self.assertTrue(list((self.path.parent / "backups/e213-sync").glob("*.db")))
        self.assertEqual(json.loads(usb.docs[".sync"])["scenes"]["Draft-001.txt"]["revision"],
                         bridge.checksum(b"new on E213"))

    def test_desktop_only_edit_updates_device(self):
        db = DB(self.path, "new on desktop")
        m = manifest("old")
        usb = USB({"Draft-001.txt": b"old"})
        summary = bridge.synchronize(usb, db, 42, m, "*")
        self.assertEqual(summary["pull"], 1)
        self.assertEqual(usb.docs["Draft-001.txt"], b"new on desktop")

    def test_conflict_preserves_both(self):
        db = DB(self.path, "changed on desktop")
        m = manifest("original")
        usb = USB({"Draft-001.txt": b"changed on E213"})
        summary = bridge.synchronize(usb, db, 42, m, "*")
        self.assertEqual(summary["conflict"], 1)
        self.assertEqual(db.get_scene(101)["content"], "changed on desktop")
        self.assertEqual(usb.docs["Draft-001.txt"], b"changed on E213")
        saved = list((self.path.parent / "backups/e213-conflicts").glob("*.txt"))
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].read_bytes(), b"changed on E213")
        self.assertNotIn(".sync", usb.docs)

    def test_missing_device_document_does_not_delete_scene(self):
        db = DB(self.path, "desktop version")
        summary = bridge.synchronize(USB({}), db, 42, manifest("original"), "*")
        self.assertEqual(summary["missing"], 1)
        self.assertEqual(db.get_scene(101)["content"], "desktop version")

    def test_wrong_chapter_mapping_aborts_before_writes(self):
        db = DB(self.path, "old")
        m = manifest("old")
        m["scenes"]["Draft-001.txt"]["chapter_id"] = 9999
        with self.assertRaises(ValueError):
            bridge.synchronize(USB({"Draft-001.txt": b"new"}), db, 42, m, "*")
        self.assertEqual(db.get_scene(101)["content"], "old")


if __name__ == "__main__":
    unittest.main()
