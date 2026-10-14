#!/usr/bin/env python3
"""USB manuscript sync between the E213 microSD and local Plot & Jot.

No server, cloud account, or network required. Explicit .sync scene IDs are the
only relationships; filenames/titles are never used to guess a destination.
"""
from __future__ import annotations

import argparse
import base64
import binascii
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
MAX_FILE = 128 * 1024  # RAM-safe per-scene transfer cap; SD card storage is larger
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


def validate_store(store: dict) -> None:
    """Validate all project links, including global filename ownership."""
    if not isinstance(store, dict) or store.get("provider") != "plot-and-jot":
        raise ValueError("Unsupported sync provider")
    if store.get("version") == 1:
        validate_manifest(store, store.get("project_id"))
        return
    if store.get("version") != 2 or not isinstance(store.get("projects"), list):
        raise ValueError("Unsupported .sync manifest version")
    if not store["projects"]:
        raise ValueError("Multi-project manifest needs projects")
    used_ids, used_names = set(), set()
    for project in store["projects"]:
        if not isinstance(project, dict):
            raise ValueError("Invalid project in .sync manifest")
        project_id = project.get("project_id")
        if type(project_id) is not int or project_id <= 0 or project_id in used_ids:
            raise ValueError("Duplicate or invalid project ID in .sync")
        used_ids.add(project_id)
        view = dict(project, version=1, provider="plot-and-jot")
        validate_manifest(view, project_id)
        if not isinstance(project.get("project_name", ""), str):
            raise ValueError("Invalid project name in .sync")
        for filename in project["scenes"]:
            folded = filename.casefold()
            if folded in used_names:
                raise ValueError(
                    f"Document {filename} is linked to multiple projects; refusing")
            used_names.add(folded)
        chapters = project.get("chapters", [])
        if not isinstance(chapters, list):
            raise ValueError("Invalid project chapter outline")
        seen = set()
        for chapter in chapters:
            if (not isinstance(chapter, dict)
                    or not isinstance(chapter.get("title"), str)
                    or not isinstance(chapter.get("files"), list)):
                raise ValueError("Invalid chapter metadata")
            for filename in chapter["files"]:
                if not isinstance(filename, str) or filename not in project["scenes"]:
                    raise ValueError("Chapter references an unlinked document")
                if filename in seen:
                    raise ValueError("A scene appears twice in the outline")
                seen.add(filename)


def manifest_payload(manifest: dict) -> bytes:
    """Encode selected project plus untouched others, without internal keys."""
    validate_manifest(manifest, manifest["project_id"])
    current = {k: manifest[k] for k in (
        "project_id", "project_name", "scenes", "chapters"
    ) if k in manifest}
    others = manifest.get("_all_projects")
    if others is None:
        # Keep the original v1 on-disk format until another project is added.
        store = dict(current, version=1, provider="plot-and-jot")
    else:
        # Keep project positions stable when one project is re-synced.
        # New projects go at the end, existing projects retain their order.
        projects = [dict(p) for p in others]
        found = False
        for idx, project in enumerate(projects):
            if project.get("project_id") == current["project_id"]:
                projects[idx] = current
                found = True
                break
        if not found:
            projects.append(current)
        store = {"version": 2, "provider": "plot-and-jot", "projects": projects}
    validate_store(store)
    content = json.dumps(store, ensure_ascii=False, indent=2).encode("utf-8")
    if len(content) > MAX_MANIFEST:
        raise ValueError(
            f"Combined project index exceeds E213's {MAX_MANIFEST}-byte limit. "
            "No document or manifest was modified."
        )
    return content


def all_linked_filenames(manifest: dict) -> set[str]:
    """All filenames owned by ANY project, for cross-project collision safety."""
    names = set(manifest.get("scenes", {}))
    for project in manifest.get("_all_projects", []):
        names.update(project.get("scenes", {}))
    return {name.casefold() for name in names}


