#include <Arduino.h>
#include <Logging.h>
#ifndef HELTEC_E213_SERIAL_ONLY
#include <HeltecE213Display.h>
#endif

static uint8_t panelId = 0;

void setup() {
  logSerial.begin(115200);
  const uint32_t serialStartedAt = millis();
  while (!logSerial && millis() - serialStartedAt < 10000) {
    delay(10);
  }
  delay(250);
  LOG_INF("E213", "DIAG: setup entered");
  logSerial.flush();

#ifndef HELTEC_E213_SERIAL_ONLY
  panelId = HeltecE213Display::readPanelId();
  LOG_INF("E213", "Panel ID: 0x%02x", panelId);
  // Construct only after serial is ready, so constructor failures are visible.
  // Static storage avoids putting the display object on the task stack.
  static HeltecE213Display display;

  LOG_INF("E213", "DIAG: display landscape");
  logSerial.flush();
  display.landscape();

  LOG_INF("E213", "DIAG: drawing");
  logSerial.flush();
  display.clearMemory();
  if (display.hasTimedOut()) return;
  display.setTextColor(BLACK);
  display.setTextSize(2);
  display.setCursor(12, 35);
  display.print("E213 V1.1");
  display.setTextSize(1);
  display.setCursor(12, 58);
  display.print("DISPLAY TEST");

  LOG_INF("E213", "DIAG: update start");
  logSerial.flush();
  display.update();
  if (display.hasTimedOut()) return;
  LOG_INF("E213", "DIAG: update complete");
  logSerial.flush();
#endif
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 2000) {
    last = millis();
    LOG_INF("E213", "DIAG: alive %lu ms, panel=0x%02x", static_cast<unsigned long>(millis()), panelId);
    logSerial.flush();
  }
}
