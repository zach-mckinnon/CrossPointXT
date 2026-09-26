#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <SD.h>
#include <SPI.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "TextPager.h"

// First E213 port milestone: a small, isolated TXT reader. This source is
// selected only by [env:heltec_e213]; the existing X3/X4 build is unchanged.
// Later milestones connect the HAL and CrossPointXT's EPUB/editor activities.
namespace {
constexpr uint8_t kDisplayPower = 18;  // E213 V1.1: active HIGH.
constexpr uint8_t kDisplaySck = 4;
constexpr uint8_t kDisplayMosi = 6;
constexpr uint8_t kDisplayCs = 5;
constexpr uint8_t kDisplayDc = 2;
constexpr uint8_t kDisplayReset = 3;
constexpr uint8_t kDisplayBusy = 1;
constexpr uint8_t kNextButton = 21;   // USER button, active LOW.
constexpr uint8_t kSelectButton = 0;  // BOOT button; never hold during reset.

// User's proposed SD module orientation. Verify labels and 3V3/GND before use.
constexpr uint8_t kSdCs = 38;
constexpr uint8_t kSdMosi = 39;
constexpr uint8_t kSdSck = 40;
constexpr uint8_t kSdMiso = 41;
constexpr uint32_t kSdClockHz = 4'000'000;
constexpr uint8_t kMaxBooks = 40;
constexpr uint8_t kBooksPerScreen = 8;
constexpr char kProgressPath[] = "/.e213-state.txt";
constexpr char kProgressTemp[] = "/.e213-state.tmp";

GxEPD2_BW<GxEPD2_213_E0213A367, GxEPD2_213_E0213A367::HEIGHT> display(
    GxEPD2_213_E0213A367(kDisplayCs, kDisplayDc, kDisplayReset, kDisplayBusy));
SPIClass sdBus(HSPI);  // Independent bus: e-ink stays on global SPI.

struct Book {
  String path;
  String title;
};

enum class Screen { StorageError, Library, Reading };
enum class ButtonEvent { None, Short, Long };

struct Button {
  const uint8_t pin;
  bool raw = false;
  bool stable = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;

  ButtonEvent poll() {
    const bool pressed = digitalRead(pin) == LOW;
    const uint32_t now = millis();
    if (pressed != raw) {
      raw = pressed;
      changedAt = now;
    }
    if (raw == stable || now - changedAt < 35) return ButtonEvent::None;
    stable = raw;
    if (stable) {
      pressedAt = now;
      return ButtonEvent::None;
    }
    return now - pressedAt >= 650 ? ButtonEvent::Long : ButtonEvent::Short;
  }
};

Button nextButton{kNextButton};
Button selectButton{kSelectButton};
std::vector<Book> books;
std::vector<uint32_t> pageStarts;
File bookFile;
e213::Page visiblePage;
Screen screen = Screen::StorageError;
size_t selectedBook = 0;
size_t pageIndex = 0;
String currentPath;
String currentTitle;
uint32_t currentSize = 0;
uint16_t screenUpdates = 0;

String cropped(const String& str, size_t limit) {
  return str.length() <= limit ? str : str.substring(0, limit - 1) + "~";
}

void refreshScreen(bool full = false) {
  // A periodic full refresh removes e-ink ghosting from repeated page turns.
  if (full || (screenUpdates++ % 10 == 0)) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);  // GFX built-in font, 6 x 8px.
    if (screen == Screen::StorageError) {
      display.setCursor(8, 9);
      display.println("SD CARD NOT FOUND");
      display.setCursor(8, 29);
      display.println("Check FAT32 + wiring:");
      display.setCursor(8, 42);
      display.println("CS38 MOSI39 SCK40 MISO41");
      display.setCursor(8, 55);
      display.println("3V3 and GND separately");
      display.setCursor(8, 100);
      display.println("Press USER to retry");
    } else if (screen == Screen::Library) {
      display.setCursor(6, 3);
      display.print("E213 LIBRARY  (");
      display.print(books.size());
      display.println(")");
      display.drawFastHLine(0, 14, display.width(), GxEPD_BLACK);
      if (books.empty()) {
        display.setCursor(6, 27);
        display.println("No TXT books found.");
        display.setCursor(6, 41);
        display.println("Put *.txt in /books on SD.");
      } else {
        const size_t start = selectedBook / kBooksPerScreen * kBooksPerScreen;
        for (size_t i = 0; i < kBooksPerScreen && i + start < books.size(); ++i) {
          const size_t id = start + i;
          display.setCursor(6, 20 + static_cast<int>(i) * 11);
          display.print(id == selectedBook ? '>' : ' ');
          display.print(cropped(books[id].title, 37));
        }
      }
      display.drawFastHLine(0, 110, display.width(), GxEPD_BLACK);
      display.setCursor(6, 113);
      display.print("USER:next BOOT:open");
    } else {
      display.setCursor(6, 3);
      display.print(cropped(currentTitle, 29));
      display.drawFastHLine(0, 14, display.width(), GxEPD_BLACK);
      for (uint8_t i = 0; i < visiblePage.lineCount; ++i) {
        display.setCursor(8, 20 + i * 9);
        display.print(visiblePage.lines[i]);
      }
      display.drawFastHLine(0, 111, display.width(), GxEPD_BLACK);
      display.setCursor(6, 114);
      display.print("<0   ");
      display.print(pageIndex + 1);
      display.print(visiblePage.eof ? "  END" : "   21>");
      display.print("  HOLD 21:BOOKS");
    }
  } while (display.nextPage());
}