class USB:
    def __init__(self, port: str):
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("Install serial support: python -m pip install pyserial") from exc
        self.serial = serial.Serial(port, 115200, timeout=1, write_timeout=10)
        # Opening native ESP32 USB CDC can reset the E213 while the e-ink panel
        # and BLE stack initialize. Never depend on one HELLO after a fixed
        # two-second delay: retry the idempotent handshake until firmware replies.
        # A USB sync session pauses keyboard discovery on the writer.
        try:
            deadline = time.monotonic() + 55
            while time.monotonic() < deadline:
                self.send("HELLO")
                try:
                    reply = self.next(timeout=min(4, max(1, deadline - time.monotonic())))
                except TimeoutError:
                    continue
                except RuntimeError as exc:
                    # A previous connection may have left an uncommitted PUT
                    # open. Its delayed timeout must not poison the next
                    # idempotent HELLO handshake.
                    if str(exc) in (
                        "E213 refused command: timeout",
                        "E213 refused command: download-timeout",
                    ):
                        continue
                    raise
                if reply == ["READY"]:
                    break
                raise RuntimeError("E213 responded with an unsupported USB sync protocol")
            else:
                raise TimeoutError(
                    "E213 did not answer USB HELLO after repeated attempts. "
                    "Hold external B1 for 1.2 seconds to enter USB SYNC on the "
                    "writer; check that USB data (not just power) works and that "
                    "the correct serial port is selected. Close serial monitors."
                )
        except Exception:
            self.serial.close()
            raise

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

    def storage(self) -> tuple[int, int]:
        """Read FAT usable capacity and currently allocated bytes (read-only)."""
        self.send("SPACE")
        try:
            response = self.next(timeout=15)
        except RuntimeError as exc:
            if "unknown-command" in str(exc):
                raise RuntimeError(
                    "E213 firmware does not support SD storage statistics yet. "
                    "Flash the latest dev heltec_e213_writer firmware."
                ) from exc
            raise
        if len(response) != 3 or response[0] != "SPACE":
            raise RuntimeError("E213 returned an invalid SD capacity response")
        try:
            if not all(value.isascii() and value.isdecimal()
                       for value in response[1:]):
                raise ValueError("nondecimal byte count")
            total, used = map(int, response[1:])
        except ValueError as exc:
            raise RuntimeError("E213 returned a nonnumeric SD capacity") from exc
        if total <= 0 or used < 0 or used > total or total > (1 << 50):
            raise RuntimeError("E213 returned inconsistent SD capacity")
        return total, used

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
        """Read-only GET2 with one clean retry for a lost ACK/idle timeout.

        No partial response is returned or written to disk: a retry must
        restart from byte zero and pass full SHA-256 validation.
        """
        for attempt in range(2):
            try:
                return self._get_once(name)
            except (RuntimeError, TimeoutError) as exc:
                detail = str(exc)
                transient = (
                    "download-timeout" in detail
                    or "download-protocol" in detail
                    or "stalled after" in detail
                    or "response timed out" in detail
                )
                if not transient:
                    raise
                if attempt:
                    raise RuntimeError(
                        f"E213 download {name} failed twice despite a fresh "
                        f"read-only connection reset: {detail}. "
                        "No partial file was accepted; keep the SD originals."
                    ) from exc
                try:
                    self.reset_download_session()
                except Exception as reset_exc:
                    raise RuntimeError(
                        f"E213 read-only download {name} could not be "
                        f"restarted after: {detail}. "
                        f"USB session reset failed: {reset_exc}. "
                        "No partial document was accepted or uploaded."
                    ) from exc
        raise AssertionError("GET2 retry attempts exhausted")

    def reset_download_session(self):
        """Reset an incomplete in-memory GET2 without touching SD content."""
        # Drop obsolete responses before asking the firmware to clear the
        # old GET2. Always issue HELLO; serial buffer clear alone cannot
        # reset the device's firmware-side transfer state.
        if hasattr(self.serial, "reset_input_buffer"):
            self.serial.reset_input_buffer()
        self.send("HELLO")
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                reply = self.next(timeout=min(3, max(1, deadline - time.monotonic())))
            except TimeoutError:
                continue
            except RuntimeError as exc:
                # A delayed ACK from the timed-out transfer may have
                # been parsed after the device cleared its GET2 state.
                # In that state the firmware returns unknown-command
                # to the old ACK, then replies READY to our HELLO.
                # Ignore only these stale reset-window errors.
                if any(code in str(exc) for code in (
                    "download-timeout", "unknown-command",
                    "unexpected-ack", "download-protocol",
                )):
                    continue
                raise
            if reply == ["READY"]:
                return
            # Ignore any response from the cancelled GET2; only READY
            # establishes a synchronized new request boundary.
        raise TimeoutError("E213 HELLO reset did not return READY")

    def _get_once(self, name: str) -> tuple[bytes, str]:
        if name != ".sync" and not NAME.fullmatch(name):
            raise ValueError("Invalid E213 filename")
        # Legacy GET streams 272-byte DATA lines without waiting for the host,
        # overflowing some ESP32-S3 CDC queues. GET2 sends one 48-byte DATA
        # line only after the host ACKs each previous line. It requires the
        # matching E213 firmware; don't silently fall back to corrupt GET.
        # The first read can occasionally contain a delayed READY or END
        # from a previous serial command. If so, reset ONLY the uncommitted
        # in-RAM GET2 state with HELLO and retry one time. Both requests
        # are read-only; neither touches SD documents or mappings.
        unexpected = None
        for attempt in range(2):
            self.send("GET2", name)
            try:
                first = self.next()
            except RuntimeError as exc:
                if "unknown-command" in str(exc):
                    raise RuntimeError(
                        "E213 firmware is too old for reliable USB downloads. "
                        "Flash the latest dev heltec_e213_writer firmware, then "
                        "reopen E213 Personal Sync. No SD format is needed."
                    ) from exc
                raise
            if len(first) == 3 and first[0] == "BEGIN" and HASH.fullmatch(first[2]):
                break
            # Do not expose a document's Base64 bytes in error dialogs.
            kind = (first[0][:24] if first else "<empty>")
            unexpected = f"{kind}, {len(first)} field(s)"
            if attempt == 1:
                raise RuntimeError(
                    f"E213 GET2 returned an invalid header for {name} "
                    f"(received {unexpected}; retry also failed). "
                    "Nothing was written to the SD card. "
                    "Keep the original files and report this diagnostic."
                )
            self.send("HELLO")
            deadline = time.monotonic() + 10
            while True:
                try:
                    reset_reply = self.next(timeout=max(1, deadline - time.monotonic()))
                except TimeoutError as exc:
                    raise RuntimeError(
                        f"E213 GET2 returned an invalid header for {name} "
                        f"(received {unexpected}); HELLO reset timed out. "
                        "Neither document nor mapping was modified."
                    ) from exc
                if reset_reply == ["READY"]:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        f"E213 GET2 returned an invalid header for {name} "
                        f"(received {unexpected}); reset returned unexpected messages. "
                        "Neither document nor mapping was modified."
                    )
        try:
            length = int(first[1])
        except ValueError as exc:
            raise RuntimeError(f"E213 GET2 returned invalid file size for {name}") from exc
        if length < 0 or length > (MAX_MANIFEST if name == ".sync" else MAX_FILE):
            raise ValueError(f"E213 GET2 file size out of bounds for {name}")
        result = bytearray()
        # The first ACK requests the first data frame. Each subsequent ACK
        # requests exactly one more frame; the final ACK receives END.
        self.send("ACK")
        while True:
            try:
                part = self.next(timeout=65)
            except TimeoutError as exc:
                raise RuntimeError(
                    f"E213 download {name} stalled after {len(result)}/{length} "
                    "bytes. Reconnect and rescan before retrying."
                ) from exc
            if part == ["END"]:
                break
            if len(part) != 2 or part[0] != "DATA":
                raise RuntimeError(
                    f"E213 download {name}: malformed DATA frame after "
                    f"{len(result)}/{length} bytes"
                )
            encoded = part[1]
            try:
                chunk = base64.b64decode(encoded, validate=True)
            except (binascii.Error, ValueError) as exc:
                raise RuntimeError(
                    f"E213 download {name}: corrupt Base64 DATA after "
                    f"{len(result)}/{length} bytes; do not trust this "
                    "transfer or overwrite either copy"
                ) from exc
            if not chunk or len(chunk) > 24 or len(result) + len(chunk) > length:
                raise RuntimeError(
                    f"E213 download {name}: invalid DATA length after "
                    f"{len(result)}/{length} bytes"
                )
            result.extend(chunk)
            self.send("ACK")
        data = bytes(result)
        if len(data) != length or checksum(data) != first[2]:
            raise RuntimeError(
                f"E213 download {name}: SHA-256 or length verification failed "
                f"({len(data)}/{length} bytes). No data was accepted."
            )
        return data, first[2]

    def put(self, name: str, data: bytes, expected: str):
        if name != ".sync" and not NAME.fullmatch(name):
            raise ValueError("Invalid E213 filename")
        if len(data) > (MAX_MANIFEST if name == ".sync" else MAX_FILE):
            raise ValueError("File too large for E213")
        if expected != "*" and not HASH.fullmatch(expected):
            raise ValueError("Invalid expected revision")
        phase = "start"
        sent = 0
        try:
            self.send("PUT", name, expected, str(len(data)))
            if self.next(timeout=65) != ["READY"]:
                raise RuntimeError("E213 refused PUT")
            # IMPORTANT: ESP32-S3 USB CDC has a small firmware-side RX
            # queue. The previous 192-byte binary frame expanded to a
            # 272-byte ASCII line after Base64 and protocol framing, which
            # could exceed a 256-byte receive buffer before loop() drained it.
            # 24 raw bytes produce a 48-byte total framed USB line, fitting
            # even a 64-byte queue. Stop-and-wait ACK keeps transfers lossless.
            chunk_bytes = 24
            for offset in range(0, len(data), chunk_bytes):
                phase = "data"
                self.send("DATA", base64.b64encode(
                    data[offset:offset + chunk_bytes]).decode("ascii"))
                if self.next(timeout=65) != ["ACK"]:
                    raise RuntimeError("E213 did not acknowledge data chunk")
                sent = min(len(data), offset + chunk_bytes)
            phase = "commit"
            self.send("END")
            if self.next(timeout=65) != ["SAVED"]:
                raise RuntimeError("E213 did not confirm saved data")
        except (TimeoutError, RuntimeError, OSError) as exc:
            raise RuntimeError(
                f"E213 upload {name} failed during {phase} "
                f"({sent}/{len(data)} bytes acknowledged): {exc}. "
                "The previous file and any earlier completed imports remain "
                "protected. Rescan before retrying."
            ) from exc


