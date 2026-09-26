#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "../../src/targets/heltec_e213/TextPager.h"

// Arduino File is unavailable in CI's host test, so model its cursor API.
class MemoryFile {
 public:
  explicit MemoryFile(std::string bytes) : content(std::move(bytes)) {}
  bool seek(uint32_t offset) {
    if (offset > content.size()) return false;
    cursor = offset;
    return true;
  }
  int read() { return available() ? static_cast<unsigned char>(content[cursor++]) : -1; }
  int available() const { return static_cast<int>(content.size() - cursor); }
  uint32_t position() const { return static_cast<uint32_t>(cursor); }

 private:
  std::string content;
  size_t cursor = 0;
};

int main() {
  using namespace e213;
  {
    MemoryFile f("Hello\r\n\r\nworld");
    Page p;
    assert(loadPage(f, 0, p));
    assert(p.lineCount == 3 && std::string(p.lines[0]) == "Hello" && std::string(p.lines[1]).empty());
    assert(std::string(p.lines[2]) == "world" && p.eof);
  }
  {
    const std::string longText(kColumns * (kLines + 1), 'x');
    MemoryFile f(longText);
    Page p, next;
    assert(loadPage(f, 0, p) && p.lineCount == kLines && !p.eof);
    assert(p.nextOffset == kColumns * kLines);
    assert(loadPage(f, p.nextOffset, next) && next.lineCount == 1 && next.eof);
  }
  {
    MemoryFile f(std::string(kColumns, 'a') + "\r\nb");
    Page p;
    assert(loadPage(f, 0, p));
    assert(p.lineCount == 2 && std::string(p.lines[1]) == "b");
  }
  {
    MemoryFile f(std::string("\xEF\xBB\xBF") + "smart \xE2\x80\x9Cquotes\xE2\x80\x9D");
    Page p;
    assert(loadPage(f, 0, p));
    assert(std::string(p.lines[0]) == "smart \"quotes\"");
    assert(p.nextOffset == 21 && p.eof);
  }
  {
    MemoryFile f("ab");
    Page p;
    assert(!loadPage(f, 9, p));
  }
  std::cout << "E213 pager tests passed\n";
}
