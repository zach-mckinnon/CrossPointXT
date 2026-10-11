#pragma once
// A reconnecting HID keyboard can advertise with neither its HID UUID,
// keyboard appearance nor name. Exact stored identity matches remain eligible.
#include "HeltecKeyboardIdentity.h"
#include <cstdint>

namespace writerkeyboard {
constexpr bool matchesKnownPeer(const HeltecKeyboardIdentity& stored,
                             const uint8_t* address, uint8_t addressType) {
  if (!stored.valid() || !address || stored.type != addressType) return false;
  for (unsigned i = 0; i < 6; ++i) {
    if (stored.address[i] != address[i]) return false;
  }
  return true;
}
}
