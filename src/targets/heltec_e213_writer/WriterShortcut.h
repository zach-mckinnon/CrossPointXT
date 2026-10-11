#pragma once

#include <cstdint>

enum class WriterShortcut : uint8_t { None, Save, Sleep, Files, Suppress };

// HID modifier bits apply equally to the left and right modifier keys.
constexpr WriterShortcut decodeWriterShortcut(const uint8_t modifiers, const uint8_t keyCode) {
  constexpr uint8_t CTRL = 0x11;
  constexpr uint8_t SHIFT = 0x22;
  constexpr uint8_t ALT_OR_GUI = 0xCC;
  if (modifiers & ALT_OR_GUI) return WriterShortcut::Suppress;
  if (!(modifiers & CTRL)) return WriterShortcut::None;
  if (keyCode == 0x16 && !(modifiers & SHIFT)) return WriterShortcut::Save;  // Ctrl+S
  if (keyCode == 0x12 && !(modifiers & SHIFT)) return WriterShortcut::Files;  // Ctrl+O
  if (keyCode == 0x14 && (modifiers & SHIFT)) return WriterShortcut::Sleep;  // Ctrl+Shift+Q
  return WriterShortcut::Suppress;  // Unsupported shortcuts must not insert letters.
}
