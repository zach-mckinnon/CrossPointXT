#pragma once
// Heltec HT-VME213 battery sense hardware (confirmed in the vendor datasheet
// and schematic): GPIO17 enables ADC, GPIO7 senses through 390k/100k divider.
// Do not apply to a different Heltec board without checking its schematic.
#include <Arduino.h>
#include <cstdio>
#include <string>

namespace writerbattery {
constexpr int kAdcControlGpio = 17;
constexpr int kAdcReadGpio = 7;
constexpr uint32_t kReadIntervalMs = 60000;

inline void begin() {
  pinMode(kAdcControlGpio, OUTPUT);
  digitalWrite(kAdcControlGpio, LOW); // Low-power divider disabled outside reads.
  analogSetPinAttenuation(kAdcReadGpio, ADC_11db);
}
inline int sampleMillivolts() {
  digitalWrite(kAdcControlGpio, HIGH);
  delay(5);
  uint32_t sum = 0;
  for (int i = 0; i < 6; ++i) {
    sum += static_cast<uint32_t>(analogReadMilliVolts(kAdcReadGpio));
    delay(1);
  }
  digitalWrite(kAdcControlGpio, LOW);
  // R17=390k R19=100k, sense voltage * 4.9 => battery millivolts.
  const int voltage = static_cast<int>((sum * 49 + 30) / 60);
  return voltage >= 2800 && voltage <= 4450 ? voltage : 0;
}
inline std::string displayLabel(int millivolts) {
  if (millivolts == 0) return "BAT --";
  if (millivolts < 3450) return "BAT LOW";
  char label[16];
  snprintf(label, sizeof(label), "BAT %d.%dV",
           millivolts / 1000, (millivolts % 1000) / 100);
  return label;
}
} // namespace writerbattery
