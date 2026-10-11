# E213 writer: SD files, button sleep, Plot & Jot transfer

This applies to `heltec_e213_writer`, the standalone Heltec S3 writer. It does
not add these features to the Xteink C3 reader. No wiring changes are required.

## Controls

| Where | Action | Result |
| --- | --- | --- |
| Every screen | Hold B2 (GPIO47) for at least 1.2 seconds, then release | Save, blank the panel, deep sleep; same routine as Ctrl+Shift+Q |
| Writing | B1 short or Ctrl+O | Save current document and open SD browser |
| Writing | B2 short or Ctrl+S | Save immediately |
| Pairing | B1 short / B2 short | Select or scan / connect keyboard |
| Pairing | Hold B1, release | Open files without a keyboard |
| Files | B1 short / Down | Next selection (wraps) |
| Files | Up | Previous selection |
| Files | B2 short / Enter | Open selection |
| Files | Hold B1, release | Parent folder; at root, resume writing |
| Files | Esc or Ctrl+O | Resume writing |

The file list includes Resume writing, New SD draft, Parent folder, Next/First
files, then eight directory entries per page. Folders can be explored from the
card root. Only `.txt` and `.md` files are editable. There is no rename/delete
command. New files receive unique names such as `/Writer/draft-1234abcd.txt`.

RESET wakes from sleep. GPIO42 and GPIO47 are not configured as wake sources.
The LED switch is independent and can still consume power while the MCU sleeps.
Sleep is cancelled if saving, display blanking, or BLE shutdown fails.

## Saving and recovery

- Ctrl+S, Button 2, autosave after a 1.8-second editing pause, and sleep all use
  the same save path. File switching saves the old document first.
- A versioned internal LittleFS checkpoint stores text, SD path, pending-save
  state, and checksums together. The original `/draft.txt` is migrated without
  deleting it. Internal mount never autoformats the filesystem.
- SD access uses `HalStorage` / `HalFile` and its mutex, on FSPI with CS38,
  MOSI39, SCLK40, MISO41 at 1 MHz. The display retains HSPI.
- Replacements are written to `<file>.e213-tmp`, synced, reopened and compared.
  The old file becomes `<file>.e213-bak` before the new file takes its name. Keep
  these suffixes reserved for writer recovery; they are hidden in the browser.
- An interrupted save retains the recovery checkpoint. A missing target with
  a backup is restored before retrying. The previous SD version remains in the
  backup after a successful replacement. FAT itself cannot guarantee survival
  of arbitrary power loss: always save/sleep before removing the card.
- If the SD file changed outside the writer while device edits were pending,
  saving stops and preserves both copies. Do not delete the internal recovery
  checkpoint to resolve this. Copy the card's external version aside, restore
  the expected previous version from `.e213-bak` if appropriate, then retry.
  Check serial output before attempting recovery; there is no conflict-resolution
  UI on the deck yet.
- A clean checkpoint reloads an updated SD file at boot. An absent SD preserves
  the internal document. An internal-only scratch draft can still save and sleep;
  a pending save to a previously selected SD file keeps the device awake if the
  card is unavailable.
- Files over 65,536 bytes, invalid UTF-8, and binary/control data are rejected
  before replacing the current draft. Paths must fit 159 UTF-8 bytes and browser
  names must fit 94 bytes. Hidden entries are skipped. Insert/remove cards only
  with power off; remount requires reset.
- UTF-8 is preserved and cursor/deletion respects code-point boundaries. The
  current built-in display font shows non-ASCII characters as `?`; it does not
  provide full Unicode glyph rendering or grapheme-cluster editing.

`SD:OK` now means the persistent HAL mount succeeded, not the old throwaway
startup write test. A failed save is reported separately. The editor's saved
footer distinguishes SD from internal-only storage.

## Plot & Jot: first working SD transfer

The companion command is `scripts/plot_jot_sync.py`. It runs on the PC with
Python's standard library. This is SD-card transfer, not Wi-Fi sync or a menu
added to the installed Plot & Jot app. The app's source repository is unchanged.

