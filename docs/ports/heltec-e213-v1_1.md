# Historical v1_1 hardware bring-up record

> These notes document the independently tested `port/heltec-e213-v1_1` implementation. `dev` now runs a different SD-only editor. See [the active dev writer guide](../e213-micro-writer.md) for current flash, USB sync, and button instructions. The older internal-checkpoint/nested-folder writer and its SD-card sync package remain historical, not live `dev` editor behavior.

# Heltec Vision Master E213 V1.1 port

## Current writer implementation (2026-10-09)

SD saving, paged file exploration, button save-and-sleep, and a PC-side Plot &
Jot transfer tool are now implemented. See [the current controls, transfer guide,
and acceptance checklist](heltec-writer-files-and-sync.md). The earlier sections
below record bring-up milestones: their internal-only storage and inactive
editor-button limitations describe the older firmware.

## Soldered hardware test (2026-10-08)

The user soldered the enclosure wiring, flashed `heltec_e213_writer`, and
inserted a freshly formatted card. A 2026-10-08 COM10 capture confirmed a
1882 MiB card mounted and passed the complete 1024-byte write/read/cleanup
test. Startup loaded the existing 100-byte internal draft and completed with
`ready=1 displayTimeout=0 sdBootTest=1 buttons=1/1`. A GPIO47 short release
was captured, followed by an internal save of 108 bytes. The user confirmed
both newly soldered buttons work, the keyboard connects, and typing works.
Heartbeats remained at `ready=1 displayTimeout=0 sdBootTest=1` through 76.4
seconds, with free heap stable at 233,268 bytes over the final samples.
The user subsequently confirmed battery-only writing, the light/power-cycle
check, and recovery of the draft after powering back on and reconnecting the
keyboard. The user also confirmed `SD:OK` on the battery-powered startup.

The writer now uses normally-open buttons from GPIO42 (B1 next/scan) and GPIO47
(B2 connect) to GND, with internal pull-ups. Short/long releases are logged even
while a keyboard is connected; in the editor the buttons have no action yet.
The onboard USER/BOOT buttons no longer control pairing in this build.

| Peripheral | Board pad | Row with USB on the right |
| --- | --- | --- |
| SD 3V3 | 3V3 | Top, between 46 and 17 |
| SD GND and button returns | GND | Either row |
| SD CS | 38 | Bottom |
| SD MOSI | 39 | Bottom |
| SD CLK | 40 | Bottom |
| SD MISO | 41 | Bottom |
| B1 | 42 | Top, leftmost square pad |
| B2 | 47 | Bottom, between 48 and 38 |

Leave the QuickLink I2C connector unused because it shares GPIO38/39.

`HeltecE213StorageProbe` is a setup-only diagnostic in the existing isolated
E213 HAL. It owns all card access during the check, before BLE starts; it does
not expose file handles or introduce an alternate general-purpose storage API.
The probe uses FSPI at 1 MHz because the verified display library owns HSPI.
It mounts without formatting, exclusively creates a randomly named
`/.e213-test-XXXXXXXX.tmp`, writes 1024 deterministic bytes through a 64-byte
stack buffer, flushes/closes, reopens and verifies every byte and EOF, removes
only its own file, then unmounts and releases SPI. Existing files are not
overwritten. A power interruption can leave a test file behind. SDK mount/file
bookkeeping is temporary, limited to one open file; no application heap buffer
or additional framebuffer is introduced. Full writer SD integration must use
the project's HalStorage/HalFile interface; this probe is not draft storage.

The pairing header shows `SD:OK` only if the entire startup test passes, or
`SD:FAIL` if it fails (including absent card or cleanup failure). It is a boot
test result, not live card detection. Insert/remove the card with power off;
reset to run the check again. Drafts still save to internal `/draft.txt`.
Internal LittleFS now mounts with automatic formatting disabled so a mount
failure cannot erase a saved draft.

Build: 35,056 bytes static RAM and 975,871 bytes flash. Compared with the prior
recorded writer build, static RAM increases by 24 bytes and flash by 2,100 bytes;
runtime heap during the card check has not been measured. `git diff --check`
passes. The following checklist distinguishes the remaining physical checks
from the serial-confirmed startup/card test above:

