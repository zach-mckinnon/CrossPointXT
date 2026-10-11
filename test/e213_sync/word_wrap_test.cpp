#include "../../src/targets/heltec_e213_writer/WordWrap.h"
#include <cassert>
#include <string>

using writerlayout::wrapWords;
using writerlayout::lineForCursor;
using writerlayout::columnForCursor;

int main() {
  {
    const std::string text = "The extraordinary butterfly returned home";
    const auto lines = wrapWords(text, 20);
    assert(lines.size() == 3);
    assert(lines[0].text == "The extraordinary");
    assert(lines[1].text == "butterfly returned");
    assert(lines[2].text == "home");
    // Soft visual breaks must never modify the actual manuscript.
    assert(text == "The extraordinary butterfly returned home");
    assert(lines[0].end == lines[1].start);
    assert(lineForCursor(lines, lines[1].start) == 1);
    assert(lineForCursor(lines, 4) == 0);
    assert(columnForCursor(lines[1], lines[1].start + 3) == 3);
  }
  {
    const auto lines = wrapWords("the supercalifragilistic word", 8);
    assert(lines.size() >= 3);
    assert(lines[0].text == "the");
    assert(lines[1].text == "supercal");
    assert(lines[2].text == "ifragili");
  }
  {
    const auto lines = wrapWords("12345678 XYZ", 8);
    assert(lines.size() == 2);
    assert(lines[0].text == "12345678");
    assert(lines[1].text == "XYZ");
  }
  {
    const auto lines = wrapWords("alpha   beta", 7);
    assert(lines.size() == 2);
    assert(lines[0].text == "alpha");
    assert(lines[1].text == "beta");
    assert(lines[0].end == lines[1].start);
    assert(columnForCursor(lines[0], 6) == 5); // Hidden soft break spaces.
  }
  {
    const auto lines = wrapWords("one two\nthree four", 8);
    assert(lines.size() == 3);
    assert(lines[0].text == "one two");
    assert(lines[1].text == "three");
    assert(lines[2].text == "four");
  }
  {
    const auto lines = wrapWords("one\n\n", 8);
    assert(lines.size() == 3);
    assert(lines[0].text == "one");
    assert(lines[1].text.empty());
    assert(lines[2].text.empty());
    assert(lineForCursor(lines, 5) == 2);
  }
  {
    const auto lines = wrapWords("", 39);
    assert(lines.size() == 1);
    assert(lines[0].text.empty());
  }
  {
    const auto lines = wrapWords("abc", 0);
    assert(lines.size() == 3); // Defensive protection from zero-sized viewport.
  }
  {
    // Long multi-byte word: if forced to wrap, never cut between UTF-8 bytes.
    const std::string text = "abcd\xC3\xA9\xC3\xA9" "abcdef";
    const auto lines = wrapWords(text, 5);
    assert(lines.size() >= 2);
    assert(lines[0].text == "abcd");
    assert(lines[1].text.substr(0,2) == "\xC3\xA9");
  }
}
