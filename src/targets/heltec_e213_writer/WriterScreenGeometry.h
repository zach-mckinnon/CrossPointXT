#pragma once
// E213 250x122 built-in GFX bitmap font uses TOP-LEFT text-cell coordinates,
// never baseline coordinates. Keep every screen on this shared geometry.
namespace writergeometry {
constexpr int kGlyphWidth = 6;
constexpr int kGlyphHeight = 8;
constexpr int kLeft = 6;
constexpr int kHeaderTop = 4;
constexpr int kHeaderDivider = 14;
constexpr int kFooterHeight = 14;
constexpr int kBodyTop = 20;
constexpr int kBodyStep = 9;
constexpr int kNormalRows = 9;
constexpr int kFocusTop = 4;
constexpr int kFocusRows = 13;
constexpr int kFileListTop = 20;
constexpr int kFileListStep = 11;
constexpr int kFileListRows = 8;

constexpr int footerDivider(int height) { return height - kFooterHeight; }
constexpr int footerTop(int height) { return height - kGlyphHeight - 2; }
constexpr int bodyRowTop(int row, bool focus) {
  return (focus ? kFocusTop : kBodyTop) + row * kBodyStep;
}
constexpr int caretBottom(int rowTop) { return rowTop + kGlyphHeight - 1; }
constexpr int fileRowTop(int row) { return kFileListTop + row * kFileListStep; }
constexpr bool fitsNormal(int height) {
  return kHeaderTop + kGlyphHeight <= kHeaderDivider &&
         bodyRowTop(kNormalRows - 1, false) + kGlyphHeight < footerDivider(height) &&
         footerDivider(height) < footerTop(height) &&
         footerTop(height) + kGlyphHeight <= height &&
         fileRowTop(kFileListRows - 1) + kGlyphHeight < footerDivider(height);
}
constexpr bool fitsFocus(int height) {
  return bodyRowTop(kFocusRows - 1, true) + kGlyphHeight <= height;
}
static_assert(fitsNormal(122), "Header/body/footer overlap on E213");
static_assert(fitsFocus(122), "Focus view clips its last row on E213");
}
