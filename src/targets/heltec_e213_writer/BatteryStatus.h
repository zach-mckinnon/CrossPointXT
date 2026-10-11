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
// One raw reading, averaged ADC voltage and the inferred battery voltage.
// An invalid measurement is not necessarily an empty battery: USB power with
// the external battery switch off can leave the sensing circuit unpowered.
struct Sample {
  int adcMillivolts = 0;
  int inferredBatteryMillivolts = 0;
  int validBatteryMillivolts = 0;
};

inline Sample sample() {
  digitalWrite(kAdcControlGpio, HIGH);
  delay(15); // Give the high-impedance divider time to settle.
  (void)analogReadMilliVolts(kAdcReadGpio); // Discard first ADC conversion.
  uint32_t sum = 0;
  for (int i = 0; i < 8; ++i) {
    sum += static_cast<uint32_t>(analogReadMilliVolts(kAdcReadGpio));
    delay(1);
  }
  digitalWrite(kAdcControlGpio, LOW);
  Sample result;
  result.adcMillivolts = static_cast<int>((sum + 4) / 8);
  // R17=390k R19=100k: VBAT = 4.9 * ADC input.
  result.inferredBatteryMillivolts = static_cast<int>((sum * 49 + 40) / 80);
  const int voltage = result.inferredBatteryMillivolts;
  result.validBatteryMillivolts = (voltage >= 2800 && voltage <= 4450) ? voltage : 0;
  Serial.printf("E213 battery: ADC_CTRL GPIO%d HIGH, ADC GPIO%d=%d mV, "
                "estimated VBAT=%d mV, display=%s\\n",
                kAdcControlGpio, kAdcReadGpio,
                result.adcMillivolts, result.inferredBatteryMillivolts,
                result.validBatteryMillivolts ? "valid" : "BAT -- (invalid/unavailable)");
  return result;
}
inline int sampleMillivolts() { return sample().validBatteryMillivolts; }
inline std::string displayLabel(int millivolts) {
  if (millivolts == 0) return "BAT --";
  if (millivolts < 3450) return "BAT LOW";
  char label[16];
  snprintf(label, sizeof(label), "BAT %d.%dV",
           millivolts / 1000, (millivolts % 1000) / 100);
  return label;
}
} // namespace writerbattery
