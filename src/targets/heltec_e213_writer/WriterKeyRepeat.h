#pragma once
// Keyboard hold profile: a pause to avoid accidental repeats,
// followed by progressively faster repeat while the key stays down.
#include <cstdint>
namespace writerrepeat {
constexpr uint32_t firstRepeatDelayMs = 450;
constexpr uint32_t interval(uint32_t heldMilliseconds) {
  return heldMilliseconds > 2000 ? 40 :
         heldMilliseconds > 1000 ? 80 : 135;
}
}
