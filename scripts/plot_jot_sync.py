"""Offline Plot & Jot <-> E213 SD transfer. Standard-library-only companion tool.

Close Plot & Jot before applying imports. The SQLite database stays on the PC.
Compatible schema inspected at plot-and-jot-source commit 90a8fd135963924474a1c967aa8e6631d027228d.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
import uuid

FORMAT = "plot-and-jot.scene-transfer"
VERSION = 1
MAX_BYTES = 65536


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def text_bytes(path: Path) -> str:
    with path.open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f"Text exceeds the deck's 64 KiB limit: {path}")
    text = data.decode("utf-8")
    if any((ord(c) < 32 and c not in "\n\r\t") or ord(c) == 127 for c in text):
        raise ValueError(f"Binary/control bytes in text: {path}")
    return text


def connect(path: Path) -> sqlite3.Connection:
    # mode=rw prevents an incorrect path from silently creating an empty DB.
    conn = sqlite3.connect(path.resolve().as_uri() + "?mode=rw", uri=True, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys=ON")
    for table, columns in {
        "projects": {"id", "name"},
        "chapters": {"id", "project_id", "title", "position"},
        "scenes": {"id", "chapter_id", "title", "content", "position", "updated_at"},
    }.items():
        actual = {row["name"] for row in conn.execute(f"PRAGMA table_info({table})")}
        if not columns <= actual:
            conn.close()
            raise ValueError(f"Unsupported Plot & Jot database schema: {table}")
    return conn


def backup_database(conn: sqlite3.Connection, path: Path) -> Path:
    destination = path.with_name(path.name + ".deck-backup-" + uuid.uuid4().hex + ".sqlite3")
    with closing(sqlite3.connect(destination)) as backup:
        conn.backup(backup)
    return destination


def initialize(conn: sqlite3.Connection) -> str:
    # Separate namespace: never migrate or replace the application's tables.
    conn.execute("CREATE TABLE IF NOT EXISTS writer_deck_identity (id INTEGER PRIMARY KEY CHECK(id=1), uuid TEXT NOT NULL)")
    conn.execute("INSERT OR IGNORE INTO writer_deck_identity VALUES (1, ?)", (str(uuid.uuid4()),))
    conn.execute("""CREATE TABLE IF NOT EXISTS writer_deck_exports (
        token TEXT PRIMARY KEY, project_id INTEGER NOT NULL, chapter_id INTEGER NOT NULL,
        scene_id INTEGER NOT NULL, base_sha256 TEXT NOT NULL, accepted_sha256 TEXT NOT NULL)""")
    conn.execute("""CREATE TABLE IF NOT EXISTS writer_deck_receipts (
        token TEXT NOT NULL, sha256 TEXT NOT NULL, scene_id INTEGER NOT NULL,
        outcome TEXT NOT NULL, PRIMARY KEY(token, sha256))""")
    return conn.execute("SELECT uuid FROM writer_deck_identity WHERE id=1").fetchone()[0]


def identity(conn: sqlite3.Connection) -> str:
    try:
        row = conn.execute("SELECT uuid FROM writer_deck_identity WHERE id=1").fetchone()
    except sqlite3.OperationalError as error:
        raise ValueError("This database has no deck exports. Export from this database first.") from error
    if not row:
        raise ValueError("Missing database identity")
    return row[0]


def atomic_write(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def export_project(conn: sqlite3.Connection, project_id: int, card: Path) -> Path:
    if not card.is_dir():
        raise ValueError("SD destination must be an existing mounted directory")
    project = conn.execute("SELECT name FROM projects WHERE id=?", (project_id,)).fetchone()
    if not project:
        raise ValueError("Unknown project")
    scenes = conn.execute("""SELECT s.*, c.project_id, c.title AS chapter_title
        FROM scenes s JOIN chapters c ON s.chapter_id=c.id WHERE c.project_id=?
        ORDER BY c.position, s.position, s.id""", (project_id,)).fetchall()
    # Validate the entire batch before writing any package or registry entry.
    for scene in scenes:
        content = scene["content"] or ""
        if len(content.encode("utf-8")) > MAX_BYTES or any(
            (ord(c) < 32 and c not in "\n\r\t") or ord(c) == 127 for c in content
        ):
            raise ValueError(f"Scene {scene['id']} is not deck-compatible text (64 KiB maximum)")
    # Every export is a new directory; never overwrite unsynced SD edits.
    batch = card / "PlotJot" / f"p{project_id}-{uuid.uuid4().hex[:8]}"
    batch.mkdir(parents=True, exist_ok=False)
    with conn:
        db_id = initialize(conn)
        for scene in scenes:
            token = str(uuid.uuid4())
            content = scene["content"] or ""
            base = digest(content)
            path = batch / f"scene-{scene['id']}.txt"
            metadata = {
                "format": FORMAT, "version": VERSION, "database_id": db_id,
                "transfer_id": token, "project_id": project_id,
                "chapter_id": scene["chapter_id"], "scene_id": scene["id"],
                "project_title": project["name"], "chapter_title": scene["chapter_title"],
                "scene_title": scene["title"], "base_sha256": base,
                "encoding": "utf-8",
            }
            conn.execute("INSERT INTO writer_deck_exports VALUES (?, ?, ?, ?, ?, ?)",
                         (token, project_id, scene["chapter_id"], scene["id"], base, base))
            atomic_write(path, content.encode("utf-8"))
            atomic_write(Path(str(path) + ".plot"), json.dumps(metadata, ensure_ascii=False, indent=2).encode("utf-8"))
    return batch


def read_transfer(conn: sqlite3.Connection, metadata_path: Path):
    if not metadata_path.name.endswith(".txt.plot") or metadata_path.stat().st_size > 16384:
        raise ValueError(f"Invalid metadata file: {metadata_path}")
    meta = json.loads(metadata_path.read_text(encoding="utf-8"))
    if not isinstance(meta, dict) or meta.get("format") != FORMAT or type(meta.get("version")) is not int or meta["version"] != VERSION:
        raise ValueError("Unsupported .plot transfer format/version")
    if meta.get("database_id") != identity(conn) or meta.get("encoding") != "utf-8":
        raise ValueError("Transfer belongs to another database or uses unsupported encoding")
    token = meta.get("transfer_id")
    if not isinstance(token, str) or len(token) != 36:
        raise ValueError("Invalid transfer identity")
    row = conn.execute("SELECT * FROM writer_deck_exports WHERE token=?", (token,)).fetchone()
    if not row:
        raise ValueError("Unrecognized transfer; metadata alone cannot link arbitrary text to a scene")
    for field in ("project_id", "chapter_id", "scene_id", "base_sha256"):
        if type(meta.get(field)) is not type(row[field]) or meta[field] != row[field]:
            raise ValueError(f"Transfer metadata does not match its export: {field}")
    # Derive text path from the sidecar filename; never follow a path in JSON.
    text_path = metadata_path.with_suffix("")
    if text_path.is_symlink() or metadata_path.is_symlink():
        raise ValueError("Transfer files must not be symbolic links")
    return row, text_bytes(text_path)


def classify(conn: sqlite3.Connection, export, incoming: str):
    scene = conn.execute("""SELECT s.*, c.project_id FROM scenes s
        JOIN chapters c ON c.id=s.chapter_id WHERE s.id=?""", (export["scene_id"],)).fetchone()
    if not scene or scene["project_id"] != export["project_id"] or scene["chapter_id"] != export["chapter_id"]:
        raise ValueError("Scene was deleted or moved; choose a destination explicitly with adopt")
    incoming_hash = digest(incoming)
    current_hash = digest(scene["content"] or "")
    receipt = conn.execute("SELECT outcome, scene_id FROM writer_deck_receipts WHERE token=? AND sha256=?",
                           (export["token"], incoming_hash)).fetchone()
    if receipt:
        return "already-imported", scene, receipt["scene_id"]
    if incoming_hash == current_hash:
        return "identical", scene, scene["id"]
    if incoming_hash == export["accepted_sha256"] or incoming_hash == export["base_sha256"]:
        return "unchanged-on-deck", scene, scene["id"]
    if current_hash == export["accepted_sha256"]:
        return "update", scene, scene["id"]
    return "conflict-copy", scene, None


def import_transfer(conn: sqlite3.Connection, metadata_path: Path, apply: bool = False) -> dict:
    export, incoming = read_transfer(conn, metadata_path)
    if not apply:
        outcome, scene, target = classify(conn, export, incoming)
        return {"file": str(metadata_path), "scene_id": scene["id"], "result": outcome, "target_scene_id": target}
    # Recheck the export and destination under a write lock before applying.
    conn.execute("BEGIN IMMEDIATE")
    try:
        export, incoming = read_transfer(conn, metadata_path)
        outcome, scene, target = classify(conn, export, incoming)
        if outcome == "update":
            conn.execute("UPDATE scenes SET content=?, updated_at=CURRENT_TIMESTAMP WHERE id=?", (incoming, scene["id"]))
        elif outcome == "conflict-copy":
            position = conn.execute("SELECT COALESCE(MAX(position), 0)+1 FROM scenes WHERE chapter_id=?", (scene["chapter_id"],)).fetchone()[0]
            cursor = conn.execute("""INSERT INTO scenes (chapter_id, title, position, content)
                VALUES (?, ?, ?, ?)""", (scene["chapter_id"], scene["title"] + " [Deck conflict]", position, incoming))
            target = cursor.lastrowid
        if outcome in {"identical", "update"}:
            conn.execute("UPDATE writer_deck_exports SET accepted_sha256=? WHERE token=?", (digest(incoming), export["token"]))
        if outcome in {"identical", "update", "conflict-copy"}:
            conn.execute("INSERT INTO writer_deck_receipts VALUES (?, ?, ?, ?)", (export["token"], digest(incoming), target, outcome))
        conn.commit()
    except BaseException:
        conn.rollback()
        raise
    return {"file": str(metadata_path), "scene_id": scene["id"], "result": outcome, "target_scene_id": target}


def adopt(conn: sqlite3.Connection, file: Path, project_id: int, chapter_id: int, apply: bool = False) -> dict:
    text = text_bytes(file)
    chapter = conn.execute("SELECT id FROM chapters WHERE id=? AND project_id=?", (chapter_id, project_id)).fetchone()
    if not chapter:
        raise ValueError("Chapter does not belong to the selected project")
    if not apply:
        return {"file": str(file), "result": "new-scene", "project_id": project_id, "chapter_id": chapter_id}
    conn.execute("BEGIN IMMEDIATE")
    try:
        initialize(conn)
        token = f"adopt:{project_id}:{chapter_id}:" + hashlib.sha256(str(file.resolve()).encode()).hexdigest()
        receipt = conn.execute("SELECT scene_id FROM writer_deck_receipts WHERE token=? AND sha256=?", (token, digest(text))).fetchone()
        if receipt:
            result = {"result": "already-imported", "scene_id": receipt[0]}
        else:
            position = conn.execute("SELECT COALESCE(MAX(position),0)+1 FROM scenes WHERE chapter_id=?", (chapter_id,)).fetchone()[0]
            cursor = conn.execute("INSERT INTO scenes (chapter_id,title,position,content) VALUES (?,?,?,?)", (chapter_id, file.stem, position, text))
            conn.execute("INSERT INTO writer_deck_receipts VALUES (?,?,?,?)", (token, digest(text), cursor.lastrowid, "adopted"))
            result = {"result": "adopted", "scene_id": cursor.lastrowid}
        conn.commit()
        return result
    except BaseException:
        conn.rollback()
        raise


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, required=True, help="Plot & Jot SQLite database on the PC")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="List project/chapter/scene IDs")
    export = commands.add_parser("export", help="Export a project into a new folder on SD")
    export.add_argument("--project", type=int, required=True)
    export.add_argument("--sd", type=Path, required=True)
    imp = commands.add_parser("import", help="Preview or apply linked SD changes")
    imp.add_argument("source", type=Path, help=".txt.plot file or exported project folder")
    imp.add_argument("--apply", action="store_true", help="Apply after closing Plot & Jot; creates a DB backup")
    adopt_cmd = commands.add_parser("adopt", help="Import an unlinked deck draft as a new scene")
    adopt_cmd.add_argument("file", type=Path)
    adopt_cmd.add_argument("--project", required=True, type=int)
    adopt_cmd.add_argument("--chapter", required=True, type=int)
    adopt_cmd.add_argument("--apply", action="store_true")
    args = parser.parse_args(argv)
    conn = None
    try:
        conn = connect(args.db)
        if args.command == "list":
            for row in conn.execute("""SELECT p.id AS project_id,p.name,c.id AS chapter_id,c.title AS chapter,
                s.id AS scene_id,s.title AS scene FROM projects p LEFT JOIN chapters c ON c.project_id=p.id
                LEFT JOIN scenes s ON s.chapter_id=c.id ORDER BY p.id,c.position,s.position"""):
                print(json.dumps(dict(row), ensure_ascii=False))
        elif args.command == "export":
            print("Backup:", backup_database(conn, args.db))
            print("Exported:", export_project(conn, args.project, args.sd))
        elif args.command == "import":
            paths = sorted(args.source.rglob("*.txt.plot")) if args.source.is_dir() else [args.source]
            if not paths:
                raise ValueError("No .txt.plot metadata found; use adopt for unlinked drafts")
            # Validate every entry first. A malformed batch cannot partially import.
            for path in paths:
                import_transfer(conn, path)
            if args.apply:
                print("Backup:", backup_database(conn, args.db))
            for path in paths:
                print(json.dumps(import_transfer(conn, path, args.apply), ensure_ascii=False))
        elif args.command == "adopt":
            adopt(conn, args.file, args.project, args.chapter)
            if args.apply:
                print("Backup:", backup_database(conn, args.db))
            print(json.dumps(adopt(conn, args.file, args.project, args.chapter, args.apply)))
        return 0
    except (OSError, ValueError, sqlite3.Error) as error:
        print(f"Sync stopped: {error}", file=sys.stderr)
        return 1
    finally:
        if conn is not None:
            conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
