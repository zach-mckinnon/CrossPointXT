#include "../../src/targets/heltec_e213_writer/WriterStats.h"
#include <cassert>
#include <string>

int main() {
  using writerstats::countWords;
  assert(countWords("") == 0);
  assert(countWords("   \n\t") == 0);
  assert(countWords("Hello, world!") == 2);
  assert(countWords("One  two\nthree\tfour") == 4);
  assert(countWords("Don't split compound-words accidentally") == 4);
  assert(countWords("... ** ---") == 0);
  assert(countWords("She *whispered* the **truth**.") == 4);
  assert(countWords("caf\xC3\xA9 cr\xC3\xA8me") == 2);
  assert(countWords("one\n\n two\r\nthree") == 3);

  // 250-pixel-wide e-ink display with GFX 6 px / character.
  assert(writerstats::rightAlignedX(250, "12 words") == 196);
  assert(writerstats::centeredX(250, "BT:OK") == 110);
  assert(writerstats::rightAlignedX(250, "SAVE ERR") == 196);
  assert(writerstats::titleColumns(196) == 30);
  assert(writerstats::centeredX(0, "BT:OK") == 0);
  assert(writerstats::rightAlignedX(10, "TOO LONG") == 0);
}