bool loadLibrary() {
  books.clear();
  selectedBook = 0;
  if (!SD.exists("/books") && !SD.mkdir("/books")) return false;
  File dir = SD.open("/books");
  if (!dir || !dir.isDirectory()) return false;
  File entry = dir.openNextFile();
  while (entry && books.size() < kMaxBooks) {
    if (!entry.isDirectory()) {
      String path = entry.name();
      const int lastSlash = path.lastIndexOf('/');
      String filename = lastSlash < 0 ? path : path.substring(lastSlash + 1);
      String suffix = filename;
      suffix.toLowerCase();
      if (suffix.endsWith(".txt")) {
        books.push_back({String("/books/") + filename, filename});
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  std::sort(books.begin(), books.end(), [](const Book& a, const Book& b) {
    String x = a.title;
    String y = b.title;
    x.toLowerCase();
    y.toLowerCase();
    return x.compareTo(y) < 0;
  });
  return true;
}

void saveProgress() {
  if (!bookFile || pageStarts.empty()) return;
  // Write to a temporary file first. If power drops during a save, the old
  // state is still present; an orphaned temp file can be recovered next boot.
  SD.remove(kProgressTemp);
  File state = SD.open(kProgressTemp, FILE_WRITE);
  if (!state) {
    Serial.println("Could not write progress temp file");
    return;
  }
  state.println(currentPath);
  state.println(currentSize);
  state.println(pageStarts[pageIndex]);
  state.flush();
  state.close();
  SD.remove(kProgressPath);
  if (!SD.rename(kProgressTemp, kProgressPath)) {
    Serial.println("Could not finalize progress; temp file retained");
  }
}

void restoreProgress() {
  const char* source = SD.exists(kProgressPath) ? kProgressPath : kProgressTemp;
  File state = SD.open(source, FILE_READ);
  if (!state) return;
  String savedPath = state.readStringUntil('\n');
  savedPath.trim();
  const uint32_t savedSize = static_cast<uint32_t>(state.parseInt());
  const uint32_t savedOffset = static_cast<uint32_t>(state.parseInt());
  state.close();
  if (savedPath != currentPath || savedSize != currentSize || savedOffset >= currentSize) return;

  // Recreate earlier page boundaries so Back works after resuming. We keep
  // only offsets in RAM, not all book text or rendered framebuffers.
  while (pageStarts.back() < savedOffset && pageStarts.size() < 100000) {
    e213::Page scratch;
    if (!e213::loadPage(bookFile, pageStarts.back(), scratch) || scratch.eof ||
        scratch.nextOffset <= pageStarts.back() || scratch.nextOffset > savedOffset) break;
    pageStarts.push_back(scratch.nextOffset);
  }
  if (pageStarts.back() == savedOffset) pageIndex = pageStarts.size() - 1;
}

void showPage() {
  if (!bookFile || pageStarts.empty()) return;
  if (!e213::loadPage(bookFile, pageStarts[pageIndex], visiblePage)) {
    Serial.println("Failed to seek/read book");
    screen = Screen::Library;
    refreshScreen(true);
    return;
  }
  screen = Screen::Reading;
  refreshScreen();
}

void openBook(size_t index) {
  if (index >= books.size()) return;
  bookFile.close();
  bookFile = SD.open(books[index].path, FILE_READ);
  if (!bookFile) {
    Serial.println("Cannot open selected book");
    screen = Screen::Library;
    refreshScreen(true);
    return;
  }
  currentPath = books[index].path;
  currentTitle = books[index].title;
  currentSize = bookFile.size();
  pageStarts.assign(1, 0);
  pageIndex = 0;
  restoreProgress();
  showPage();
  saveProgress();
}

void enterLibrary() {
  if (bookFile) saveProgress();
  bookFile.close();
  currentPath = "";
  if (!loadLibrary()) {
    screen = Screen::StorageError;
  } else {
    screen = Screen::Library;
  }
  refreshScreen(true);
}

void nextPage() {
  if (!bookFile || visiblePage.eof || visiblePage.nextOffset <= pageStarts[pageIndex]) return;
  if (pageIndex + 1 == pageStarts.size()) pageStarts.push_back(visiblePage.nextOffset);
  ++pageIndex;
  showPage();
  saveProgress();
}

void previousPage() {
  if (!bookFile || pageIndex == 0) return;
  --pageIndex;
  showPage();
  saveProgress();
}

void mountCard() {
  pinMode(kSdCs, OUTPUT);
  digitalWrite(kSdCs, HIGH);
  sdBus.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  if (!SD.begin(kSdCs, sdBus, kSdClockHz)) {
    Serial.println("SD init failed: check 3V3, GND, CS38/MOSI39/SCK40/MISO41");
    screen = Screen::StorageError;
    refreshScreen(true);
    return;
  }
  Serial.printf("SD ready: %llu MB\n", static_cast<unsigned long long>(SD.cardSize() / 1048576ULL));
  enterLibrary();
}

void onButton(uint8_t pin, ButtonEvent event) {
  if (event == ButtonEvent::None) return;
  if (screen == Screen::StorageError) {
    if (pin == kNextButton) mountCard();
  } else if (screen == Screen::Library) {
    if (pin == kNextButton && !books.empty()) {
      selectedBook = (selectedBook + 1) % books.size();
      refreshScreen();
    } else if (pin == kSelectButton && event == ButtonEvent::Long) {
      enterLibrary();  // Rescan after updating the microSD card.
    } else if (pin == kSelectButton && !books.empty()) {
      openBook(selectedBook);
    }
  } else if (pin == kNextButton && event == ButtonEvent::Long) {
    enterLibrary();
  } else if (pin == kNextButton) {
    nextPage();
  } else if (pin == kSelectButton) {
    previousPage();
  }
}
}  // namespace

void setup() {
  pinMode(kNextButton, INPUT_PULLUP);
  pinMode(kSelectButton, INPUT_PULLUP);
  pinMode(kDisplayPower, OUTPUT);
  digitalWrite(kDisplayPower, HIGH);
  Serial.begin(115200);
  delay(250);
  Serial.printf("CrossPointXT E213 bring-up: flash=%lu, psram=%lu\n",
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getPsramSize()));
  SPI.begin(kDisplaySck, -1, kDisplayMosi, kDisplayCs);
  display.init(115200);
  display.setRotation(1);
  Serial.printf("E-ink: %dx%d\n", display.width(), display.height());
  mountCard();
}

void loop() {
  onButton(kNextButton, nextButton.poll());
  onButton(kSelectButton, selectButton.poll());
  delay(10);
}
