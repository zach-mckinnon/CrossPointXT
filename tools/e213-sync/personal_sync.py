#!/usr/bin/env python3
"""Personal, local Linux GUI for syncing E213 SD drafts to Plot n Jot scenes.

No changes to Plot n Jot, no server and no cloud. The existing safety-checked
USB protocol/three-way sync engine is the only writer of scene prose.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import queue
import re
import unicodedata
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

import plot_jot_sync as bridge
from plot_db_local import LocalPlotDatabase

CONFIG_PATH = Path.home() / ".config" / "e213-sync" / "personal.json"
BACKUP_PATH = Path.home() / ".local" / "share" / "e213-sync" / "backups"


def default_db_path() -> Path:
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "PlotAndJot" / "writer_data.db"


def default_plot_source() -> Path:
    siblings = [Path(__file__).resolve().parents[2].parent / "plot-and-jot-source",
                Path.home() / "repos" / "plot-and-jot-source",
                Path.home() / "Documents" / "Repos" / "plot-and-jot-source"]
    return next((p for p in siblings if (p / "src/writer/database/database.py").is_file()), siblings[0])


def db_manager(source: str, database: str):
    # The personal GUI needs only the installed Plot n Jot SQLite database.
    # The source argument is retained for internal bridge compatibility.
    return LocalPlotDatabase(database)


def list_projects(source: str, database: str):
    db = db_manager(source, database)
    return [(int(p["id"]), str(p["name"])) for p in db.get_projects()]


def list_scenes(source: str, database: str, project_id: int):
    db = db_manager(source, database)
    if not db.get_project(project_id):
        raise ValueError(f"No Plot n Jot project #{project_id}")
    return [(int(scene["id"]), f"{chapter['title']} / {scene['title']}")
            for chapter in db.get_full_manuscript_data(project_id)
            for scene in chapter["scenes"]]


def with_usb(port: str, operation):
    device = bridge.USB(port)
    try:
        return operation(device)
    finally:
        device.close()


def snapshot(port: str, source: str, database: str, project_id: int):
    db = db_manager(source, database)
    project = db.get_project(project_id)
    if not project:
        raise ValueError("Select a valid Plot n Jot project.")
    def inspect(usb):
        docs = sorted(usb.files())
        manifest, _ = bridge.get_manifest(usb, project_id, project["name"])
        bindings = manifest["scenes"]
        return [(name, str(bindings[name]["scene_id"]) if name in bindings else "")
                for name in docs]
    return with_usb(port, inspect)


def suggest_filename(scene_label: str) -> str:
    """FAT32 and USB-safe default for a Plot n Jot chapter/scene title."""
    title = scene_label.rsplit(" / ", 1)[-1]
    plain = unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode()
    simple = re.sub(r"[^a-zA-Z0-9 _-]", "-", plain)
    simple = re.sub(r" +", " ", simple).strip(" -_")[:44].rstrip(" -_")
    return (simple or "New Scene") + ".txt"


def chapter_outline(db, project_id: int, scene_bindings: dict) -> list[dict]:
    """Mirror actual Plot n Jot chapter/scene positions, never alphabetical TXT.

    The database adapter returns chapters/scenes in their position order.
    Include only scenes already linked to files; leave local drafts untouched.
    Existing bindings (scene ID, chapter ID, revision) stay unchanged.
    """
    filenames = {int(item["scene_id"]): filename
                 for filename, item in scene_bindings.items()}
    result = []
    for chapter in db.get_full_manuscript_data(project_id):
        files = [filenames[int(scene["id"])]
                 for scene in chapter["scenes"] if int(scene["id"]) in filenames]
        if files:
            result.append({
                "title": str(chapter["title"])[:80],
                "files": files
            })
    return result


def set_chapter_outline(db, project_id: int, manifest: dict) -> bool:
    """Update ordering metadata only; never change prose or revisions."""
    outline = chapter_outline(db, project_id, manifest["scenes"])
    if manifest.get("chapters") == outline:
        return False
    manifest["chapters"] = outline
    return True


def refresh_chapter_order(port: str, source: str, database: str,
                          project_id: int) -> str:
    db = db_manager(source, database)
    project = db.get_project(project_id)
    if not project:
        raise ValueError("Plot n Jot project no longer exists.")
    def update(usb):
        manifest, previous = bridge.get_manifest(usb, project_id, project["name"])
        if not manifest["scenes"]:
            return "No linked scenes yet. Import or link a scene first."
        updated = set_chapter_outline(db, project_id, manifest)
        if not updated:
            return "Chapter/scene order is already up to date."
        BACKUP_PATH.mkdir(parents=True, exist_ok=True)
        backup = bridge.backup_device(usb, BACKUP_PATH)
        bridge.put_manifest(usb, manifest, previous)
        confirmed, _ = bridge.get_manifest(usb, project_id, project["name"])
        if confirmed.get("chapters") != manifest["chapters"]:
            raise RuntimeError(
                "Chapter order write could not be verified. The SD backup is "
                f"preserved at {backup}."
            )
        return (f"Updated {len(manifest['chapters'])} chapter group(s) on E213.\\n"
                "TXT contents and Plot n Jot database were not changed.\\n"
                f"Verified SD backup: {backup}")
    return with_usb(port, update)


def import_scene(port: str, source: str, database: str, project_id: int,
                 scene_id: int, filename: str) -> str:
    """Create a new named SD document from the existing Plot n Jot scene.

    Never create, rename or overwrite a document with an existing filename.
    The SD sync manifest is written only after a verified file readback.
    If manifest commit fails, the new E213 document remains as a safe unlinked
    copy; the Plot n Jot database is not touched.
    """
    db = db_manager(source, database)
    project = db.get_project(project_id)
    if not project:
        raise ValueError("Plot n Jot project is not available.")
    scene = bridge.db_scene(db, project_id, scene_id)
    if not bridge.NAME.fullmatch(filename):
        raise ValueError("Use a name of up to 44 letters/numbers/spaces/-/_ with .txt.")
    content = (scene["content"] or "").encode("utf-8")
    if len(content) > bridge.MAX_FILE:
        raise ValueError(
            f"Scene is {len(content)} bytes, above the E213's "
            f"{bridge.MAX_FILE}-byte per-document limit. Split the scene first."
        )

    def perform(usb):
        docs = usb.files()
        if any(name.casefold() == filename.casefold() for name in docs):
            raise ValueError("A document with that name already exists on the E213. Choose another.")
        if len(docs) >= 200:
            raise ValueError("E213 has reached its 200-document limit.")
        manifest, previous = bridge.get_manifest(usb, project_id, project["name"])
        if filename in manifest["scenes"]:
            raise ValueError("That filename is already bound to a scene.")
        if any(item["scene_id"] == scene_id for item in manifest["scenes"].values()):
            raise ValueError("Scene is already linked to an E213 document.")

        BACKUP_PATH.mkdir(parents=True, exist_ok=True)
        backup = bridge.backup_device(usb, BACKUP_PATH)
        usb.put(filename, content, "*")  # * only creates a NEW file.
        actual, digest = usb.get(filename)
        expected = bridge.checksum(content)
        if digest != expected or actual != content:
            raise RuntimeError(
                f"Import readback failed for {filename}. The original Plot n Jot scene "
                "is untouched; inspect the SD backup before retrying."
            )
        manifest["scenes"][filename] = {
            "scene_id": int(scene_id),
            "chapter_id": int(scene["chapter_id"]),
            "revision": expected
        }
        set_chapter_outline(db, project_id, manifest)
        try:
            bridge.put_manifest(usb, manifest, previous)
        except Exception as exc:
            raise RuntimeError(
                f"{filename} was copied to SD, but its .sync link could not be saved. "
                "The text remains on the SD as an unlinked document. Rescan and link "
                "that document rather than importing it again. "
                f"Actual device error: {type(exc).__name__}: {exc}"
            ) from exc
        return (
            f"IMPORTED {filename} from Plot n Jot scene #{scene_id}.\n"
            f"Verified {len(content)} bytes on the E213 SD card.\n"
            f"Scene mapped for future bidirectional sync.\n"
            f"SD backup: {backup}\n"
            "Open this file on E213 using Ctrl+O (or the wired B1 picker)."
        )
    return with_usb(port, perform)


def import_scenes(port: str, source: str, database: str, project_id: int,
                  scene_ids: list[int]) -> str:
    """Import selected scenes in one USB session, preserving every existing file.

    An orphan left by a failed manifest write can be linked without uploading
    again, but ONLY if its entire contents match the desktop scene. Commit and
    verify each scene mapping before continuing so partial batches can resume.
    """
    ids = list(dict.fromkeys(scene_ids))
    if not ids:
        raise ValueError("Select at least one Plot n Jot scene.")
    db = db_manager(source, database)
    project = db.get_project(project_id)
    if not project:
        raise ValueError("Plot n Jot project is not available.")

    titles = {int(item["id"]): f"{chapter['title']} / {item['title']}"
              for chapter in db.get_full_manuscript_data(project_id)
              for item in chapter["scenes"]}
    scenes = []
    for scene_id in ids:
        scene = bridge.db_scene(db, project_id, scene_id)
        content = (scene["content"] or "").encode("utf-8")
        if len(content) > bridge.MAX_FILE:
            raise ValueError(
                f"{titles.get(scene_id, str(scene_id))} is {len(content)} bytes; "
                f"E213 supports up to {bridge.MAX_FILE} bytes per document. "
                "No documents were transferred."
            )
        scenes.append((scene_id, scene, content, suggest_filename(
            titles.get(scene_id, f"Scene {scene_id}"))))

    def run(usb):
        available = usb.files()
        manifest, previous = bridge.get_manifest(usb, project_id, project["name"])
        if len(available) >= 200:
            raise ValueError("E213 already has 200 documents.")
        BACKUP_PATH.mkdir(parents=True, exist_ok=True)
        backup = bridge.backup_device(usb, BACKUP_PATH)
        report = [f"Verified E213 backup: {backup}"]
        bound_ids = {int(row["scene_id"]) for row in manifest["scenes"].values()}
        successful = 0
        skipped = 0
        reserved = set()

        for scene_id, scene, content, suggested in scenes:
            if scene_id in bound_ids:
                report.append(f"ALREADY LINKED: scene #{scene_id}; skipped")
                skipped += 1
                continue

            # For the normal first import, use the scene title. If several
            # different scenes share a title, suffix the scene ID safely.
            filename = suggested
            # Many manuscripts have "Scene 1" in several chapters. Keep each
            # scene distinct even when imported in a *later* batch, not just
            # when titles collide during this one batch. Filenames are stable
            # once linked; we never rename a user's existing document.
            taken_linked = any(
                saved.casefold() == filename.casefold()
                and saved in manifest["scenes"]
                for saved in available
            )
            if filename.casefold() in reserved or taken_linked:
                stem = suggested[:-4]
                suffix = f" - {scene_id}"
                filename = f"{stem[:44-len(suffix)].rstrip()}{suffix}.txt"
            if not bridge.NAME.fullmatch(filename):
                raise ValueError(f"Cannot make a safe filename for scene #{scene_id}")

            previous_name = next((n for n in available
                                  if n.casefold() == filename.casefold()), None)
            existed = previous_name is not None
            if existed:
                if previous_name in manifest["scenes"]:
                    report.append(f"NAME TAKEN: {filename} linked to another scene; skipped #{scene_id}")
                    skipped += 1
                    continue
                old_bytes, old_hash = usb.get(previous_name)
                if old_bytes != content or old_hash != bridge.checksum(content):
                    report.append(
                        f"DIFFERENT CONTENT: {filename} already on SD; "
                        f"scene #{scene_id} NOT overwritten or linked. "
                        "Use single-scene import with a different title."
                    )
                    skipped += 1
                    continue
                filename = previous_name
                report.append(f"RECOVERED: existing identical {filename}; no copy needed")
            else:
                if len(available) >= 200:
                    report.append("SD FULL: 200-document limit reached; remaining scenes skipped")
                    break

            proposed = dict(manifest["scenes"])
            proposed[filename] = {
                "scene_id": int(scene_id),
                "chapter_id": int(scene["chapter_id"]),
                "revision": bridge.checksum(content)
            }
            staged_manifest = dict(manifest, scenes=proposed)
            set_chapter_outline(db, project_id, staged_manifest)
            bridge.validate_manifest(staged_manifest, project_id)
            if len(json.dumps(staged_manifest, ensure_ascii=False, indent=2).encode("utf-8")) > bridge.MAX_MANIFEST:
                raise ValueError("The E213 sync mapping file is full (16 KiB). No further scenes imported.")

            if not existed:
                usb.put(filename, content, "*")
                transferred, verified_hash = usb.get(filename)
                if transferred != content or verified_hash != bridge.checksum(content):
                    raise RuntimeError(
                        f"{filename} was copied but failed byte-for-byte readback. "
                        "Existing Plot n Jot content was not changed."
                    )
                available[filename] = verified_hash

            try:
                bridge.put_manifest(usb, staged_manifest, previous)
            except Exception as exc:
                done = "\n".join(report)
                raise RuntimeError(
                    f"Scene #{scene_id} ({filename}) is on SD, but its .sync "
                    "mapping could not be saved. The document is preserved; "
                    "run Scan E213 and Link selected (or retry the batch). "
                    f"Underlying device error: {type(exc).__name__}: {exc}\n"
                    f"Earlier results:\n{done}"
                ) from exc

            # Read back the on-device mapping before the next scene. This also
            # detects a failed manifest commit even if the host saw SAVED.
            confirmed, previous = bridge.get_manifest(usb, project_id, project["name"])
            if confirmed["scenes"].get(filename) != proposed[filename]:
                raise RuntimeError(
                    f"The .sync link for {filename} could not be verified after saving. "
                    "Stop here and rescan before retrying."
                )
            manifest = confirmed
            bound_ids.add(scene_id)
            reserved.add(filename.casefold())
            successful += 1
            report.append(f"IMPORTED + LINKED: scene #{scene_id} -> {filename}")

        report.append(f"Completed: {successful} linked, {skipped} skipped.")
        report.append("Original Plot n Jot scenes were not altered.")
        return "\n".join(report)
    return with_usb(port, run)


def link_scene(port: str, source: str, database: str, project_id: int,
               filename: str, scene_id: int, prefer: str):
    """Link only: never overwrite the manuscript implicitly when linking."""
    db = db_manager(source, database)
    if prefer not in ("device", "plot"):
        raise ValueError("Choose which copy should win the initial sync.")
    scene = bridge.db_scene(db, project_id, scene_id)
    def link(usb):
        docs = usb.files()
        if filename not in docs:
            raise ValueError("Selected E213 file is no longer present. Rescan.")
        manifest, previous = bridge.get_manifest(usb, project_id, db.get_project(project_id)["name"])
        if filename in manifest["scenes"]:
            raise ValueError("This document is already linked.")
        if any(value["scene_id"] == scene_id for value in manifest["scenes"].values()):
            raise ValueError("This Plot n Jot scene is already linked to another document.")
        device_bytes, device_rev = usb.get(filename)
        desktop_rev = bridge.checksum((scene["content"] or "").encode("utf-8"))
        if bridge.checksum(device_bytes) != device_rev:
            raise ValueError("Device file changed during linking; rescan.")
        BACKUP_PATH.mkdir(parents=True, exist_ok=True)
        backup = bridge.backup_device(usb, BACKUP_PATH)
        baseline = device_rev if prefer == "plot" else desktop_rev
        manifest["scenes"][filename] = {
            "scene_id": scene_id, "chapter_id": int(scene["chapter_id"]),
            "revision": baseline
        }
        set_chapter_outline(db, project_id, manifest)
        bridge.put_manifest(usb, manifest, previous)
        return f"Linked {filename} to scene {scene_id}.\nSD backup: {backup}\nClick Preview, then Sync to transfer content."
    return with_usb(port, link)


def unlink_scene(port: str, source: str, database: str, project_id: int, filename: str):
    """Remove the filename-to-scene link; keep both copies of the prose."""
    db = db_manager(source, database)
    project = db.get_project(project_id)
    if not project:
        raise ValueError("Project not found.")
    def unlink(usb):
        manifest, previous = bridge.get_manifest(usb, project_id, project["name"])
        if filename not in manifest["scenes"]:
            raise ValueError("Selected document is not linked.")
        BACKUP_PATH.mkdir(parents=True, exist_ok=True)
        backup = bridge.backup_device(usb, BACKUP_PATH)
        del manifest["scenes"][filename]
        set_chapter_outline(db, project_id, manifest)
        bridge.put_manifest(usb, manifest, previous)
        return (f"Unlinked {filename}. Both the E213 document and Plot n Jot scene remain unchanged.\n"
                f"SD backup: {backup}\nYou may now rename this document on the E213.")
    return with_usb(port, unlink)


def sync_action(port: str, source: str, database: str, project_id: int, apply: bool):
    db = db_manager(source, database)
    project = db.get_project(project_id)
    if not project:
        raise ValueError("Project no longer exists.")
    def run(usb):
        manifest, previous = bridge.get_manifest(usb, project_id, project["name"])
        if not manifest["scenes"]:
            return "No scene links yet. Select a document and scene, then click Link."
        output = io.StringIO()
        if apply:
            BACKUP_PATH.mkdir(parents=True, exist_ok=True)
            backup = bridge.backup_device(usb, BACKUP_PATH)
            output.write(f"Verified E213 SD backup: {backup}\n")
        with contextlib.redirect_stdout(output):
            summary = bridge.synchronize(usb, db, project_id, manifest, previous, dry_run=not apply)
        if apply:
            # Manuscript sync may have replaced .sync. Refresh its SHA before
            # writing the display-only chapter/scene outline separately.
            current, revision = bridge.get_manifest(usb, project_id, project["name"])
            if set_chapter_outline(db, project_id, current):
                bridge.put_manifest(usb, current, revision)
                output.write("Refreshed E213 chapter and scene order.\n")
        output.write(f"\nSummary: {summary}\n")
        if summary["conflict"]:
            output.write("CONFLICT: neither competing copy was overwritten. Resolve manually.\n")
        return output.getvalue()
    return with_usb(port, run)


def serial_ports():
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("PySerial is missing. Run the Linux launcher installer again.") from error
    ports = list(list_ports.comports())
    ports.sort(key=lambda item: (item.vid != 0x303A, item.device))
    return [p.device for p in ports]


def read_settings() -> dict:
    try:
        value = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (FileNotFoundError, ValueError, OSError):
        return {}


class PersonalSyncApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("E213 ↔ Plot n Jot — Personal Sync")
        root.geometry("940x700")
        root.minsize(730, 540)
        self.events: queue.Queue = queue.Queue()
        self.working = False
        self.projects: list[tuple[int, str]] = []
        self.scenes: list[tuple[int, str]] = []
        self.preview_key: tuple | None = None
        saved = read_settings()
        self.source = tk.StringVar(value="")
        self.database = tk.StringVar(value=saved.get("database", str(default_db_path())))
        self.port = tk.StringVar(value=saved.get("port", ""))
        self.project_choice = tk.StringVar()
        self.scene_choice = tk.StringVar()
        self.prefer = tk.StringVar(value="E213 text wins")
        self.status = tk.StringVar(value="Connect E213 by USB, then load your projects.")
        self._build()
        self.root.after(100, self._poll)
        self.root.after(150, self.refresh_ports)

    def _build(self):
        outer = ttk.Frame(self.root, padding=16)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(1, weight=1)
        ttk.Label(outer, text="Personal E213 Sync", font=("", 17, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 12))
        ttk.Label(outer, text="Works outside Plot n Jot. No cloud or app modifications.").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(0, 12))

        ttk.Label(outer, text="Plot n Jot database").grid(row=2, column=0, sticky="w", pady=4)
        ttk.Entry(outer, textvariable=self.database).grid(row=2, column=1, sticky="ew", padx=8)
        ttk.Button(outer, text="Browse…", command=self.browse_database).grid(row=2, column=2)

        controls = ttk.Frame(outer)
        controls.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(12, 8))
        ttk.Label(controls, text="E213 USB port").pack(side="left")
        self.ports = ttk.Combobox(controls, textvariable=self.port, width=21)
        self.ports.pack(side="left", padx=8)
        ttk.Button(controls, text="Detect", command=self.refresh_ports).pack(side="left")
        ttk.Button(controls, text="Load projects", command=self.load_projects).pack(side="left", padx=(24, 8))
        ttk.Button(controls, text="Scan E213", command=self.scan).pack(side="left")

        selectors = ttk.Frame(outer)
        selectors.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(6, 12))
        ttk.Label(selectors, text="Project").pack(side="left")
        self.project_box = ttk.Combobox(selectors, textvariable=self.project_choice,
                                        state="readonly", width=50)
        self.project_box.pack(side="left", padx=8, fill="x", expand=True)
        self.project_box.bind("<<ComboboxSelected>>", self.on_project_change)

        ttk.Label(outer, text="Documents on E213 (select a row to link)").grid(
            row=6, column=0, columnspan=3, sticky="w")
        tree_area = ttk.Frame(outer)
        tree_area.grid(row=7, column=0, columnspan=3, sticky="nsew", pady=(4, 12))
        outer.rowconfigure(7, weight=1)
        self.tree = ttk.Treeview(tree_area, columns=("filename", "scene"), show="headings", height=9)
        self.tree.heading("filename", text="SD .txt document")
        self.tree.heading("scene", text="Linked Plot n Jot scene ID")
        self.tree.column("filename", width=360)
        self.tree.column("scene", width=220)
        self.tree.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(tree_area, orient="vertical", command=self.tree.yview)
        scrollbar.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=scrollbar.set)

        mapping = ttk.Frame(outer)
        mapping.grid(row=8, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        ttk.Label(mapping, text="Map selected document to scene").grid(row=0, column=0, sticky="w")
        self.scene_box = ttk.Combobox(mapping, textvariable=self.scene_choice, width=54, state="readonly")
        self.scene_box.grid(row=1, column=0, sticky="ew", pady=(3, 0))
        mapping.columnconfigure(0, weight=1)
        ttk.Label(mapping, text="Initial priority").grid(row=0, column=1, padx=(12, 0), sticky="w")
        ttk.Combobox(mapping, textvariable=self.prefer, width=22, state="readonly",
                     values=["E213 text wins", "Plot n Jot text wins"]).grid(
                         row=1, column=1, padx=(12, 6), pady=(3, 0))
        ttk.Button(mapping, text="Link selected", command=self.link).grid(
            row=1, column=2, padx=(8, 0))
        ttk.Button(mapping, text="Unlink", command=self.unlink).grid(
            row=1, column=3, padx=(8, 0))
        ttk.Label(mapping, text="No documents on the E213 yet?").grid(
            row=2, column=0, sticky="w", pady=(10, 0))
        ttk.Button(mapping, text="Import one scene",
                   command=self.import_selected_scene).grid(
                       row=2, column=1, sticky="w", padx=(12, 0), pady=(10, 0))
        ttk.Button(mapping, text="Import multiple scenes…",
                   command=self.open_multi_import).grid(
                       row=2, column=2, columnspan=2, sticky="w", padx=(12, 0), pady=(10, 0))

        actions = ttk.Frame(outer)
        actions.grid(row=9, column=0, columnspan=3, sticky="w", pady=(4, 10))
        ttk.Button(actions, text="1. Preview changes", command=self.preview).pack(side="left")
        self.apply_button = ttk.Button(actions, text="2. Sync now", command=self.sync, state="disabled")
        self.apply_button.pack(side="left", padx=12)
        ttk.Button(actions, text="Back up E213", command=self.backup).pack(side="left")
        ttk.Button(actions, text="Refresh chapter order", command=self.refresh_order).pack(
            side="left", padx=(12, 0))
        ttk.Label(outer, textvariable=self.status).grid(row=10, column=0, columnspan=3, sticky="w")
        self.log = tk.Text(outer, wrap="word", height=9, state="disabled")
        self.log.grid(row=11, column=0, columnspan=3, sticky="nsew", pady=(5, 0))
        outer.rowconfigure(11, weight=1)

    def _write(self, message):
        self.log.configure(state="normal")
        self.log.insert("end", message.rstrip() + "\n\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _save_config(self):
        try:
            CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            CONFIG_PATH.write_text(json.dumps({
                "database": self.database.get(), "port": self.port.get()
            }, indent=2), encoding="utf-8")
        except OSError:
            pass  # Sync can still work without retaining local settings.

    def _run(self, label, fn, done):
        if self.working:
            return
        self.working = True
        self.status.set(label)
        self.apply_button.configure(state="disabled")
        def background():
            try:
                self.events.put((done, fn(), None))
            except Exception as exc:
                self.events.put((done, None, exc))
        threading.Thread(target=background, daemon=True).start()

    def _poll(self):
        try:
            while True:
                done, result, error = self.events.get_nowait()
                self.working = False
                if error is not None:
                    self.status.set("Action failed; see below.")
                    self._write(f"ERROR: {error}")
                    messagebox.showerror("E213 Sync", str(error))
                else:
                    done(result)
        except queue.Empty:
            pass
        self.root.after(100, self._poll)

    def _args(self):
        self._save_config()
        if not self.port.get().strip():
            raise ValueError("Select the E213 USB serial port first.")
        idx = self.project_box.current()
        if idx < 0 or idx >= len(self.projects):
            raise ValueError("Load and select a Plot n Jot project first.")
        return self.port.get().strip(), self.source.get(), self.database.get(), self.projects[idx][0]

    def _args_or_message(self):
        try:
            return self._args()
        except ValueError as error:
            messagebox.showerror("E213 Sync", str(error))
            return None

    def refresh_ports(self):
        try:
            found = serial_ports()
            self.ports["values"] = found
            if found and self.port.get() not in found:
                self.port.set(found[0])
            self.status.set("USB ports detected. Load projects and scan E213.")
        except Exception as error:
            self._write(f"Port detection: {error}")

    def browse_source(self):
        value = filedialog.askdirectory(initialdir=self.source.get())
        if value:
            self.source.set(value)
            self.preview_key = None

    def browse_database(self):
        value = filedialog.askopenfilename(title="Select Plot n Jot writer_data.db",
                                           filetypes=[("SQLite database", "*.db"), ("All files", "*")])
        if value:
            self.database.set(value)
            self.preview_key = None

    def load_projects(self):
        source, database = self.source.get(), self.database.get()
        def done(projects):
            self.projects = projects
            self.project_box["values"] = [f"{name}  (#{id_})" for id_, name in projects]
            if projects:
                self.project_box.current(0)
                self.on_project_change()
            self.status.set(f"Loaded {len(projects)} Plot n Jot projects.")
        self._run("Reading Plot n Jot projects…", lambda: list_projects(source, database), done)

    def on_project_change(self, _event=None):
        self.preview_key = None
        idx = self.project_box.current()
        if idx < 0 or idx >= len(self.projects):
            return
        source, database, project_id = self.source.get(), self.database.get(), self.projects[idx][0]
        def done(scenes):
            self.scenes = scenes
            self.scene_box["values"] = [f"{label}  (#{scene_id})" for scene_id, label in scenes]
            if scenes:
                self.scene_box.current(0)
            self.tree.delete(*self.tree.get_children())
            self.status.set("Choose Scan E213 to list documents for this project.")
        self._run("Loading chapter scenes…", lambda: list_scenes(source, database, project_id), done)

    def scan(self):
        args = self._args_or_message()
        if not args: return
        def done(files):
            self.tree.delete(*self.tree.get_children())
            for filename, scene_id in files:
                self.tree.insert("", "end", iid=filename, values=(filename, scene_id or "Not linked"))
            self.preview_key = None
            self.status.set(f"Found {len(files)} E213 document(s).")
        self._run("Reading SD drafts and links…", lambda: snapshot(*args), done)

    def open_multi_import(self):
        args = self._args_or_message()
        if not args:
            return
        if not self.scenes:
            messagebox.showinfo(
                "Import scenes", "This project has no scenes to import yet."
            )
            return

        dialog = tk.Toplevel(self.root)
        dialog.title("Import Plot n Jot scenes to E213")
        dialog.geometry("620x490")
        dialog.minsize(460, 350)
        dialog.transient(self.root)
        dialog.grab_set()
        holder = ttk.Frame(dialog, padding=14)
        holder.pack(fill="both", expand=True)
        ttk.Label(holder, text="Choose scenes to copy onto the E213",
                  font=("", 13, "bold")).pack(anchor="w")
        ttk.Label(holder, text="Ctrl/Shift-click to select multiple; or use Select all. "
                  "Already-linked scenes are skipped.").pack(anchor="w", pady=(5, 10))
        area = ttk.Frame(holder)
        area.pack(fill="both", expand=True)
        chosen = tk.Listbox(area, selectmode="extended", exportselection=False)
        bar = ttk.Scrollbar(area, orient="vertical", command=chosen.yview)
        chosen.configure(yscrollcommand=bar.set)
        for scene_id, label in self.scenes:
            chosen.insert("end", f"{label}  (#{scene_id})")
        chosen.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        selected = self.scene_box.current()
        if selected >= 0:
            chosen.selection_set(selected)
            chosen.see(selected)
        buttons = ttk.Frame(holder)
        buttons.pack(fill="x", pady=(12, 0))
        ttk.Button(buttons, text="Select all",
                   command=lambda: chosen.selection_set(0, "end")).pack(side="left")
        ttk.Button(buttons, text="Clear",
                   command=lambda: chosen.selection_clear(0, "end")).pack(side="left", padx=8)

        def start():
            ids = [self.scenes[int(i)][0] for i in chosen.curselection()]
            if not ids:
                messagebox.showinfo("Import scenes", "Select at least one scene.", parent=dialog)
                return
            if not messagebox.askyesno(
                "Import selected scenes",
                f"Copy {len(ids)} Plot n Jot scenes onto the E213 SD card?\n\n"
                "Each document gets a separate .txt file and an exact scene link.\n"
                "Existing drafts are never overwritten. Identical orphaned "
                "copies can be relinked automatically.\n\n"
                "A backup is made first. Keep the writer in USB SYNC mode "
                "and close Plot n Jot during the transfer.",
                parent=dialog
            ):
                return
            dialog.grab_release()
            dialog.destroy()

            def done(report):
                self._write(report)
                self.status.set("Import finished. Review details and rescan E213.")
                self.preview_key = None
                self.scan()

            self._run(
                f"Importing {len(ids)} scenes over USB…",
                lambda: import_scenes(*args, ids), done
            )

        ttk.Button(buttons, text="Cancel", command=dialog.destroy).pack(side="right")
        ttk.Button(buttons, text="Import selected",
                   command=start).pack(side="right", padx=8)

    def import_selected_scene(self):
        args = self._args_or_message()
        if not args:
            return
        idx = self.scene_box.current()
        if idx < 0 or idx >= len(self.scenes):
            messagebox.showinfo("Import to E213", "Select a Plot n Jot scene to import first.")
            return
        scene_id, label = self.scenes[idx]
        filename = simpledialog.askstring(
            "Import Plot n Jot scene",
            "New filename on the E213 microSD (a new .txt, never overwrites):",
            initialvalue=suggest_filename(label), parent=self.root
        )
        if filename is None:
            return
        filename = filename.strip()
        if not bridge.NAME.fullmatch(filename):
            messagebox.showerror(
                "Invalid filename",
                "Use up to 44 letters, numbers, spaces, hyphens, or underscores, ending in .txt."
            )
            return
        if not messagebox.askyesno(
            "Copy scene to E213",
            f"Copy '{label}' to a NEW SD document named {filename}?\n\n"
            "Your Plot n Jot scene will not be changed.\n"
            "The new document will be linked for future two-way sync.\n"
            "The E213 will be backed up first.\n\n"
            "If a scene has special Unicode characters, the current E213 font "
            "might not display them correctly."
        ):
            return
        def done(report):
            self._write(report)
            self.status.set(f"Imported {filename}. Open it in the E213 file picker.")
            self.preview_key = None
            self.scan()
        self._run("Copying Plot n Jot scene onto E213 SD…",
                  lambda: import_scene(*args, scene_id, filename), done)

    def link(self):
        args = self._args_or_message()
        if not args: return
        picked = self.tree.selection()
        if not picked:
            messagebox.showinfo("Link a document", "First select a document in the E213 list.")
            return
        idx = self.scene_box.current()
        if idx < 0 or idx >= len(self.scenes):
            messagebox.showinfo("Link a document", "Choose the destination Plot n Jot scene.")
            return
        filename, scene_id = picked[0], self.scenes[idx][0]
        prefer = "device" if self.prefer.get() == "E213 text wins" else "plot"
        if not messagebox.askyesno("Link document",
                f"Link {filename} to Plot n Jot scene #{scene_id}?\n\n"
                f"On the first sync, {self.prefer.get()}.\n\n"
                "Linking writes the SD mapping, but does NOT replace scene prose yet."):
            return
        def done(msg):
            self._write(msg)
            self.status.set("Linked. Preview before syncing.")
            self.scan()
        self._run("Backing up SD and linking scene…",
                  lambda: link_scene(*args, filename, scene_id, prefer), done)

    def unlink(self):
        args = self._args_or_message()
        if not args: return
        picked = self.tree.selection()
        if not picked:
            messagebox.showinfo("Unlink a document", "Select a linked document from the E213 list.")
            return
        filename = picked[0]
        if not messagebox.askyesno("Unlink document",
                f"Remove the Plot n Jot scene mapping for {filename}?\n\n"
                "Neither document will be deleted or overwritten.\n"
                "You can then rename the SD document on the E213."):
            return
        def done(output):
            self._write(output)
            self.status.set("Unlinked without deleting either document.")
            self.scan()
        self._run("Backing up and unlinking…",
                  lambda: unlink_scene(*args, filename), done)

    def preview(self):
        args = self._args_or_message()
        if not args: return
        def done(output):
            self._write(output)
            self.preview_key = args
            self.apply_button.configure(state="normal")
            self.status.set("Review PUSH / PULL / CONFLICT before syncing.")
        self._run("Previewing changes (read-only)…", lambda: sync_action(*args, apply=False), done)

    def sync(self):
        args = self._args_or_message()
        if not args or self.preview_key != args:
            messagebox.showwarning("Preview required", "Preview this project and E213 before syncing.")
            return
        if not messagebox.askyesno("Sync E213 ↔ Plot n Jot",
                "Is Plot n Jot closed and the E213 finished typing?\n\n"
                "The tool backs up E213 files and preserves conflicting edits."):
            return
        self.preview_key = None
        def done(output):
            self._write(output)
            self.status.set("Sync complete. Review summary and reopen Plot n Jot.")
            self.scan()
        self._run("Backing up E213 and synchronizing…", lambda: sync_action(*args, apply=True), done)

    def refresh_order(self):
        args = self._args_or_message()
        if not args:
            return
        def done(output):
            self._write(output)
            self.status.set("E213 chapter/scene order refreshed.")
            self.preview_key = None
        self._run(
            "Updating E213 chapter/scene order without changing prose…",
            lambda: refresh_chapter_order(*args), done
        )

    def backup(self):
        args = self._args_or_message()
        if not args: return
        port = args[0]
        def task():
            BACKUP_PATH.mkdir(parents=True, exist_ok=True)
            return with_usb(port, lambda usb: bridge.backup_device(usb, BACKUP_PATH))
        def done(location):
            self._write(f"Verified SD backup: {location}")
            self.status.set("E213 backup completed.")
        self._run("Backing up SD drafts…", task, done)


def main():
    root = tk.Tk()
    PersonalSyncApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