The adapter was checked against [Plot & Jot database.py at 90a8fd1](https://github.com/zach-mckinnon/plot-and-jot-source/blob/90a8fd135963924474a1c967aa8e6631d027228d/src/writer/database/database.py#L39).
Projects, chapters and scenes have integer IDs; scene content is plain text.
The [app's default Windows path](https://github.com/zach-mckinnon/plot-and-jot-source/blob/90a8fd135963924474a1c967aa8e6631d027228d/src/writer/core/paths.py#L41)
is `%LOCALAPPDATA%\PlotAndJot\writer_data.db`. Verify that this is the database
used by your installation; pass another path explicitly if needed.

1. Save and close Plot & Jot. Power off the deck and put its SD card in the PC.
2. From the CrossPointXT repository in PowerShell, substitute the actual SD
   drive letter below:

   ```powershell
   $db = "$env:LOCALAPPDATA\PlotAndJot\writer_data.db"
   python scripts/plot_jot_sync.py --db $db list
   python scripts/plot_jot_sync.py --db $db export --project 1 --sd E:\
   ```

3. Export creates a new `PlotJot/p1-xxxxxxxx/` folder. Each scene has a
   `scene-<id>.txt` plus `scene-<id>.txt.plot`. Keep both files together. A new
   export never overwrites an old package containing unsynced edits.
4. Safely eject, put the card back in the powered-off deck, start it, open files,
   navigate into that folder and open the scene text. Edit, Ctrl+S, then sleep.
5. Put the card back in the PC. With Plot & Jot closed, preview and apply:

   ```powershell
   python scripts/plot_jot_sync.py --db $db import E:\PlotJot\p1-xxxxxxxx
   python scripts/plot_jot_sync.py --db $db import E:\PlotJot\p1-xxxxxxxx --apply
   ```

6. Reopen Plot & Jot to see the scene. The same package can receive further deck
   edits. Export a fresh package to bring later desktop edits back to the deck.

To bring in a new unlinked deck file, select its destination explicitly:

```powershell
python scripts/plot_jot_sync.py --db $db adopt E:\Writer\draft-1234abcd.txt --project 1 --chapter 1
python scripts/plot_jot_sync.py --db $db adopt E:\Writer\draft-1234abcd.txt --project 1 --chapter 1 --apply
```

`list` shows the IDs. Preview is the default; `--apply` writes. Every CLI export
or applied import/adoption creates a separate SQLite backup beside the database.
Close Plot & Jot to prevent its in-memory editor from later autosaving stale text
over imported changes. The tool validates the known schema and never creates a
database when a supplied path is wrong.

### Metadata and conflicts

`.txt.plot` is a **new version-1 transfer sidecar**, not an existing Plot & Jot
project-file standard. JSON includes a format marker, version, database UUID,
transfer UUID, project/chapter/scene IDs, titles, UTF-8 encoding and the exported
content's SHA-256. Firmware edits only the text; metadata remains untouched.

The PC adds three namespaced tables (`writer_deck_identity`,
`writer_deck_exports`, `writer_deck_receipts`) to track identity, accepted hashes,
and imports. Existing application tables are not migrated. The SQLite database
and these tracking records stay on the PC; never put the live database on SD.

Import checks metadata against that database's export record. An extension or
copied title alone cannot claim a scene. Unknown versions, forged/mismatched IDs,
wrong databases, deleted/moved scenes, malformed text and oversized content are
rejected. No scene is implicitly recreated or moved. Ordinary documents require
the explicit `adopt` destination.

If only the deck changed, its text updates the existing scene while preserving
title, order, summary and other project structure. If both copies changed, the
desktop scene remains intact and a new `[Deck conflict]` scene holds the deck
version in the same chapter. Repeat imports are recorded so they do not create
duplicate conflict scenes or replay already imported changes. The tool does not
recalculate the application's Wiki link index or comment offsets; use the app's
link rescan after importing. Backups preserve the pre-import state.

## Verification

2026-10-09: the S3 build and COM10 upload passed (168,744 bytes static RAM,
1,034,371 bytes flash). Serial confirmed persistent SD mount, recovery of the
existing 362-byte draft, and `ready=1 displayTimeout=0 sdMounted=1` through at
least 80 seconds uptime. Free heap was approximately 101,000-104,000 bytes in that capture.
The user subsequently confirmed that holding Button 2 worked in the requested
keyboard-free sleep test. This confirms the physical sleep gesture; file-browser
operation, SD content verification, RESET wake, and the Plot & Jot hardware round
trip have not yet been separately confirmed for this build. Sleep from every
screen also remains a broader acceptance check.
The original C3 `default` environment also built successfully after the target
source exclusion (109,564 bytes static RAM, 5,465,817 bytes flash).

Automated checks cover real WriterStore code using an in-memory fault-injection
HAL, plus Python transfer tests for round trips, conflicts, duplicate imports,
wrong IDs, UTF-8, limits, backup integrity and adoption. A separate integration
check exercised the unmodified Plot & Jot DatabaseManager from the linked commit.
The standalone S3 firmware uses two static 64 KiB text buffers (active and staged
load/verification) and eight browser names. There are no per-row allocations,
no second framebuffer, and no changes to the C3 editor's buffers.

Run the Python tests from the repo with:

```powershell
python -m unittest discover -s test/heltec_e213 -p 'test_*.py' -v
```

From an MSVC Developer Command Prompt, the host firmware tests run with:

```bat
cl /nologo /std:c++20 /EHsc /D_CRT_SECURE_NO_WARNINGS /Itest\heltec_e213\stubs test\heltec_e213\writer_storage_test.cpp src\targets\heltec_e213_writer\WriterStore.cpp /Fo.pio\ /Fe.pio\writer-storage-tests.exe
.pio\writer-storage-tests.exe
cl /nologo /std:c++17 /c /constexpr:steps2000000 test\heltec_e213\writer_shortcut_test.cpp test\heltec_e213\keyboard_reconnect_test.cpp /Fo.pio\
```

The test stubs are confined to that explicit host-test include path; they are
never part of the firmware build. The C3 base environment excludes `src/targets/`
so standalone S3 entry points cannot leak into its build.

Physical acceptance checklist for this new build:

1. Confirm startup reports SD mounted and no reset loop; reconnect the keyboard.
2. Save a distinctive line, sleep, inspect the actual `.txt` on the PC, then boot
   and reopen it. Verify the exact text, not just `SD:OK`.
3. Copy a second text file and a nested folder to SD while powered off. Browse
   and open them, then switch back; verify separate contents survive restart.
4. Turn off the keyboard. Hold B2 about two seconds and release from pairing,
   writing and the browser. Confirm the panel blanks and RESET restores writing.
5. Export a disposable Plot & Jot project, edit one scene on the deck, import it,
   and repeat the import. Then change both copies and verify a single conflict
   copy is created without replacing the desktop scene.
6. Use a spare card for power-interruption/read-only/full-card tests. Confirm save
   failures keep the device awake and preserve the old SD file plus recovery.

Do not treat a successful build or host tests as completed physical acceptance.
