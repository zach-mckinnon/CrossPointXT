#pragma once

#include <cstddef>
#include <cstdint>

// Standalone page layout for plain-text books. This intentionally has no Arduino
// dependencies so cursor/offset behavior can be tested on the host before
// attaching an SD card. Readers must implement seek(), read(), available(),
// and position() (the Arduino File API supplies all four).
namespace e213 {
constexpr uint8_t kColumns = 38;  // 38 * 6px fits between 8px margins at 250px.
constexpr uint8_t kLines = 10;    // Header and footer remain readable at 122px.

struct Page {
  char lines[kLines][kColumns + 1] = {};
  uint8_t lineCount = 0;
  uint32_t nextOffset = 0;
  bool eof = false;
};

// Adafruit GFX's built-in font is ASCII-only. Normalize common punctuation
// without losing the original UTF-8 byte offsets used by SD bookmarks.
template <class Stream>
char nextGlyph(Stream& file) {
  const int c = file.read();
  if (c < 0) return '\0';
  if (c < 0x80) return static_cast<char>(c);

  // Recognize typical punctuation emitted by Calibre/Markdown. For other
  // non-ASCII characters, show '?' rather than splitting a UTF-8 sequence.
  const int continuationCount = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
  uint32_t codepoint = c & (continuationCount == 1 ? 0x1F : continuationCount == 2 ? 0x0F : 0x07);
  if (!continuationCount) return '?';
  for (int i = 0; i < continuationCount; ++i) {
    const int tail = file.read();
    if (tail < 0) return '?';
    if ((tail & 0xC0) != 0x80) {
      // Do not silently consume the start of a different character.
      file.seek(file.position() - 1);
      return '?';
    }
    codepoint = (codepoint << 6) | (tail & 0x3F);
  }
  switch (codepoint) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    case 0x00A0: return ' ';
    default: return '?';
  }
}

template <class Stream>
bool loadPage(Stream& file, uint32_t offset, Page& result) {
  result = Page{};
  if (!file.seek(offset)) return false;
  if (offset == 0 && file.available()) {
    // Skip an optional UTF-8 BOM only at the start of the book.
    const int b1 = file.read();
    const int b2 = file.read();
    const int b3 = file.read();
    if (!(b1 == 0xEF && b2 == 0xBB && b3 == 0xBF)) file.seek(0);
  }

  while (result.lineCount < kLines && file.available()) {
    char* line = result.lines[result.lineCount];
    uint8_t col = 0;
    bool terminated = false;
    while (file.available() && col < kColumns) {
      const char glyph = nextGlyph(file);
      if (glyph == '\r') continue;
      if (glyph == '\n') {
        terminated = true;
        break;
      }
      line[col++] = glyph == '\t' ? ' ' : glyph;
    }

    // When a line precisely reaches the width, consume one following newline
    // so the next page never acquires a spurious blank line.
    if (col == kColumns && file.available()) {
      const uint32_t at = file.position();
      const int next = file.read();
      if (next == '\r') {
        const uint32_t afterCr = file.position();
        if (file.read() != '\n') file.seek(afterCr);
      } else if (next != '\n') {
        file.seek(at);
      }
    }
    line[col] = '\0';
    if (col > 0 || terminated) ++result.lineCount;
  }
  result.nextOffset = file.position();
  result.eof = !file.available();
  return true;
}
}  // namespace e213
