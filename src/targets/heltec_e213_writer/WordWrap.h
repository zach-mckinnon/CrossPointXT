#pragma once
// Word-aware display wrapping for the small E213 e-ink screen.
// Layout never modifies the saved manuscript and never inserts hard line breaks.
// Kept Arduino-independent so host regression tests can exercise cursor mapping.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace writerlayout {

struct VisualLine {
  size_t start = 0; // First source byte represented by the visible line.
  size_t end = 0;   // First source byte AFTER this line, including hidden soft-wrap spaces.
  std::string text;
};

inline bool isSoftBreakSpace(const char ch) {
  return ch == ' ' || ch == '\t';
}

inline std::vector<VisualLine> wrapWords(const std::string& manuscript, const size_t columns) {
  std::vector<VisualLine> lines;
  const size_t width = std::max(static_cast<size_t>(1), columns);
  size_t paragraphStart = 0;

  while (paragraphStart <= manuscript.size()) {
    const size_t newline = manuscript.find('\n', paragraphStart);
    const size_t paragraphEnd = newline == std::string::npos ? manuscript.size() : newline;
    size_t pos = paragraphStart;

    if (pos == paragraphEnd) {
      lines.push_back({pos, pos, ""});
    }

    while (pos < paragraphEnd) {
      const size_t room = paragraphEnd - pos;
      if (room <= width) {
        // Final visual row of a paragraph. No extra newline is written to storage.
        lines.push_back({pos, paragraphEnd, manuscript.substr(pos, room)});
        pos = paragraphEnd;
        continue;
      }

      const size_t hardEnd = pos + width;
      size_t lastBreak = std::string::npos;

      // Prefer the LAST whitespace before the visual width, so the next word
      // begins on a new line, with no accidental mid-word split.
      for (size_t i = pos + 1; i < hardEnd; ++i) {
        if (isSoftBreakSpace(manuscript[i]) &&
            !isSoftBreakSpace(manuscript[i - 1])) {
          lastBreak = i;
        }
      }

      if (lastBreak != std::string::npos) {
        size_t afterBreak = lastBreak;
        while (afterBreak < paragraphEnd && isSoftBreakSpace(manuscript[afterBreak]))
          ++afterBreak;
        lines.push_back({pos, afterBreak, manuscript.substr(pos, lastBreak - pos)});
        pos = afterBreak;
        continue;
      }

      // A word exactly fits, followed by whitespace outside the width.
      if (isSoftBreakSpace(manuscript[hardEnd])) {
        size_t afterBreak = hardEnd;
        while (afterBreak < paragraphEnd && isSoftBreakSpace(manuscript[afterBreak]))
          ++afterBreak;
        lines.push_back({pos, afterBreak, manuscript.substr(pos, width)});
        pos = afterBreak;
        continue;
      }

      // A single word exceeds the screen. Only then break inside the word.
      // Walk backward to avoid splitting a UTF-8 byte sequence. A future
      // variable-width renderer should also measure individual glyph widths.
      size_t safeEnd = hardEnd;
      while (safeEnd > pos + 1 && safeEnd < manuscript.size() &&
             (static_cast<unsigned char>(manuscript[safeEnd]) & 0xC0) == 0x80) {
        --safeEnd;
      }
      lines.push_back({pos, safeEnd, manuscript.substr(pos, safeEnd - pos)});
      pos = safeEnd;
    }

    if (newline == std::string::npos) break;
    paragraphStart = newline + 1;
  }

  if (lines.empty()) lines.push_back({0, 0, ""});
  return lines;
}

// The line following a soft wrap starts at the byte after wrap whitespace;
// the caret on that byte belongs to the new visual line.
inline size_t lineForCursor(const std::vector<VisualLine>& lines, const size_t cursor) {
  if (lines.empty()) return 0;
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    if (cursor < lines[i + 1].start) return i;
  }
  return lines.size() - 1;
}

inline size_t columnForCursor(const VisualLine& line, const size_t cursor) {
  if (cursor <= line.start) return 0;
  return std::min(cursor - line.start, line.text.size());
}

} // namespace writerlayout
