#include "../../src/targets/heltec_e213_writer/WriterScreenGeometry.h"
#include <cassert>

// Fixed size of the physically verified E213 FC1 panel, landscape 250x122.
// GFX's default 6x8 font anchors setCursor at the TOP, not the baseline.
int main() {
  using namespace writergeometry;
  static_assert(kGlyphWidth == 6 && kGlyphHeight == 8);
  static_assert(kHeaderTop == 4 && kHeaderDivider == 14);
  static_assert(footerDivider(122) == 108);
  static_assert(footerTop(122) == 112);
  static_assert(kHeaderTop + kGlyphHeight - 1 < kHeaderDivider);
  static_assert(footerTop(122) > footerDivider(122));
  static_assert(footerTop(122) + kGlyphHeight <= 122);
  static_assert(bodyRowTop(0, false) == 20);
  static_assert(bodyRowTop(8, false) == 92);
  static_assert(caretBottom(bodyRowTop(8, false)) == 99);
  static_assert(caretBottom(bodyRowTop(8, false)) < footerDivider(122));
  static_assert(bodyRowTop(0, true) == 4);
  static_assert(bodyRowTop(12, true) == 112);
  static_assert(caretBottom(bodyRowTop(12, true)) == 119);
  static_assert(fileRowTop(7) == 97);
  static_assert(fileRowTop(7) + kGlyphHeight - 1 < footerDivider(122));
  static_assert(fitsNormal(122) && fitsFocus(122));
  assert(!fitsNormal(90));
  assert(!fitsFocus(110));
}
