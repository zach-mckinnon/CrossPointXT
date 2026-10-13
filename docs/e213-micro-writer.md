# E213 Micro Writer — personalized Heltec V1.1 fork

Active development: dev. No plans to merge into the original CrossPoint project.

## Merged v1_1 physical hardware baseline

The assembled writer was verified on the historical
port/heltec-e213-v1_1 branch with an **LCMEN2R13EFC1 panel** and
externally soldered B1=GPIO42, B2=GPIO47, in addition to the SPI SD wires
CS38/MOSI39/SCK40/MISO41. These are not the onboard USER/BOOT buttons.
The default BW display driver hung on this unit, so the dev writer now uses
the FC1 display adapter with a ten-second display BUSY timeout and a
MISO=NC workaround for PSRAM GPIO33. SD SPI clock is 1 MHz.

Wired controls in the merged dev writer:
- B1 short (keyboard connected): save and enter document picker.
- B1 short (picker): move to next document; B2 short opens it.
- B1 long (picker): leave the picker.
- B1 short (pairing): select next keyboard, or request scanning.
- B1 long (pairing): open files without a keyboard.
- B2 short (writing): force SD save.
- B2 short (pairing): connect selected keyboard.
- **B2 hold >=1.2 seconds and release**: save active work, blank the display,
  end Bluetooth and enter deep sleep. **RESET wakes.**
- Ctrl+Shift+Q: same save-and-sleep from BLE keyboard.

The dev implementation is a newer SD-only editor. The old v1_1
LittleFS checkpoint storage and nested-folder browser **are not active**
in dev, because their data model conflicts with the current USB/SD
sync manifest and /drafts layout. The historical v1_1 commit is retained
as a merge parent and its separate SD-card Plot n Jot workflow remains
available for future adaptation. Do not mix the two storage layouts or
expect old v1_1 .e213-bak recovery files to be recognized by new dev.

The port/heltec-e213-v1_1 branch remains unchanged; the git merge
does not include uncommitted edits on a local Windows checkout.

## Storage — USB writes to SD, not internal flash

**The external microSD is the only active manuscript storage.** USB serial is a
transfer mechanism to the SD card that remains installed in the E213. Typing,
autosave, Ctrl+O, and USB Plot & Jot sync all read/write the SAME SD files.

    SD Card (FAT32)/
      drafts/
        Draft-001.txt
        Draft-002.txt
        Draft-002.txt.bak
      .sync                    Plot & Jot scene mapping and revision hashes
      .active                  Last selected draft
      .flash-imported          One-time import marker (if applicable)

Names starting with "." are hidden on some desktop operating systems. Backups
remain beside the SD document in .bak files. The 128 KiB per-document limit is
an ESP32 RAM/editor safety limit, not SD card capacity. Up to 200 drafts are
listed in the current file picker. Project subfolders and truly large streamed
documents remain future features.

## Proposed external SPI microSD wiring

The Heltec E213 V1.1 requires an external SPI microSD breakout. These GPIOs
come from our unverified hardware-port plan, NOT a confirmed wiring test.

| microSD breakout | ESP32-S3 GPIO |
| --- | --- |
| CS | 38 |
| MOSI / DI | 39 |
| SCK / CLK | 40 |
| MISO / DO | 41 |
| GND | GND |
| VCC | 3V3 ONLY if the breakout is rated for 3.3 V input |

The firmware uses a separate SPI bus at 4 MHz, not the e-ink display pins.
Check the breakout supply requirements and verify wiring with temporary leads
before soldering. Bare SD signals must never receive 5 V.

Format the card FAT32 on your computer. Firmware does not automatically
format SD or flash. With no readable card, writing and USB sync are disabled.

## Earlier internal-flash drafts

IMPORTANT: Before flashing, BACK UP the old manuscripts and existing firmware.

