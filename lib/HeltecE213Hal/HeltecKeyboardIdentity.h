#pragma once

#include <cstdint>

// Single versioned NVS blob, with no padding or heap-backed strings.
struct HeltecKeyboardIdentity {
  uint8_t version = 0;
  uint8_t type = 0;
  uint8_t address[6] = {};

  constexpr bool valid() const {
    if (version != 1 || type > 3) return false;
    bool nonzero = false;
    bool nonbroadcast = false;
    for (uint8_t b : address) {
      nonzero = nonzero || b != 0;
      nonbroadcast = nonbroadcast || b != 255;
    }
    return nonzero && nonbroadcast;
  }
  constexpr bool operator==(const HeltecKeyboardIdentity& other) const {
    if (version != other.version || type != other.type) return false;
    for (unsigned i = 0; i < 6; ++i) if (address[i] != other.address[i]) return false;
    return true;
  }
};
static_assert(sizeof(HeltecKeyboardIdentity) == 8);

class HeltecKeyboardIdentityStore final {
 public:
  static bool load(HeltecKeyboardIdentity& identity);
  static bool save(const HeltecKeyboardIdentity& identity);
};
