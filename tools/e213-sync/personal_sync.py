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
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

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

        actions = ttk.Frame(outer)
        actions.grid(row=9, column=0, columnspan=3, sticky="w", pady=(4, 10))
        ttk.Button(actions, text="1. Preview changes", command=self.preview).pack(side="left")
        self.apply_button = ttk.Button(actions, text="2. Sync now", command=self.sync, state="disabled")
        self.apply_button.pack(side="left", padx=12)
        ttk.Button(actions, text="Back up E213", command=self.backup).pack(side="left")
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
