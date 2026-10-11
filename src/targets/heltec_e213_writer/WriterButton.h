#pragma once
#include <cstdint>

enum class ButtonEvent { None, Short, Long };

// Release-triggered actions: a long press can never also open/save a file.
// Input is sampled in loop, so no ISR accesses display or storage.
struct WriterButton {
  bool raw = false;
  bool stable = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  ButtonEvent update(bool pressed, uint32_t now) {
    if (pressed != raw) { raw = pressed; changedAt = now; }
    if (raw == stable || now - changedAt < 35) return ButtonEvent::None;
    stable = raw;
    if (stable) { pressedAt = now; return ButtonEvent::None; }
    return now - pressedAt >= 1200 ? ButtonEvent::Long : ButtonEvent::Short;
  }
};
