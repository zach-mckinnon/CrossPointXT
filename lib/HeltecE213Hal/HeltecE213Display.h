#pragma once

#include <heltec-eink-modules.h>
#include <Logging.h>
#include <esp_sleep.h>

// Isolated from the Xteink HAL while bringing up the E213 target.
// The E213 display is write-only. The upstream library's dummy MISO GPIO33
// belongs to octal PSRAM on the ESP32-S3R8 and must not be configured as GPIO.
#ifdef HELTEC_E213_PANEL_FC1
using HeltecE213Panel = EInkDisplay_VisionMasterE213;
#else
using HeltecE213Panel = EInkDisplay_VisionMasterE213V1_1;
#endif

class HeltecE213Display final : public HeltecE213Panel {
 public:
  HeltecE213Display() { pin_miso = static_cast<uint8_t>(-1); }
  bool hasTimedOut() const { return timedOut; }

  bool blankForSleep() {
    if (timedOut) return false;
    prepareFrame(true);
    update();
    wait();  // FC1 sends panel power-off at the end of update().
    return !timedOut;
  }

  void sleepUntilReset() {
    display_spi->end();
    // Avoid powering the unpowered panel through its signal pins.
    pinMode(DEFAULT_SDI, INPUT);
    pinMode(DEFAULT_CLK, INPUT);
    pinMode(PIN_DISPLAY_DC, INPUT);
    pinMode(PIN_DISPLAY_CS, INPUT);
    pinMode(PIN_DISPLAY_RST, INPUT);
    Platform::VExtOff();  // GPIO18 has a board-level pull-down during deep sleep.
    // No timer or button wake source: RESET starts a fresh boot.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
  }

  bool cleanupDue() const {
    return partialRefreshCount >= MAX_PARTIAL_REFRESHES && millis() - lastFullRefreshAt >= MIN_FULL_REFRESH_INTERVAL_MS;
  }

  void prepareFrame(const bool forceFull, const bool allowCleanup = true) {
    // Reuse the existing framebuffer. Clear only MCU memory: clearMemory()
    // also writes a blank frame to the panel before the new image is ready.
    const bool full = forceFull || fastmode_state == NOT_SET || (allowCleanup && cleanupDue());
    if (full) {
      if (fastmode_state != OFF) fastmodeOff();
      partialRefreshCount = 0;
      lastFullRefreshAt = millis();
    } else {
      if (fastmode_state != ON) fastmodeOn();
      if (partialRefreshCount < MAX_PARTIAL_REFRESHES) ++partialRefreshCount;
    }
    fullscreen();
    clearPage();
  }

  // Read the controller ID over bidirectional MOSI before SPI initialization,
  // following Heltec's VME213/Global_Simple example (command 0x2f).
  static uint8_t readPanelId() {
    pinMode(PIN_PCB_VEXT, OUTPUT);
    digitalWrite(PIN_PCB_VEXT, HIGH);
    delay(100);
    pinMode(DEFAULT_CLK, OUTPUT);
    pinMode(PIN_DISPLAY_DC, OUTPUT);
    pinMode(PIN_DISPLAY_CS, OUTPUT);
    pinMode(PIN_DISPLAY_RST, OUTPUT);
    digitalWrite(PIN_DISPLAY_RST, LOW);
    delay(20);
    digitalWrite(PIN_DISPLAY_RST, HIGH);
    delay(20);
    digitalWrite(PIN_DISPLAY_DC, LOW);
    digitalWrite(PIN_DISPLAY_CS, LOW);
    pinMode(DEFAULT_SDI, OUTPUT);
    digitalWrite(DEFAULT_CLK, LOW);
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
      digitalWrite(DEFAULT_SDI, (0x2f & mask) ? HIGH : LOW);
      digitalWrite(DEFAULT_CLK, HIGH);
      delayMicroseconds(1);
      digitalWrite(DEFAULT_CLK, LOW);
      delayMicroseconds(1);
    }
    delay(10);
    digitalWrite(PIN_DISPLAY_DC, HIGH);
    pinMode(DEFAULT_SDI, INPUT_PULLUP);
    uint8_t id = 0;
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
      digitalWrite(DEFAULT_CLK, LOW);
      delayMicroseconds(1);
      digitalWrite(DEFAULT_CLK, HIGH);
      delayMicroseconds(1);
      if (digitalRead(DEFAULT_SDI)) id |= mask;
    }
    digitalWrite(PIN_DISPLAY_CS, HIGH);
    return id;
  }

 protected:
  void wait() override {
    if (timedOut) return;
    const uint32_t startedAt = millis();
    while (digitalRead(PIN_DISPLAY_BUSY) == BUSY_LEVEL) {
      if (millis() - startedAt >= 10000) {
        timedOut = true;
        LOG_ERR("E213", "Display BUSY timeout: busy=%d rst=%d power=%d", digitalRead(PIN_DISPLAY_BUSY),
                digitalRead(PIN_DISPLAY_RST), digitalRead(PIN_PCB_VEXT));
        return;
      }
      delay(1);
    }
  }

 private:
  static constexpr uint8_t MAX_PARTIAL_REFRESHES = 100;
  static constexpr uint32_t MIN_FULL_REFRESH_INTERVAL_MS = 60000;
  uint8_t partialRefreshCount = 0;
  uint32_t lastFullRefreshAt = 0;
#ifdef HELTEC_E213_PANEL_FC1
  static constexpr int BUSY_LEVEL = LOW;
#else
  static constexpr int BUSY_LEVEL = HIGH;
#endif
  bool timedOut = false;
};