def get_manifest(usb: USB, project_id: int, name: str) -> tuple[dict, str]:
    """Return one project's view, retaining all other project bindings."""
    current = {
        "version": 1, "provider": "plot-and-jot",
        "project_id": project_id, "project_name": name, "scenes": {}
    }
    try:
        data, revision = usb.get(".sync")
    except RuntimeError as err:
        if "not-found" not in str(err):
            raise
        return current, "*"
    store = json.loads(data.decode("utf-8"))
    validate_store(store)
    if store["version"] == 1:
        if store["project_id"] == project_id:
            return store, revision
        # Adding a second project migrates the index only at PUT. The first
        # project's scene IDs, original filename links, and revisions remain.
        current["_all_projects"] = [{
            k: store[k] for k in ("project_id", "project_name", "scenes", "chapters")
            if k in store
        }]
        return current, revision
    projects = store["projects"]
    selected = next((p for p in projects if p["project_id"] == project_id), current)
    current.update(selected)
    current["_all_projects"] = projects
    return current, revision


def put_manifest(usb: USB, manifest: dict, previous: str):
    usb.put(".sync", manifest_payload(manifest), previous)


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


def backup_device(usb: USB, destination: Path) -> Path:
    """Create a SHA-256 verified, read-only copy of all exposed SD drafts."""
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    folder = destination / f"e213-sd-backup-{stamp}"
    folder.mkdir(parents=True, exist_ok=False)
    for name, revision in usb.files().items():
        if not NAME.fullmatch(name) or not HASH.fullmatch(revision):
            raise RuntimeError(f"Unsafe filename or revision: {name!r}")
        data, fetched_revision = usb.get(name)
        if fetched_revision != revision:
            raise RuntimeError(f"{name} changed during backup; retry with writing stopped")
        copy = folder / name
        copy.write_bytes(data)
        if checksum(copy.read_bytes()) != revision:
            raise RuntimeError(f"Local backup verification failed for {name}")
    try:
        data, revision = usb.get(".sync")
    except RuntimeError as err:
        if "not-found" not in str(err):
            raise
    else:
        copy = folder / ".sync"
        copy.write_bytes(data)
        if checksum(copy.read_bytes()) != revision:
            raise RuntimeError("Local .sync backup verification failed")
    return folder


