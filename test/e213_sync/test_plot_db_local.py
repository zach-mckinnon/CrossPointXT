"""Offline tests for Plot n Jot SQLite access without importing/changing its app."""
import sqlite3
import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/e213-sync"))
from plot_db_local import LocalPlotDatabase


class LocalDatabaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.filename = Path(self.temp.name) / "writer_data.db"
        with sqlite3.connect(self.filename) as con:
            con.executescript("""
                CREATE TABLE projects (id INTEGER PRIMARY KEY, name TEXT);
                CREATE TABLE chapters (id INTEGER PRIMARY KEY, project_id INTEGER,
                                       title TEXT, position INTEGER);
                CREATE TABLE scenes (id INTEGER PRIMARY KEY, chapter_id INTEGER,
                                     title TEXT, position INTEGER, content TEXT,
                                     updated_at TEXT);
                INSERT INTO projects VALUES (1, 'My Manuscript');
                INSERT INTO chapters VALUES (7, 1, 'Opening', 1);
                INSERT INTO scenes VALUES (101, 7, 'First Scene', 1, 'Initial', 'old');
            """)
        self.db = LocalPlotDatabase(self.filename)

    def test_projects_and_chapter_scene_discovery(self):
        self.assertEqual(self.db.get_projects()[0]["name"], "My Manuscript")
        self.assertEqual(self.db.get_project(1)["id"], 1)
        chapters = self.db.get_full_manuscript_data(1)
        self.assertEqual(chapters[0]["title"], "Opening")
        self.assertEqual(chapters[0]["scenes"][0]["id"], 101)
        self.assertEqual(self.db.get_scene(101)["chapter_title"], "Opening")

    def test_scene_content_edit_does_not_modify_other_records(self):
        self.db.update_scene_content(101, "Updated from E213")
        self.assertEqual(self.db.get_scene(101)["content"], "Updated from E213")
        self.assertEqual(self.db.get_projects()[0]["name"], "My Manuscript")
        with self.assertRaises(ValueError):
            self.db.update_scene_content(999, "Lost")

    def test_missing_database_is_not_created(self):
        target = Path(self.temp.name) / "missing.db"
        with self.assertRaises(FileNotFoundError):
            LocalPlotDatabase(target)
        self.assertFalse(target.exists())

    def test_wrong_schema_is_not_treated_as_plot_jot(self):
        target = Path(self.temp.name) / "other.db"
        with sqlite3.connect(target) as conn:
            conn.execute("CREATE TABLE unrelated (a INTEGER)")
        with self.assertRaises(ValueError):
            LocalPlotDatabase(target)


if __name__ == "__main__":
    unittest.main()
