# Heltec Vision Master E213 V1.1 port

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
