#pragma once

#include <cstdint>

// Scheduling only: owned by the Arduino loop, never by BLE callbacks.
class KeyboardReconnect final {
 public:
  static constexpr uint32_t RETRY_DELAY_MS = 20000;
  static constexpr uint32_t DISCONNECT_DELAY_MS = 2500;
  static constexpr uint32_t ADVERTISEMENT_RETRY_MS = 5000;

  constexpr void begin(bool remembered, uint32_t now) {
    automatic = remembered;
    waitFrom = now;
    waitMs = 0;
  }
  constexpr bool due(uint32_t now) const { return automatic && uint32_t(now - waitFrom) >= waitMs; }
  // Advertising from the exact stored peer can trigger an earlier retry,
  // without creating a tight loop when a keyboard rejects connections.
  constexpr bool knownAdvertisementDue(uint32_t now) const {
    return automatic && uint32_t(now - waitFrom) >= ADVERTISEMENT_RETRY_MS;
  }
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
