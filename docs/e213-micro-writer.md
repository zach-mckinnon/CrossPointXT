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
