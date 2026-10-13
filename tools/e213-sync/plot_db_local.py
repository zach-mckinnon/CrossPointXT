"""Small SQLite adapter for a personal E213 sync window."""
import sqlite3
from pathlib import Path

class LocalPlotDatabase:
    def __init__(self, db_path):
        self.db_path = Path(db_path).expanduser().resolve()
        if not self.db_path.is_file():
            raise FileNotFoundError(f"Plot n Jot database not found: {self.db_path}")
        with self.get_connection() as conn:
            names = {row[0] for row in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if not {"projects", "chapters", "scenes"}.issubset(names):
            raise ValueError("This is not a Plot n Jot project database.")

    def get_connection(self):
        conn = sqlite3.connect(str(self.db_path), timeout=5)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys=ON")
        conn.execute("PRAGMA busy_timeout=5000")
        return conn

    def get_projects(self):
        with self.get_connection() as conn:
            return conn.execute("SELECT * FROM projects ORDER BY name COLLATE NOCASE").fetchall()

    def get_project(self, project_id):
        with self.get_connection() as conn:
            return conn.execute("SELECT * FROM projects WHERE id=?", (project_id,)).fetchone()

    def get_scene(self, scene_id):
        with self.get_connection() as conn:
            return conn.execute("SELECT s.*, c.title AS chapter_title FROM scenes s JOIN chapters c ON c.id=s.chapter_id WHERE s.id=?", (scene_id,)).fetchone()

    def get_full_manuscript_data(self, project_id):
        with self.get_connection() as conn:
            rows = conn.execute("SELECT c.id AS chapter_id, c.title AS chapter_title, c.position AS chapter_position, s.id AS scene_id, s.title AS scene_title, s.position AS scene_position, s.content AS scene_content FROM chapters c LEFT JOIN scenes s ON s.chapter_id=c.id WHERE c.project_id=? ORDER BY c.position,c.id,s.position,s.id", (project_id,)).fetchall()
        chapters = []
        for row in rows:
            if not chapters or chapters[-1]["chapter_id"] != row["chapter_id"]:
                chapters.append({"chapter_id": row["chapter_id"], "title": row["chapter_title"], "scenes": []})
            if row["scene_id"] is not None:
                chapters[-1]["scenes"].append({"id": row["scene_id"], "title": row["scene_title"], "content": row["scene_content"]})
        return chapters

    def update_scene_content(self, scene_id, content):
        with self.get_connection() as conn:
            result = conn.execute("UPDATE scenes SET content=?,updated_at=CURRENT_TIMESTAMP WHERE id=?", (content,scene_id))
            if result.rowcount != 1:
                raise ValueError(f"Scene {scene_id} was deleted during sync.")
            conn.commit()
