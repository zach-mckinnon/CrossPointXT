#!/usr/bin/env python3
"""USB manuscript sync between E213 internal flash and local Plot & Jot.

No server, cloud account, or network required. Explicit .sync scene IDs are the
only relationships; filenames/titles are never used to guess a destination.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import re
import sqlite3
import sys
import time
from pathlib import Path

PREFIX = "E213SYNC1|"
NAME = re.compile(r"^[A-Za-z0-9 _-]{1,44}\.txt$")
HASH = re.compile(r"^[a-f0-9]{64}$")
MAX_FILE = 65536
MAX_MANIFEST = 16384


def checksum(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decide_sync(base: str, device: str, desktop: str) -> str:
    """Optimistic three-way comparison; never overwrite two divergent edits."""
    if not all(HASH.fullmatch(x) for x in (base, device, desktop)):
        raise ValueError("Missing or invalid scene revision; manual relink required")
    if device == desktop:
        return "same"
    if desktop == base:
        return "push"
    if device == base:
        return "pull"
    return "conflict"


def validate_manifest(manifest: dict, project_id: int) -> None:
    if not isinstance(manifest, dict) or manifest.get("version") != 1:
        raise ValueError("Unsupported .sync manifest version")
    if manifest.get("provider") != "plot-and-jot" or manifest.get("project_id") != project_id:
        raise ValueError(".sync belongs to another project/provider: refusing automatic upload")
    scenes = manifest.get("scenes")
    if not isinstance(scenes, dict):
        raise ValueError("Manifest needs a scenes object")
    for filename, binding in scenes.items():
        if not NAME.fullmatch(filename) or not isinstance(binding, dict):
            raise ValueError("Unsafe filename or scene binding")
        if (type(binding.get("scene_id")) is not int or binding["scene_id"] <= 0
                or type(binding.get("chapter_id")) is not int or binding["chapter_id"] <= 0
                or not HASH.fullmatch(str(binding.get("revision", "")))):
            raise ValueError(f"Invalid binding for {filename}")


class USB:
    def __init__(self, port: str):
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("Install serial support: python -m pip install pyserial") from exc
        self.serial = serial.Serial(port, 115200, timeout=1, write_timeout=10)
        # Serial attach can reset an ESP32-S3. Skip startup diagnostics.
        time.sleep(2)
        self.send("HELLO")
        if self.next() != ["READY"]:
            raise RuntimeError("E213 sync protocol not available on this firmware")

    def close(self):
        self.serial.close()

    def send(self, *parts: str):
        line = PREFIX + "|".join(parts) + "\n"
        self.serial.write(line.encode("ascii"))
        self.serial.flush()

    def next(self, timeout=30) -> list[str]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().decode("ascii", errors="replace").strip()
            if not line.startswith(PREFIX):
                continue
            parts = line[len(PREFIX):].split("|")
            if parts[0] == "ERR":
                raise RuntimeError("E213 refused command: " + "|".join(parts[1:]))
            return parts
        raise TimeoutError("E213 response timed out; check cable/port and awake firmware")

    def files(self) -> dict[str, str]:
        self.send("LIST")
        files = {}
        while True:
            parts = self.next()
            if parts == ["END"]:
                return files
            if len(parts) != 4 or parts[0] != "FILE" or not NAME.fullmatch(parts[1]):
                raise RuntimeError("Malformed file listing")
            files[parts[1]] = parts[2]

    def get(self, name: str) -> tuple[bytes, str]:
        if name != ".sync" and not NAME.fullmatch(name):
            raise ValueError("Invalid E213 filename")
        self.send("GET", name)
        first = self.next()
        if len(first) != 3 or first[0] != "BEGIN":
            raise RuntimeError("Unexpected GET response")
        length = int(first[1])
        if length > (MAX_MANIFEST if name == ".sync" else MAX_FILE):
            raise ValueError("Excessive file length")
        result = bytearray()
        while True:
            part = self.next()
            if part == ["END"]:
                break
            if len(part) != 2 or part[0] != "DATA":
                raise RuntimeError("Bad data frame")
            result.extend(base64.b64decode(part[1], validate=True))
            if len(result) > length:
                raise RuntimeError("Oversized data transfer")
        data = bytes(result)
        if len(data) != length or checksum(data) != first[2]:
            raise RuntimeError("Transfer failed checksum verification")
        return data, first[2]

    def put(self, name: str, data: bytes, expected: str):
        if name != ".sync" and not NAME.fullmatch(name):
            raise ValueError("Invalid E213 filename")
        if len(data) > (MAX_MANIFEST if name == ".sync" else MAX_FILE):
            raise ValueError("File too large for E213")
        if expected != "*" and not HASH.fullmatch(expected):
            raise ValueError("Invalid expected revision")
        self.send("PUT", name, expected, str(len(data)))
        if self.next() != ["READY"]:
            raise RuntimeError("E213 refused PUT")
        for offset in range(0, len(data), 192):
            self.send("DATA", base64.b64encode(data[offset:offset+192]).decode("ascii"))
        self.send("END")
        if self.next(timeout=60) != ["SAVED"]:
            raise RuntimeError("E213 did not confirm saved data")


def get_manifest(usb: USB, project_id: int, name: str) -> tuple[dict, str]:
    try:
        data, revision = usb.get(".sync")
    except RuntimeError as err:
        if "not-found" not in str(err):
            raise
        return {
            "version": 1, "provider": "plot-and-jot",
            "project_id": project_id, "project_name": name, "scenes": {}
        }, "*"
    manifest = json.loads(data.decode("utf-8"))
    validate_manifest(manifest, project_id)
    return manifest, revision


def put_manifest(usb: USB, manifest: dict, previous: str):
    validate_manifest(manifest, manifest["project_id"])
    data = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")
    usb.put(".sync", data, previous)


def db_scene(db, project_id: int, scene_id: int, chapter_id: int | None = None):
    scene = db.get_scene(scene_id)
    if not scene:
        raise ValueError(f"Scene {scene_id} does not exist in Plot & Jot")
    with db.get_connection() as conn:
        matches = conn.execute(
            "SELECT 1 FROM chapters WHERE id=? AND project_id=?",
            (scene["chapter_id"], project_id)
        ).fetchone()
    if not matches or (chapter_id is not None and scene["chapter_id"] != chapter_id):
        raise ValueError(f"Scene {scene_id} does not belong to the specified project/chapter")
    return scene


def backup_db(db) -> Path:
    # sqlite backup() is WAL-aware and safe while the desktop DB is open.
    root = Path(db.db_path).parent / "backups" / "e213-sync"
    root.mkdir(parents=True, exist_ok=True)
    name = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    dest = root / f"before-e213-sync-{name}.db"
    with sqlite3.connect(db.db_path) as source, sqlite3.connect(dest) as target:
        source.backup(target)
    return dest


def save_conflict(db, filename: str, device: bytes):
    target = Path(db.db_path).parent / "backups" / "e213-conflicts"
    target.mkdir(parents=True, exist_ok=True)
    name = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    path = target / f"{name}-{filename}"
    path.write_bytes(device)
    return path


def synchronize(usb: USB, db, project_id: int, manifest: dict, manifest_hash: str):
    changed = False
    backed_up = False
    summary = {"push": 0, "pull": 0, "same": 0, "conflict": 0, "missing": 0}
    for filename, item in manifest["scenes"].items():
        scene = db_scene(db, project_id, item["scene_id"], item["chapter_id"])
        try:
            device_bytes, device_rev = usb.get(filename)
        except RuntimeError as exc:
            if "not-found" in str(exc):
                summary["missing"] += 1
                print(f"SKIP {filename}: absent on E213 (no delete or overwrite)")
                continue
            raise
        desktop_bytes = (scene["content"] or "").encode("utf-8")
        desktop_rev = checksum(desktop_bytes)
        action = decide_sync(item["revision"], device_rev, desktop_rev)
        if action == "push":
            # Recheck right before writing; never clobber a concurrent desktop edit.
            current = db_scene(db, project_id, item["scene_id"], item["chapter_id"])
            if checksum((current["content"] or "").encode("utf-8")) != desktop_rev:
                action = "conflict"
            else:
                if not backed_up:
                    print("Plot & Jot backup:", backup_db(db))
                    backed_up = True
                db.update_scene_content(item["scene_id"], device_bytes.decode("utf-8"))
                item["revision"] = device_rev
                changed = True
        elif action == "pull":
            usb.put(filename, desktop_bytes, device_rev)
            item["revision"] = desktop_rev
            changed = True
        elif action == "same" and item["revision"] != device_rev:
            item["revision"] = device_rev
            changed = True
        if action == "conflict":
            path = save_conflict(db, filename, device_bytes)
            print(f"CONFLICT {filename}: E213 copy preserved at {path}; Plot & Jot unchanged")
        else:
            print(f"{action.upper():8} {filename}")
        summary[action] += 1
    if changed:
        put_manifest(usb, manifest, manifest_hash)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial port, e.g. COM10")
    parser.add_argument("--plot-source", type=Path, required=True,
                        help="Path to local plot-and-jot-source checkout")
    parser.add_argument("--db", type=Path, help="Override Plot & Jot database path")
    parser.add_argument("--project", type=int, help="Existing Plot & Jot project ID")
    parser.add_argument("--list-projects", action="store_true")
    parser.add_argument("--list-scenes", action="store_true")
    parser.add_argument("--list-files", action="store_true")
    parser.add_argument("--bind", nargs=2, metavar=("E213_FILENAME", "SCENE_ID"),
                        help="Explicitly link an existing E213 document to a scene")
    parser.add_argument("--prefer", choices=("plot", "device"),
                        help="Which copy wins when first binding differing documents")
    parser.add_argument("--sync", action="store_true", help="Synchronize existing scene links")
    args = parser.parse_args(argv)

    sys.path.insert(0, str(args.plot_source.resolve()))
    from src.writer.database.database import DatabaseManager
    db = DatabaseManager(args.db)
    if args.list_projects:
        for project in db.get_projects():
            print(f"{project['id']:5}  {project['name']}")
        return
    if not args.project or not db.get_project(args.project):
        parser.error("--project must be an existing project ID (see --list-projects)")
    if args.list_scenes:
        for chapter in db.get_full_manuscript_data(args.project):
            for scene in chapter["scenes"]:
                print(f"{scene['id']:5}  [{chapter['title']}] {scene['title']}")
        return
    if not args.port:
        parser.error("--port is required for device operations")
    usb = USB(args.port)
    try:
        if args.list_files:
            for name in usb.files():
                print(name)
            return
        manifest, old_hash = get_manifest(usb, args.project, db.get_project(args.project)["name"])
        if args.bind:
            name, value = args.bind
            if not NAME.fullmatch(name):
                parser.error("Use a simple E213 filename ending in .txt")
            scene = db_scene(db, args.project, int(value))
            if name in manifest["scenes"]:
                parser.error(f"{name} is already linked; refuse accidental rebinding")
            if any(v["scene_id"] == scene["id"] for v in manifest["scenes"].values()):
                parser.error("Scene already linked to another file")
            device_bytes, device_hash = usb.get(name)
            desktop_hash = checksum((scene["content"] or "").encode("utf-8"))
            if desktop_hash != device_hash and not args.prefer:
                parser.error("Copies differ. Repeat with --prefer plot or --prefer device")
            # Baseline reflects the chosen authoritative copy. A follow-up sync
            # will change only the opposite side, never both without permission.
            initial = desktop_hash if args.prefer == "plot" else device_hash
            manifest["scenes"][name] = {
                "scene_id": scene["id"], "chapter_id": scene["chapter_id"],
                "revision": initial
            }
            put_manifest(usb, manifest, old_hash)
            manifest, old_hash = get_manifest(usb, args.project, db.get_project(args.project)["name"])
            print(f"LINKED {name} -> scene {scene['id']}")
        if args.sync or args.bind:
            print(synchronize(usb, db, args.project, manifest, old_hash))
        else:
            parser.error("Choose --sync, --bind, --list-files or a listing option")
    finally:
        usb.close()


if __name__ == "__main__":
    main()
