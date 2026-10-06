#include <Arduino.h>
#include <heltec-eink-modules.h>

EInkDisplay_VisionMasterE213V1_1 display;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("E213 DIAG: setup entered");
  Serial.flush();

  Serial.println("E213 DIAG: display landscape");
  Serial.flush();
  display.landscape();

  Serial.println("E213 DIAG: drawing");
  Serial.flush();
  display.clearMemory();
  display.setTextColor(BLACK);
  display.setTextSize(2);
  display.setCursor(12, 35);
  display.print("E213 V1.1");
  display.setTextSize(1);
  display.setCursor(12, 58);
  display.print("DISPLAY TEST");

  Serial.println("E213 DIAG: update start");
  Serial.flush();
  display.update();
  Serial.println("E213 DIAG: update complete");
  Serial.flush();
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 2000) {
    last = millis();
    Serial.printf("E213 DIAG: alive %lu ms\n", static_cast<unsigned long>(millis()));
    Serial.flush();
  }
}