1. With USB/battery disconnected, check polarity, solder bridges, and each
   button's continuity (open released, closed to GND pressed).
2. Insert the card, leave the enclosure LED off, turn the battery switch on,
   connect USB, upload the writer, and capture serial at 115200.
3. Confirm `Startup write/read/cleanup test: PASS (1024 bytes)`, `SD:OK`, and
   stable heartbeats with `ready=1 displayTimeout=0 sdBootTest=1`.
4. Press/release B1 and B2 separately. Expect GPIO42 then GPIO47 release logs;
   idle raw levels are `buttons=1/1`. Pair a keyboard using B1/B2.
5. Type a unique suffix, Ctrl+S, then Ctrl+Shift+Q; reset/reconnect and verify
   persistence. Test battery-only writing by unplugging USB with the main
   switch still on, then test the light switch.

The main switch is in battery positive, not USB power. Leave it on for charging.
Full hardware off requires USB unplugged and the main switch off. The separate
3V3 LED switch can remain powered in deep sleep and should be switched off too.

### Automatic keyboard reconnect (2026-10-08)

The writer now remembers the last keyboard after a working HID subscription.
The E213 HAL stores one validated, versioned 8-byte identity record in NVS
(`e213-keyboard` / `last-peer`), writing only when that record changes. NimBLE
continues to own and persist Bluetooth security keys. For bonded keyboards the
saved address is the peer identity, not its potentially rotating radio address.
Unbonded private addresses are not persisted as reconnect targets.

Startup tries the remembered address without requiring a pairing advertisement.
On the first upgrade, exactly one existing NimBLE bond is used as an unambiguous
fallback; multiple bonds require one manual keyboard selection. After a failed
attempt, automatic retry waits 20 seconds while ordinary scanning continues.
After losing a working connection, the first retry waits 2.5 seconds. Connection
attempts use the existing 8-second connect timeout (security/service discovery
have their own SDK waits). These operations still run synchronously in the main
loop; the reconnect screen is rendered before starting an automatic attempt.
The keyboard must be awake, using its existing paired slot, and connectable.

B1 cancels automatic retries for the current manual-selection session; B2
connects the selected candidate. A successful manual connection becomes the
remembered keyboard. Connection failures do not overwrite the remembered peer.
The draft stays in RAM across reconnects and continues saving internally.

The client is now retained and reused after disconnect instead of losing its
pointer and allocating another client each time. All connection/discovery and
NVS work stays in the Arduino loop; callbacks only publish state/input. Reconnect
state and identity use fixed-size members, with no new application heap buffer.
NimBLE's existing client/service allocations and transient Preferences handles
remain SDK-owned. No new framebuffer or task is added.

Validation: compile-time tests cover first-use/manual behavior, retry delays,
timer wraparound, and invalid identity records; existing shortcut tests also
pass. The writer builds at 35,144 bytes static RAM / 982,327 bytes flash
(+88 / +6,456 from the hardware-test build). Physical reconnect testing is
pending; the board was disconnected from USB during preparation.

Acceptance: connect once if necessary, save the draft, leave the keyboard on its
paired slot, then reset or power-cycle only the writer. It should return to the
draft automatically; type to confirm reports. Next turn the keyboard off/on and
check recovery without buttons. With the keyboard off, use B1 to confirm manual
selection remains available between attempts. Repeated reconnects should not
consume additional client slots; inspect the heartbeat heap and disconnect logs.

## USB/display troubleshooting (2026-10-06)

The writer upload was verified on COM10 (ESP32-S3, 16 MB flash, 8 MB embedded
PSRAM). A serial-only sketch boots and prints a heartbeat. The E0213A367 display
diagnostic reaches initialization but times out with `busy=1 rst=1 power=1`.
This explains an apparently successful flash followed by a blank screen: the
original display library waits indefinitely before the writer draws its UI.
The original writer also prints its startup messages only once, after 250 ms.

The user physically confirmed that the FC1 diagnostic displays `E213 V1.1` and
`DISPLAY TEST`. This board therefore requires the **LCMEN2R13EFC1-compatible
driver**, despite the V1.1 PCB marking. The writer now selects that driver with
`HELTEC_E213_PANEL_FC1=1`. The E0213A367 driver's opposite BUSY polarity was the
confirmed cause of the initialization hang.

