# E213 Micro Writer — customized fork

Use branch dev. Master is untouched; there are no plans to contribute this fork upstream.

## Windows firmware

1. Back up the Heltec flash and manuscripts before flashing; do not erase or format an existing LittleFS partition.
2. In PowerShell in CrossPointXT:
    git switch dev
    git pull
    pio run -e heltec_e213_writer
    pio run -e heltec_e213_writer -t upload --upload-port COM10
3. Substitute the actual COM port if different.

The writer deliberately does NOT autoformat LittleFS on mount failure.
If storage is blank or unreadable, first explicitly provision or recover it, instead of risking deletion.

## Bluetooth keyboard

- Ctrl+O: open/close file picker; Up/Down select; Enter open; Escape cancel
- Ctrl+N: new uniquely numbered draft such as Draft-002.txt
- Ctrl+S: save now; autosave remains enabled after 1.8 seconds
- Drafts live under /drafts/*.txt in internal flash (maximum 40 files of 64 KiB).
- Existing /draft.txt is copied when no new drafts exist; the old file remains available as a recovery copy.
- Every write creates a temporary file and retains the previous version as a .bak backup.
- Failed saves prevent switching drafts.

## USB sync to the Plot & Jot laptop

This first implementation uses USB and internal flash, not microSD or Wi-Fi.
Close Plot & Jot before syncing so unsaved desktop UI edits cannot subsequently overwrite database changes.
Run once:
    python -m pip install pyserial

From CrossPointXT, with Plot & Jot source cloned adjacent:
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --list-projects

Then:
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --list-scenes
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --list-files

To explicitly link an existing E213 document to an existing Plot & Jot scene:
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --bind Draft-001.txt 101 --prefer device

--prefer device uses the E213 draft as the first authoritative version.
--prefer plot replaces the E213 draft with Plot & Jot scene content instead.
When content is already identical, omit --prefer.

To synchronize later:
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --sync

## Data safety

- A valid /.sync manifest (v1) associates explicit project, scene and chapter IDs; filename matches are never guessed.
- Wrong project/provider/version is rejected; unlinked files are never automatically uploaded.
- Both ends are compared against SHA-256 of last synced content.
- Device-only change: update Plot & Jot, after first taking a WAL-safe SQLite backup.
- Desktop-only change: update the E213 using a version-checked write; preserve previous copy as .bak.
- Both changed: preserve E213 copy under the PlotAndJot backups/e213-conflicts folder and do NOT overwrite either side.
- Missing E213 file never deletes the Plot & Jot scene.
- Serial transfers use checksums, acknowledged chunks, and explicit write confirmation.
- USB interface accepts commands from physically attached computers only. Do not use untrusted hosts.

## Limits and validation

The repository now implements USB + internal-flash sync, but physical E213/Plot & Jot
end-to-end round-trip tests remain outstanding. No Wi-Fi, SD card, automatic
scene creation/rename or multi-project UI yet.

Run unit tests:
    python -m unittest discover -s test/e213_sync -p test_*.py

The GitHub Actions workflow builds reader/writer and runs host tests on dev.
