#pragma once

#include <cstdint>

// Scheduling only: owned by the Arduino loop, never by BLE callbacks.
class KeyboardReconnect final {
 public:
  static constexpr uint32_t RETRY_DELAY_MS = 20000;
  static constexpr uint32_t DISCONNECT_DELAY_MS = 2500;

  constexpr void begin(bool remembered, uint32_t now) {
    automatic = remembered;
    waitFrom = now;
    waitMs = 0;
  }
  constexpr bool due(uint32_t now) const { return automatic && uint32_t(now - waitFrom) >= waitMs; }
  constexpr bool enabled() const { return automatic; }
  constexpr void manualSelection() { automatic = false; }
  constexpr void failed(uint32_t now) { waitFrom = now; waitMs = RETRY_DELAY_MS; }
  constexpr void connected(uint32_t now) { automatic = true; disconnected(now); }
  constexpr void disconnected(uint32_t now) { waitFrom = now; waitMs = DISCONNECT_DELAY_MS; }

 private:
  bool automatic = false;
  uint32_t waitFrom = 0;
  uint32_t waitMs = 0;
};