The writer and diagnostic now use a bounded BUSY wait and recurring serial output. The
isolated `HeltecE213Hal` adapter disables the library's dummy GPIO33 MISO input;
GPIO33 is reserved for octal PSRAM on ESP32-S3R8. No additional framebuffer is
allocated by the adapter. Logging uses the existing log ring buffer; the writer
heartbeat reports readiness, display timeout state and free heap every five seconds.

Available diagnostic targets:

- `heltec_e213_serial`: USB heartbeat, no display library.
- `heltec_e213_diag`: E0213A367 driver, BUSY high means busy.
- `heltec_e213_diag_fc1`: LCMEN2R13EFC1 driver, BUSY low means busy.

```powershell
pio run -e heltec_e213_diag_fc1 -t upload --upload-port COM10
pio device monitor -e heltec_e213_diag_fc1 -p COM10 -b 115200
```

The board's panel-ID probe returns `0xff`. Heltec's
[Global_Simple example](https://github.com/HelTecAutomation/Heltec_ESP32/blob/master/examples/VME213/Global_Simple/Global_Simple.ino)
selects the FC1 driver when the low two ID bits are not `01`, but `0xff` alone
does not prove panel identity: an unresponsive bus can also read all ones.
The FC1 diagnostic completed without a BUSY timeout and its test image was
physically confirmed. Other boards still require visual verification; a successful
return from the driver is not proof that the panel refreshed.

To run the writer on this verified panel:

```powershell
pio run -e heltec_e213_writer -t upload --upload-port COM10
pio device monitor -e heltec_e213_writer -p COM10 -b 115200
```

The updated writer was built, flashed and observed reporting `Writer alive:
ready=1 displayTimeout=0`, with approximately 239 KB free heap. The user has
confirmed keyboard pairing and typing. On 2026-10-07 the user also reported that
Ctrl+S and the quit shortcut worked and the draft survived. For an
E0213A367-equipped board, omit the
`HELTEC_E213_PANEL_FC1` flag instead of inferring the panel from the PCB revision.

Repeated black/white flashes during keyboard scanning were investigated with a
serial capture: uptime increased from 5.8 to 49.7 seconds without repeated startup
messages. Both writer render functions were ignoring `forceFull`, leaving every
scan-status update in full-refresh mode. The adapter now uses partial refresh for
routine updates, with full refresh at startup and explicit transitions. It clears
the existing MCU framebuffer without first sending a
blank frame to the panel. No additional framebuffer or heap allocation is added.
This change was built and flashed; serial heartbeats report `ready=1` and
`displayTimeout=0`, with uptime increasing through 68.8 seconds. The user
confirmed that the pairing screen is now steady and readable.

Follow-up typing feedback and a device photo showed a caret above the active row,
a divider through the header and a clipped footer. The built-in font uses a 6x8
cell with a top-left origin, not baseline coordinates. The writer now draws the
caret from the row's top through its bottom, places the header above its divider,
and anchors the footer inside the display height. Text wrapping is disabled in
the driver because the writer already wraps body text itself.

The original 20-partial-update cleanup interval was too frequent while typing.
Cleanup now requires 100 partial updates and at least 60 seconds since the last
full refresh; in the editor it also waits for three seconds without keyboard
input. The count saturates while typing continues, so cleanup cannot wrap around
and be lost. Typing debounce is now 120 ms idle / 500 ms between render attempts
(previously 220 / 900 ms); physical panel refresh time is additional. These changes
reuse the existing framebuffer and add no heap allocations.
The follow-up build and COM10 upload passed; captured heartbeats remained at
`ready=1 displayTimeout=0`. The user confirmed that the cursor and layout now look
right. Longer typing and ghosting checks for the new cleanup interval remain pending.

Acceptance for this follow-up: confirm the caret aligns with typed characters on
each row, the header/footer are readable, and sustained typing does not cause a
full cleanup flash. After enough updates, pause for three seconds and check that
cleanup leaves text readable. If ghosting builds up before cleanup, the interval
needs tuning on the actual panel. Caps Lock and held-key repeat remain unsupported.

### Writer shortcuts and enclosure light

- `Ctrl+S`: save immediately and update the footer. Unchanged drafts are not
  rewritten. Both left and right Ctrl work.
- `Ctrl+Shift+Q`: save, blank the e-ink panel with a full refresh, stop BLE, unmount
  LittleFS, switch off display peripheral power, and enter ESP32 deep sleep.
  Tap **RESET** without holding BOOT to wake, then reconnect the keyboard.
  This is sleep, not a physical battery disconnect; keyboard input cannot wake it.
- A failed save cancels sleep and keeps the unsaved draft in RAM. A failed display
  blank or BLE shutdown also cancels sleep. Save errors are shown in the footer
  and logged; automatic retries are limited to once every five seconds.
- Unsupported Ctrl/Alt/GUI combinations are suppressed rather than inserting the
  shortcut letter. Caps Lock and held-key repeat remain unchanged.

Saving writes `/draft.tmp`, closes it and renames it over `/draft.txt` using
LittleFS replacement semantics; it no longer deletes the previous draft before
the rename. The shortcut decoder has compile-time tests for left/right/mixed
modifiers and suppression of unsupported modified input:

```powershell
& "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp32s3-elf-g++.exe" -std=c++17 -fsyntax-only test/heltec_e213/writer_shortcut_test.cpp
```

These tests, the writer build and the COM10 upload pass. A post-upload heartbeat
reported `ready=1 displayTimeout=0` before the USB serial connection dropped;
that alone does not establish successful sleep. On 2026-10-07 the user physically
tested Ctrl+S and Ctrl+Shift+Q and reported that both worked and the text survived.
This confirms the user-observed save/quit/persistence path; battery-only behavior,
sleep current and failure recovery have not been separately verified. The new error messages use the existing
i18n module and generator. Build usage is 35,032 bytes static RAM and 973,771
bytes flash (previously 34,864 / 754,851); the increase is mainly linked translation
tables in flash. No additional application heap buffers or framebuffer are added.
Repeatable acceptance check: type a unique suffix, press Ctrl+S and check the saved
footer; type another suffix and immediately press Ctrl+Shift+Q; confirm a blank
screen, then RESET/reconnect and confirm both suffixes survived without an extra
`s` or `Q` being inserted. Sleep current has not been measured.

The Heltec board (not the ESP32 chip) includes single-cell 3.7 V Li-ion/LiPo
charging via USB. A small enclosure LED can share the board's regulated 3V3 rail:
`3V3 -> switch -> resistor -> LED anode; LED cathode -> GND`. For a small white
indicator LED, 330 ohms is a conservative starting value; actual current is
approximately `(3.3 V - LED forward voltage) / resistance`. For example, a 3.0 V
LED gives about 0.9 mA. Confirm the LED's ratings and brightness. Do not omit the
resistor or power an illumination LED directly from a GPIO. A GPIO-controlled
version should switch the LED with a suitable transistor/MOSFET. The manual
3V3 light remains powered during deep sleep unless its switch is opened.
Use the board battery connector with verified polarity and a compatible cell;
an extra charger is unnecessary. Sources: [Heltec product specifications](https://heltec.org/project/vision-master-e213/)
and [published board schematic](https://resource.heltec.cn/download/HT-VME213/HT-VME213_Schematic_Diagram_V1.0.pdf).

The older planning and reader milestone notes below predate these diagnostics.

Status: **planning / unbuilt**. This document records the verified hardware facts and implementation sequence for a Heltec port of CrossPointXT. Nothing in this branch has been flashed or validated on the target yet.

## Target hardware

- Board: Heltec Vision Master E213 **V1.1**, ESP32-S3, 16 MB SPI flash, 8 MB PSRAM (confirmed with esptool `flash-id`).
- Display: 250 × 122 monochrome E-Ink. Meshtastic's E213 firmware uses `GxEPD2_213_E0213A367` for the later board/panel revision and `GxEPD2_213_FC1` for the original revision.
- Display signals: SCLK 4; MOSI 6; CS 5; DC 2; RST 3; BUSY 1.
- Display power enable: GPIO18, **active HIGH**.
- Built-in controls: GPIO21 user button; GPIO0 BOOT button (**avoid holding GPIO0 during startup**).
- LoRa is not required and must remain disabled in this port.

Source: [Meshtastic E213 variant](https://github.com/meshtastic/firmware/blob/master/variants/esp32s3/heltec_vision_master_e213/variant.h) and [Meshtastic E213 PlatformIO environment](https://github.com/meshtastic/firmware/blob/master/variants/esp32s3/heltec_vision_master_e213/platformio.ini).

## Proposed *external* SPI microSD module

These are the proposed assignments based on the intended physical alignment; **not yet validated with the attached module**.

| SD signal | ESP32-S3 GPIO |
|---|---:|
| CS | 38 |
| MOSI | 39 |
| SCK | 40 |
| MISO | 41 |
| VCC | 3V3 (only if the module accepts 3.3 V power) |
| GND | GND |

Check module pin labels and orientation physically before soldering. Do not join power pads to adjacent GPIOs merely because the holes line up. Verify SD communication with temporary wires first.

## Important repository constraints

This CrossPointXT fork currently starts from the X3/X4 **ESP32-C3** edition. Its `platformio.ini` defaults to `esp32-c3-devkitm-1`, uses a git submodule at `open-x4-sdk` (pointing at `crosspoint-reader/community-sdk`), and provides the existing plain-text editor at `src/activities/editor/TextEditorActivity.cpp`.

Its older SDK has a display implementation designed specifically for X3/X4. **Do not assume that current upstream CrossPoint's newer FreeInk SDK can simply be substituted.** Port the older SDK/HAL carefully, or plan a separate upgrade with a code-diff review. Since the SDK is a git submodule, upstreamable SDK changes should go in a separately versioned SDK fork/submodule commit, not an untracked local edit.

References: [CrossPointXT platformio.ini](../platformio.ini), [CrossPointXT HAL](../lib/hal/HalDisplay.cpp), [CrossPointXT editor](../src/activities/editor/TextEditorActivity.cpp), [SDK](https://github.com/crosspoint-reader/community-sdk).

## Work sequence

1. **Preserve rollback:** take and verify a 16 MB read-only backup of existing Meshtastic firmware and save Meshtastic configuration separately. Treat the raw flash image as private because it may contain credentials.
2. **Set up development build:** add a separate `heltec-e213` PlatformIO environment targeting ESP32-S3/16MB/8MB PSRAM. Avoid changing the existing default X3/X4 target. Check the memory configuration and boot/upload path.
3. **HAL input and power:** isolate ESP32-C3/Xteink-specific assumptions in `HalGPIO`, `HalPowerManager`, and SDK input/power code. Map GPIO21 and GPIO0, and handle GPIO18 display power.
4. **Display integration:** add a 250 × 122 profile and adapt the *known-working* E0213A367 GxEPD2 implementation to the existing `EInkDisplay` API, including framebuffer format, refresh modes and sleep. Smoke-test a static screen; the display itself is already known to work under Meshtastic.
5. **Storage:** add the external SPI SD pin configuration, confirm FAT32 read/write and safe card detection, and verify CrossPointXT cache and note storage.
6. **UI adaptation:** scale/replace fixed 800 × 480 and 792 × 528 layouts for 250 × 122. Start with book list, TXT reader and page controls, then EPUB rendering and settings. Text editor and keyboard UI come after reading works.
7. **Writing and future Plot & Jot:** reuse the existing BLE text editor, adapt editing layouts for this small panel, then add a *separate* scene-aware sync protocol with conflict preservation. Do not directly expose the Plot & Jot SQLite database to the ESP32.

## Acceptance criteria for first useful milestone

- Builds without changing the X3/X4 firmware behavior.
- Boots on the E213 V1.1 and reports diagnostics over USB serial.
- Handles e-ink initialize/draw/refresh/sleep without hangs.
- Reads a TXT file from the external SD card, paginates it, and allows forward/back navigation.
- Restores reading position after reboot.
- No automatic manuscript writes or sync until storage reliability is verified.

## Commands (Windows PowerShell)

```powershell
git clone --recursive https://github.com/zach-mckinnon/CrossPointXT.git
cd CrossPointXT
git switch port/heltec-e213-v1_1
git submodule update --init --recursive
```

Do **not** flash the current `default` build: it targets ESP32-C3/Xteink hardware, not this ESP32-S3/Heltec board.


## IMPLEMENTED: E213 TXT-reader bring-up target (first milestone)

The branch now includes an **isolated** PlatformIO environment and a working *source-level* TXT-reader prototype. The complete CrossPointXT EPUB/UI/editor port is **not yet implemented**, and the Heltec firmware has not yet been verified on physical hardware.

Files:
- \`platformio.ini\`: \`[env:heltec_e213]\` ESP32-S3, 16 MB flash, 8 MB OPI PSRAM, QIO flash.
- \`src/targets/heltec_e213/main.cpp\`: GxEPD2 E0213A367 display, SD card, library menu, two-button navigation, TXT reading and last-reading-position persistence.
- \`src/targets/heltec_e213/TextPager.h\`: portable, offset-preserving TXT pagination with common UTF-8 punctuation normalized for the initial ASCII font.
- \`test/heltec_e213/pager_test.cpp\`: host tests covering CRLF, empty lines, wrapped lines, UTF-8 BOM/smart quotes, offsets, and invalid seeks.
- \`.github/workflows/e213-build.yml\`: host tests and ESP32-S3 firmware compile; firmware artifact is explicitly **unverified** until flashed and tested.

The target deliberately bypasses the original ESP32-C3 HAL while confirming the actual wiring and file I/O. Once these are proven, we can integrate the existing CrossPointXT EPUB and BLE editor activities into an E213-specific HAL. Do **not** mistake this bring-up target for a completed CrossPointXT port.

### Windows build and test

\`\`\`powershell
git switch port/heltec-e213-v1_1
git pull
git submodule update --init --recursive
pio run -e heltec_e213
\`\`\`

**Before uploading**, save a full 16 MB Meshtastic backup, check that the file is 16,777,216 bytes, and export any device configuration you need to keep. The bootloader can typically restore this image if the custom firmware fails, but this is not a replacement for hardware-safe wiring.

\`\`\`powershell
# Replace COM10 if Windows enumerates the board differently.
pio run -e heltec_e213 -t upload --upload-port COM10
pio device monitor -p COM10 -b 115200
\`\`\`

The first prototype is intended for **USB power only**. Battery monitoring, deep sleep, EPUB rendering, Wi-Fi transfers, and BLE editing are still pending.

### SD card layout

Format your card as FAT32 and add plain-text files. Example:

\`\`\`text
/books/
  Short Story.txt
  My Manuscript.txt
\`\`\`

The reader creates \`/books\` if missing and stores its last-open-book position in \`/.e213-state.txt\`. This initial version lists up to 40 TXT files from \`/books\` (no nested-folder browsing). It is **read-only**: no text-editor writes or manuscript synchronization yet.

### Buttons

- **Library:** GPIO21 USER short press = next book; GPIO0 BOOT short press = open; GPIO0 long press = rescan.
- **Reading:** GPIO21 short press = next page; GPIO0 short press = previous page; GPIO21 long press = library.
- **SD error:** GPIO21 = retry mounting.

GPIO0 is a strapping/BOOT pin: never keep it pressed while resetting or powering on. First uploads may require the usual BOOT + RESET sequence.

### Rollback

If Meshtastic must be restored, use the saved raw 16 MB backup, not a randomly selected firmware file:

\`\`\`powershell
esptool --chip esp32s3 -p COM10 write-flash 0x0 .\meshtastic-backup.bin
\`\`\`

Confirm your actual backup filename and COM port first. Keep the image private, as it can contain stored configuration and credentials.


**Important:** The GitHub Actions artifact currently contains the **application `firmware.bin` only**. Do not write it by itself over Meshtastic with `esptool`: the old bootloader/partition table may not match this target's `partitions.csv`. Use the PlatformIO `upload` command above to flash the matched bootloader, partition table, and firmware together. The Actions build proves compilation, not safe boot on the actual E213.