Once SD is mounted, the firmware attempts a one-time, copy-only migration of
old LittleFS /draft.txt and /drafts/*.txt files. It also copies /.sync and
/.active metadata only when those SD counterparts are absent. It never erases
old flash files, and existing SD drafts cannot be overwritten by migration.
Different versions become Recovered-NNN.txt on SD.

A successful migration creates /.flash-imported; if an import operation fails,
the writer stops editing so the source can be recovered first. If the old
filesystem cannot mount, it is left untouched; verify the new card actually
has your previous content before continuing. Flash partition changes or a
full-chip erase may destroy recoverable old drafts, so back up FIRST.

## Windows PowerShell build and upload

    git fetch origin
    git switch dev
    git pull
    pio run -e heltec_e213_writer
    pio run -e heltec_e213_writer -t upload --upload-port COM10
    pio device monitor -p COM10 -b 115200

Replace COM10 with your actual E213 port. Close the serial monitor before sync.

## Keyboard controls

- Ctrl+O: open/close SD document picker; arrows navigate, Enter opens, Esc exits
- Ctrl+R: rename active or file-picker-selected draft; linked Plot n Jot files must be unlinked first
- Ctrl+N: open a title editor; Enter creates a named `.txt` file (blank title makes next Draft-NNN); Esc cancels
- Ctrl+S: save to SD; autosave still runs after 1.8 seconds idle
- Reboot restores the last document via SD /.active
- A failed save blocks switching documents; never silently fall back to flash

## First-time transfer: Plot n Jot to a blank E213

**Plot n Jot itself does not install on the micro-writer.** The E213 runs its
own lightweight editor, and the Linux sync app copies each selected Plot n Jot
scene as an ordinary UTF-8 `.txt` on the E213 microSD card.

1. Flash the latest `dev` writer firmware. Connect E213 to the Linux computer
   with a USB *data* cable. Hold the external **B1 (GPIO42)** button for
   about 1.2 seconds, then release. The display should say **USB SYNC // E213**
   and **Bluetooth scanning paused**. No Bluetooth keyboard is required.
2. Open **E213 Personal Sync** on Linux, click **Detect**, **Load projects**,
   select a Plot n Jot project, then **Scan E213**. Close other serial monitors.
3. In the **Map selected document to scene** dropdown choose an existing
   chapter scene. Click **Import selected Plot n Jot scene to E213**.
4. Confirm the proposed TXT filename. The tool refuses duplicates, backs up
   existing SD files, writes the scene to a **new** document, verifies its
   SHA-256 and content, and binds it to its Plot n Jot scene in `/.sync`.
   Plot n Jot's scene and database are NOT modified on import.
5. After import, hold B1 to exit USB SYNC. With keyboard connected press
   Ctrl+O, select the imported TXT, and press Enter. Type and save normally.
   Later return to USB SYNC and use Preview changes / Sync now to transfer
   modifications in either direction.

**USB timeout behavior:** native ESP32-S3 USB CDC can reconnect or reset at
serial attach. The Linux bridge now sends several idempotent HELLO requests
for up to 55 seconds rather than dropping the session after one early packet.
A received HELLO also enters USB SYNC mode automatically, pauses BLE discovery
and keeps the USB loop responsive. For the first test, use B1 hold to enter
USB SYNC before clicking Scan or Import. The sync mode does not require pairing.
If there is no reply, verify the selected serial port, a USB data cable, and
the physical SD card; the GUI reports an actionable error instead of silently
continuing.

Existing manuscript names and Plot n Jot links are never overwritten by
Import. A file is limited to 128 KiB. Some Unicode punctuation may not render
in the initial E213 bitmap font although UTF-8 bytes remain intact.

## Personal Linux desktop sync window (Plot n Jot stays unchanged)

The optional **E213 Personal Sync** window is a standalone helper in CrossPointXT.
It connects directly to the E213's USB port and your existing Plot n Jot
SQLite database. It does not need a Plot n Jot source checkout, server,
cloud account, extension, or a change to the Plot n Jot application.

Install a Linux application-menu shortcut once, from your CrossPointXT dev checkout:

```bash
git pull --ff-only origin dev
bash tools/e213-sync/install-linux-launcher.sh
```

Then open **E213 Personal Sync** from the Linux application menu instead of
running routine terminal commands. The launcher creates a separate Python
virtual environment and installs PySerial on first run. If your distro lacks
Tkinter or venv, install `python3-tk` or `python3-venv` first.

**Personal workflow:**

1. Connect the E213 by USB, keep it awake, close any serial monitor.
2. Confirm the Plot n Jot database path. Linux default:
   `~/.local/share/PlotAndJot/writer_data.db` (honors XDG_DATA_HOME).
3. Click Detect, Load projects, choose your project, then Scan E213.
4. Select an SD document and a Plot n Jot scene; choose which copy wins at
   initial sync and click Link selected. Linking alone does not change prose.
5. Preview changes before choosing Sync now. Close Plot n Jot first.
6. The tool automatically backs up the E213 SD drafts, preserves conflicting
   edits, and backs up the database before pushing text into Plot n Jot.
7. To rename a linked file, use Unlink first (nothing deleted), Ctrl+R on
   the E213 to rename it, then link the new filename in the personal GUI.

Both app preferences and backups are local; no telemetry or server.
The separate GUI reuses the existing revision-checked USB sync protocol.
The GUI has not yet been tested end-to-end on the physical E213.

## USB Plot & Jot synchronization (SD remains inserted)

Connect E213 via USB to laptop. Serial LIST, GET, PUT operate on SD /drafts
and SD /.sync. The bridge updates the existing Plot & Jot SQLite scene records
through explicit scene IDs, with backups and revision-based conflict protection.

Close Plot & Jot first to avoid unsaved application buffers overwriting results.
Install the serial client:

    python -m pip install pyserial

From CrossPointXT (with plot-and-jot-source cloned beside it):

    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --list-projects
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --list-scenes
    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --list-files

Bind a particular SD draft to a particular existing Plot & Jot scene:

    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --bind Draft-001.txt 101 --prefer device

Use --prefer plot if the desktop scene should replace the SD draft. Omit
--prefer if the two copies are identical. Subsequent synchronization:

    python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --sync

Rules:
- SD /.sync records project, chapter and scene IDs and SHA-256 baselines.
- Unlinked drafts never upload, and a wrong-project manifest is rejected.
- If both ends changed, no overwrite: a device conflict copy is kept on laptop.
- Database backup precedes upload; SD retains .bak for overwritten drafts.
- Missing SD drafts do not delete Plot & Jot scenes.
- Do not remove card during writing or USB transfer; power down before removal.

## Testing and limitations

    python -m unittest discover -s test/e213_sync -p "test_*.py"

CI compiles E213 writer and tests sync rules on dev. Actual GPIO wiring,
SD read/write endurance, reboot recovery and a real USB Plot & Jot round trip
still require hardware validation. Direct removable-card-reader sync (without
E213) is a separate, not-yet-implemented transport.


## Text layout and rich-formatting plan

### Smart word wrapping (implemented in dev, hardware validation pending)

The E213 currently uses a compact fixed-width screen font with 39 columns.
The new editor layout wraps before the next word when possible, splitting only
an unbroken word longer than a display row. Hidden wrap spaces stay in the
underlying text, and soft wraps NEVER add newline bytes to the SD manuscript.
Cursor-up/down and cursor placement use the same visual-line mapping. Host
regression tests run via test/e213_sync/word_wrap_test.cpp.

For a truly proportional font, replace the 39-column measure with per-glyph
pixel widths (including bold/italic font variants). Preserve UTF-8 code point
boundaries in cursor movement, deletion and wrapping.

### Bold and italics (planned, NOT implemented yet)

Prefer lightweight Markdown source, with a formatted E213 reading/editing view:
- One asterisk around words for italic, two for bold, three for bold italic.
- Keyboard Ctrl+B / Ctrl+I should toggle styles around a selection; if nothing
  is selected, toggle the style for subsequently typed text.
- The visual editor should draw with matched regular, bold, italic, bold-italic
  Adafruit GFX fonts. Hidden Markdown control characters still need correct
  source-to-screen cursor mapping.
- Save the original text and markup to the SD card. Do not write visual line
  breaks into the document and do not destroy ordinary literal asterisks.
- Rendering must handle incomplete Markdown pairs and escape sequences safely.
- On 250x122 hardware, use compact readable font variants; large 9-12pt fonts
  reduce words per line substantially.

Important compatibility detail: Plot & Jot's current scene editor stores
plain text using setPlainText/toPlainText. Its present UI will show Markdown
asterisks as literal text unless Plot & Jot is upgraded with a matching
formatting-aware editing/rendering mode. USB transfer can preserve the markup
bytes unchanged, but preserving true styled appearance on both devices
requires coordinated changes to BOTH projects.


## Pre-test reliability workflow (recommended order)

These steps are intended for the first physical Heltec V1.1 test. CI cannot
verify the wiring, battery supply, BLE keyboard HID reports or e-ink speed.

1. **Protect the original:** Back up existing flash drafts and manuscripts
   before uploading a new build. Do not erase the whole flash/partition.
2. **Check external SD module power and pinouts:** FAT32 microSD, correct 3.3 V
   logic levels, CS=38 MOSI=39 SCK=40 MISO=41 as *proposed, not verified*.
   Check the module's VCC rating. Start with a throwaway card and test text.
3. **Check boot and card failure handling:** Boot with no card; editor should
   warn and disable writing, not write to internal flash. Reinsert the card,
   reboot, and verify the SD storage-ready console message.
4. **Test writing before syncing:** Pair BLE keyboard; enter several paragraphs,
   use Ctrl+S, Ctrl+N, Ctrl+O, move the cursor, disconnect the keyboard immediately
   after typing, and reboot. The SD draft contents must survive intact.
5. **Back up active SD drafts using USB before the first live Plot & Jot sync.**
   With COM10 connected, no serial monitor, and no keyboard typing:

       python tools/e213-sync/plot_jot_sync.py --port COM10 --backup-device ./e213-backups

   This is READ ONLY on the E213 and does not need the Plot & Jot checkout.
   It checks SHA-256 during transfer and checks its own local copies afterward.
   This USB snapshot includes active TXT drafts and the sync manifest only.
   It is NOT a whole-card image: .bak/.failed recovery files and hidden
   non-manuscript files require a separate full card-reader copy.
6. **Test one explicit Plot & Jot scene link, then preview writes** with the
   desktop Plot & Jot app closed:

       python tools/e213-sync/plot_jot_sync.py --plot-source ../plot-and-jot-source --project 42 --port COM10 --sync --dry-run

   This displays PUSH/PULL/CONFLICT actions and changes neither side. After
   inspecting the preview, run the same command without --dry-run to apply it.
   An initial --bind modifies the card's .sync manifest, so first back up
   manuscripts and choose --prefer deliberately if two copies differ.
7. **Verify both directions:** Edit on Heltec, sync to desktop, inspect exact
   scene content. Edit that same desktop scene, sync back, and inspect SD text.
   Create a conflicting change on *both* sides and confirm neither is overwritten
   and the device copy appears in the laptop's e213-conflicts directory.

Never pull the card or disconnect power during an ordinary live SD write.
Controlled power-loss tests belong after the basic recovery path is proven.

### Still not production-certified

- External microSD GPIO mapping and module power have not been validated.
- There is no battery-low write cutoff, error recovery for unexpected physical
  card removal, or verified endurance/power-cut fault-injection test.
- The current line wrapping is smart at word boundaries but uses 39 **byte**
  columns; full UTF-8/proportional typography and Markdown bold/italic rendering
  are still planned.
- USB transfer is designed for one authorized local laptop, not multiple
  concurrent writers. Do not type during a USB sync; the UI indicates the pause.
- Direct card-reader Plot & Jot sync and per-novel folders are not implemented.


## E213 editing screen status layout

The current minimum built-in bitmap text uses Adafruit GFX setTextSize(1),
**6 px wide × 8 px high per character**, with a 9 px baseline step.
Word wrapping presently uses 39 character/byte columns and nine visible rows.
This is the **smallest text size we intend to offer**. More comfortable 12 px
and 16 px glyph-height font options and font-family selection are planned,
not part of this firmware yet.

Editing-screen status positions (250 × 122 e-ink screen):
- Header LEFT: currently opened document's name, automatically cropped so
  it does not overlap status.
- Header RIGHT: SAVED, UNSAVED, SAVE ERR, or USB SYNC.
- Footer CENTER: BT:OK for connected, BT:-- for disconnected.
- Footer RIGHT: live N words counter, derived from manuscript content.
- Middle: nine rows of actual manuscript text at the current minimum size.

A failed SD save remains a visible SAVE ERR until a later successful save.
UNSAVED means edited content remains in memory while autosave is pending.
Word counting treats whitespace-separated groups containing at least one
letter, digit, or non-ASCII byte as words; this is not yet full Unicode-aware
linguistic segmentation.

Next typography step: genuinely legible font faces sized ~8/12/16 px with
pixel-accurate wrapping and correct cursor mapping, then SD-persisted display
preferences. Any font/size setting must NEVER alter saved manuscript bytes.


## Writerdeck editing shortcuts, focus mode, battery voltage

New in dev (hardware keyboard testing still required):

| Shortcut | Behavior |
| --- | --- |
| Ctrl+Z | Undo; consecutive typing and held-delete strokes are grouped |
| Ctrl+Y | Redo; a new edit clears redo |
| Home / End | Beginning / end of current prose sentence (. ! ? boundaries) |
| Ctrl+Home / Ctrl+End | Beginning / end of entire document |
| Hold Backspace / Delete | Initial press; auto repeat after 450 ms, faster after one and two seconds |
| F11 or Ctrl+Shift+F | Toggle focus mode, hide header/footer and expand from 9 to 13 rows |
| Ctrl+S / Ctrl+N / Ctrl+O / Ctrl+R | Save, name a new document, open picker, rename document |

Undo history stores changes, not manuscript snapshots; it is bounded to 48
operations and approximately 32 KiB per draft. Switching drafts or replacing
the active file over USB clears history. History is RAM-only and does not
survive a reboot; manuscripts continue to save to external SD.

Battery voltage readout: the Heltec HT-VME213 datasheet identifies GPIO17
as ADC_CTRL; the vendor schematic labels GPIO7 as VBAT_Read. The resistor
divider is 390k/100k, so battery volts = measured input volts * 4.9.
At startup and every 60 s, firmware briefly powers the divider, samples the
ADC, and disables the divider again. It displays BAT 3.9V bottom-left when
valid, BAT LOW under 3.45 V, or BAT -- when absent/out of range.

Voltage display is a preliminary onboard-sensor measurement, NOT a
calibrated state-of-charge percentage. The sensing circuit and polarity must
be verified on the user's physical board before enabling low-battery
automatic sleep/shutdown. A suitable protected 1-cell LiPo with the correct
battery-connector polarity is required; never apply 5V to the battery jack.

Still missing versus mature dedicated writerdecks: persistent sleep/wake,
measured battery runtime, fast low-ghosting incremental e-ink refresh,
comfortable Unicode/proportional fonts, selection/clipboard, search/replace,
robust power-loss tests and validated Plot & Jot sync.


## Automatic Bluetooth keyboard reconnect (ported from working v1_1)

The BLE client stores the last successful keyboard's stable address/type in
ESP32 NVS under e213-keyboard/last-peer, using the same versioned identity
layout as the historical port. On boot, it restores that address, or adopts
the one existing NimBLE bond if there is exactly one and no stored address.
The Arduino loop schedules the reconnect; BLE callbacks never do synchronous
connect/discovery. Successful HID subscription remembers the connected peer.
The client is reused on disconnect to avoid repeated NimBLE client allocations.

- Initial reboot: with keyboard awake and in the same BLE device slot, the E213
  should connect without pressing B1/B2.
- If the keyboard is turned off or unavailable, attempts are spaced 20 seconds
  apart. After a successful connection drops, reconnect starts after 2.5 s.
- Press B1 to select a different advertised keyboard, then B2 to connect:
  manual selection temporarily cancels auto retry; the new successful keyboard
  becomes the remembered keyboard.
- When no usable last peer is present, the writer still scans normally. Make
  one successful manual connection to populate NVS before testing automatic
  reconnect.
- For multi-device keyboards, reconnect only works when the keyboard is
  awake and advertising/available on the same paired device slot. Another PC
  or phone may claim the connection first.
- Successful paired/bonded identity should survive RESET, save-and-sleep, and
  power removal. If a keyboard uses only rotating, unbonded random addresses,
  reconnect may require manual pairing again; we do not save a temporary
  private address as a stable identity.
- Serial output includes "Loaded last keyboard identity", "Reconnecting to
  keyboard", and "Keyboard ready" messages to identify the failure stage.

The native host test checks retry timing, rollover, manual-selection override,
and invalid persisted identity structures. Physical BLE reconnect still needs
testing with the actual keyboard.


### Diagnosing a keyboard that needs pairing mode after writer power-cycle

A BLE peripheral can advertise a different set of fields when reconnecting:
it may omit its keyboard name, HID service UUID, and appearance. The dev
scanner now matches the stored identity address/type BEFORE its generic HID
advertisement filter, so a known keyboard is still recognized when such fields
are missing. A detected known advertisement triggers an early auto-connect
attempt after a five-second failure guard; ordinary retries still occur after
20 seconds. It does NOT automatically trust another device merely because
it uses the same keyboard name.

First, pair/connect manually once after flashing. In the serial monitor
(115200), verify:
- "Connection security: bonded=1" and "Saved keyboard identity verified in
  persistent NVS" (bonded=0 is a useful clue if the keyboard won't reconnect).
- On the next RESET or power-cycle: "Loaded last keyboard identity" and
  "Auto-connect attempt" followed by either "Keyboard ready" or the exact
  connection failure. A seen advertisement gives "Saved keyboard IS advertising".
- If no saved peer is reported, NVS storage was absent or the keyboard's
  changing private address could not be remembered. No full-flash erase is
  required simply to test reconnection.

A BLE central cannot force a peripheral that has stopped advertising to
reconnect. Some keyboard models wake and advertise after a keypress or a
short power toggle; some only resume advertising in pairing mode. Test the
same Bluetooth device slot and ensure a PC/phone hasn't claimed the keyboard.
If you see no "Saved keyboard IS advertising" after 30-60 seconds and several
keypresses, the keyboard may not be advertising except in pairing mode; the
serial log and keyboard model are needed to distinguish that limitation from
a failed bond or incorrectly stored address. Do NOT erase the NVS partition or
re-pair repeatedly without checking the logs, because that obscures diagnosis.
