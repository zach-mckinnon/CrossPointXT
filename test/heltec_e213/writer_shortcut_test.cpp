#include "../../src/targets/heltec_e213_writer/WriterShortcut.h"

// Compile-time tests run without Arduino, Bluetooth, or a connected device.
static_assert(decodeWriterShortcut(0x01, 0x16) == WriterShortcut::Save);
static_assert(decodeWriterShortcut(0x10, 0x16) == WriterShortcut::Save);
static_assert(decodeWriterShortcut(0x01, 0x12) == WriterShortcut::Files);
static_assert(decodeWriterShortcut(0x10, 0x12) == WriterShortcut::Files);
static_assert(decodeWriterShortcut(0x03, 0x12) == WriterShortcut::Suppress);
static_assert(decodeWriterShortcut(0x03, 0x14) == WriterShortcut::Sleep);
static_assert(decodeWriterShortcut(0x30, 0x14) == WriterShortcut::Sleep);
static_assert(decodeWriterShortcut(0x21, 0x14) == WriterShortcut::Sleep);
static_assert(decodeWriterShortcut(0x12, 0x14) == WriterShortcut::Sleep);
static_assert(decodeWriterShortcut(0x01, 0x14) == WriterShortcut::Suppress);
static_assert(decodeWriterShortcut(0x03, 0x16) == WriterShortcut::Suppress);
static_assert(decodeWriterShortcut(0x00, 0x16) == WriterShortcut::None);
static_assert(decodeWriterShortcut(0x02, 0x14) == WriterShortcut::None);

constexpr bool modifiedKeysNeverBecomeText() {
  for (int modifier = 0; modifier < 256; ++modifier) {
    for (int key = 4; key <= 0x52; ++key) {
      const auto shortcut = decodeWriterShortcut(modifier, key);
      if ((modifier & 0xDD) && shortcut == WriterShortcut::None) return false;
      if ((modifier & 0xCC) && shortcut != WriterShortcut::Suppress) return false;
    }
  }
  return true;
}
static_assert(modifiedKeysNeverBecomeText());
