# E213 Micro Writer — personalized Heltec V1.1 fork

Active development: dev. No plans to merge into the original CrossPoint project.

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
- Ctrl+N: create a new numbered draft on SD
- Ctrl+S: save to SD; autosave still runs after 1.8 seconds idle
- Reboot restores the last document via SD /.active
- A failed save blocks switching documents; never silently fall back to flash

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
