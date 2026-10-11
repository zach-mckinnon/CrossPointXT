#pragma once
// E213 writer status math. Independent of Arduino for native regression tests.
#include <cctype>
#include <cstddef>
#include <string>

namespace writerstats {
// Counts whitespace-delimited groups containing at least one letter/number or
// UTF-8 byte. Punctuation-only groups (such as Markdown "**") aren't words.
// This is an offline, language-neutral approximation, not a full ICU segmenter.
inline size_t countWords(const std::string& text) {
  size_t words = 0;
  bool inToken = false;
  bool hasWordCharacter = false;
  for (const unsigned char c : text) {
    if (std::isspace(c)) {
      if (inToken && hasWordCharacter) ++words;
      inToken = false;
      hasWordCharacter = false;
    } else {
      inToken = true;
      if (std::isalnum(c) || c >= 0x80) hasWordCharacter = true;
    }
  }
  if (inToken && hasWordCharacter) ++words;
  return words;
}

// Default GFX bitmap font is 6x8 px at text size 1.
constexpr int kCharacterWidth = 6;
constexpr int kHorizontalMargin = 6;

inline int rightAlignedX(int displayWidth, const std::string& label) {
  const int px = static_cast<int>(label.size()) * kCharacterWidth;
  return displayWidth > px + kHorizontalMargin ?
      displayWidth - kHorizontalMargin - px : 0;
}

inline int centeredX(int displayWidth, const std::string& label) {
  const int px = static_cast<int>(label.size()) * kCharacterWidth;
  return displayWidth > px ? (displayWidth - px) / 2 : 0;
}

inline size_t titleColumns(int statusX) {
  const int available = statusX - kHorizontalMargin - 6; // 6 px clear gap.
  return available > 0 ? static_cast<size_t>(available / kCharacterWidth) : 0;
}
} // namespace writerstats