def synchronize(usb: USB, db, project_id: int, manifest: dict, manifest_hash: str,
                dry_run: bool = False):
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
            elif not dry_run:
                if not backed_up:
                    print("Plot & Jot backup:", backup_db(db))
                    backed_up = True
                db.update_scene_content(item["scene_id"], device_bytes.decode("utf-8"))
                item["revision"] = device_rev
                changed = True
        elif action == "pull" and not dry_run:
            usb.put(filename, desktop_bytes, device_rev)
            item["revision"] = desktop_rev
            changed = True
        elif action == "same" and item["revision"] != device_rev and not dry_run:
            item["revision"] = device_rev
            changed = True
        if action == "conflict":
            if dry_run:
                print(f"WOULD CONFLICT {filename}: neither copy would be overwritten")
            else:
                path = save_conflict(db, filename, device_bytes)
                print(f"CONFLICT {filename}: E213 copy preserved at {path}; Plot & Jot unchanged")
        else:
            print(f"{'WOULD ' if dry_run else ''}{action.upper():8} {filename}")
        summary[action] += 1
    if changed and not dry_run:
        put_manifest(usb, manifest, manifest_hash)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial port, e.g. COM10")
    parser.add_argument("--plot-source", type=Path,
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
    parser.add_argument("--dry-run", action="store_true",
                        help="Preview --sync without changing device, desktop or manifest")
    parser.add_argument("--backup-device", type=Path,
                        help="Save read-only SHA-256 verified snapshot of SD drafts and manifest")
    args = parser.parse_args(argv)

    if args.dry_run and (not args.sync or args.bind or args.backup_device):
        parser.error("--dry-run is supported only with --sync")
    if args.backup_device:
        if not args.port:
            parser.error("--port required to back up the device")
        usb = USB(args.port)
        try:
            print("Read-only SD backup:", backup_device(usb, args.backup_device))
        finally:
            usb.close()
        return
    if not args.plot_source:
        parser.error("--plot-source is required for Plot & Jot operations")
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
            # Baseline is the non-authoritative side, so sync updates that side.
            initial = device_hash if args.prefer == "plot" else desktop_hash
            manifest["scenes"][name] = {
                "scene_id": scene["id"], "chapter_id": scene["chapter_id"],
                "revision": initial
            }
            put_manifest(usb, manifest, old_hash)
            manifest, old_hash = get_manifest(usb, args.project, db.get_project(args.project)["name"])
            print(f"LINKED {name} -> scene {scene['id']}")
        if args.sync or args.bind:
            print(synchronize(usb, db, args.project, manifest, old_hash, dry_run=args.dry_run))
        else:
            parser.error("Choose --sync, --bind, --list-files or a listing option")
    finally:
        usb.close()


if __name__ == "__main__":
    main()
