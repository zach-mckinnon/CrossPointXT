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
