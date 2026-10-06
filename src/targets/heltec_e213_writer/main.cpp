#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <LittleFS.h>
#include <SPI.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BleWriterKeyboard.h"

// E213 writer bring-up:
// - no microSD required
// - BLE HID keyboard input
// - one scratch document stored in internal flash
// - e-ink partial refresh with a typing-friendly debounce
namespace {
constexpr uint8_t kDisplayPower = 18;  // E213 V1.1: active HIGH.
constexpr uint8_t kDisplaySck = 4;
constexpr uint8_t kDisplayMosi = 6;
constexpr uint8_t kDisplayCs = 5;
constexpr uint8_t kDisplayDc = 2;
constexpr uint8_t kDisplayReset = 3;
constexpr uint8_t kDisplayBusy = 1;
constexpr uint8_t kNextButton = 21;   // USER, active LOW.
constexpr uint8_t kSelectButton = 0;  // BOOT; do not hold during reset.

constexpr char kDraftPath[] = "/draft.txt";
constexpr char kDraftTempPath[] = "/draft.tmp";
constexpr size_t kMaxDraftBytes = 64 * 1024;

constexpr int kTextLeft = 6;
constexpr int kTextTop = 20;
constexpr int kTextBaselineStep = 9;
constexpr int kCharactersPerLine = 39;
constexpr int kVisibleRows = 9;

constexpr unsigned long kRenderIdleDelayMs = 220;
constexpr unsigned long kRenderMaxDelayMs = 900;
constexpr unsigned long kAutosaveDelayMs = 1800;
constexpr unsigned long kPairingRefreshMs = 1000;

GxEPD2_BW<GxEPD2_213_E0213A367, GxEPD2_213_E0213A367::HEIGHT> display(
    GxEPD2_213_E0213A367(kDisplayCs, kDisplayDc, kDisplayReset, kDisplayBusy, SPI));

BleWriterKeyboard keyboard;

enum class ButtonEvent { None, Short, Long };

struct Button {
  uint8_t pin;
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

    return now - pressedAt >= 700 ? ButtonEvent::Long : ButtonEvent::Short;
  }
};

struct VisualLine {
  size_t start = 0;
  size_t end = 0;
  std::string text;
};

Button nextButton{kNextButton};
Button selectButton{kSelectButton};

std::string document;
size_t cursor = 0;
bool storageReady = false;
bool dirty = false;
bool renderPending = true;
uint32_t screenRefreshCount = 0;
uint32_t lastKeyboardStatusVersion = 0;
unsigned long lastEditAt = 0;
unsigned long lastKeyAt = 0;
unsigned long lastRenderAt = 0;
unsigned long lastPairingRenderAt = 0;

std::string cropped(const std::string& value, const size_t maxChars) {
  if (value.size() <= maxChars) return value;
  if (maxChars < 2) return value.substr(0, maxChars);
  return value.substr(0, maxChars - 1) + "~";
}

void setRefreshWindow(const bool forceFull = false) {
  if (forceFull || screenRefreshCount++ % 18 == 0) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }
}

std::vector<VisualLine> buildVisualLines() {
  std::vector<VisualLine> lines;

  if (document.empty()) {
    lines.push_back({0, 0, ""});
    return lines;
  }

  size_t offset = 0;
  while (offset <= document.size()) {
    const size_t lineStart = offset;
    size_t count = 0;
    std::string text;

    while (offset < document.size() && document[offset] != '\n' && count < kCharactersPerLine) {
      char c = document[offset];
      if (c == '\r') {
        ++offset;
        continue;
      }
      text.push_back(c);
      ++offset;
      ++count;
    }

    lines.push_back({lineStart, offset, text});

    if (offset >= document.size()) break;

    if (document[offset] == '\n') {
      ++offset;
      if (offset == document.size()) {
        lines.push_back({offset, offset, ""});
        break;
      }
      continue;
    }

    // If the line filled up, keep offset unchanged so the next visual row
    // starts at exactly the next character.
  }

  if (lines.empty()) lines.push_back({0, 0, ""});
  return lines;
}

size_t cursorVisualLine(const std::vector<VisualLine>& lines) {
  for (size_t i = 0; i < lines.size(); ++i) {
    const bool finalLine = i + 1 == lines.size();
    if (cursor >= lines[i].start && (cursor < lines[i].end || (cursor == lines[i].end && finalLine))) {
      return i;
    }

    // Cursor can sit on a newline byte; visually that belongs at the end of
    // the line immediately before the newline.
    if (cursor == lines[i].end && cursor < document.size() && document[cursor] == '\n') {
      return i;
    }
  }
  return lines.size() - 1;
}

int cursorColumn(const VisualLine& line) {
  if (cursor <= line.start) return 0;
  return static_cast<int>(std::min(cursor - line.start, static_cast<size_t>(kCharactersPerLine)));
}

void renderPairing(const bool forceFull = false) {
  setRefreshWindow(forceFull);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);

    display.setCursor(6, 9);
    display.println("E213 MICRO WRITER");
    display.drawFastHLine(0, 14, display.width(), GxEPD_BLACK);

    display.setCursor(6, 27);
    display.println(keyboard.statusText());

    const int count = keyboard.candidateCount();
    if (count == 0) {
      display.setCursor(6, 45);
      display.println("1. Turn keyboard on");
      display.setCursor(6, 58);
      display.println("2. Put it in pairing mode");
      display.setCursor(6, 71);
      display.println("3. Wait for it to appear");
      display.setCursor(6, 89);
      display.println("USER: scan again");
    } else {
      const int selected = keyboard.selectedCandidateIndex();
      const int first = std::max(0, selected - 2);
      for (int row = 0; row < 5 && first + row < count; ++row) {
        BleWriterKeyboard::Candidate candidate;
        if (!keyboard.getCandidate(first + row, candidate)) continue;
        display.setCursor(6, 43 + row * 12);
        display.print(first + row == selected ? "> " : "  ");
        display.print(cropped(candidate.name, 30).c_str());
      }
    }

    display.drawFastHLine(0, 110, display.width(), GxEPD_BLACK);
    display.setCursor(6, 119);
    if (keyboard.isConnecting()) {
      display.print("Connecting...");
    } else if (count > 0) {
      display.print("USER next | BOOT connect");
    } else {
      display.print("BLE keyboards only");
    }
  } while (display.nextPage());

  lastRenderAt = millis();
  lastPairingRenderAt = lastRenderAt;
}

void renderWriter(const bool forceFull = false) {
  const std::vector<VisualLine> lines = buildVisualLines();
  const size_t activeLine = cursorVisualLine(lines);
  const size_t firstLine = activeLine >= static_cast<size_t>(kVisibleRows - 1)
                               ? activeLine - static_cast<size_t>(kVisibleRows - 1)
                               : 0;

  setRefreshWindow(forceFull);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);

    display.setCursor(6, 9);
    display.print("DRAFT");
    display.setCursor(76, 9);
    display.print(keyboard.isConnected() ? "BT:OK" : "BT:--");
    display.setCursor(146, 9);
    display.print(document.size());
    display.print(" bytes");
    display.drawFastHLine(0, 14, display.width(), GxEPD_BLACK);

    for (int row = 0; row < kVisibleRows; ++row) {
      const size_t index = firstLine + static_cast<size_t>(row);
      if (index >= lines.size()) break;
      const int baseline = kTextTop + row * kTextBaselineStep;
      display.setCursor(kTextLeft, baseline);
      display.print(lines[index].text.c_str());

      if (index == activeLine) {
        const int column = std::min(cursorColumn(lines[index]), kCharactersPerLine);
        const int x = std::min(display.width() - 2, kTextLeft + column * 6);
        display.drawFastVLine(x, baseline - 7, 8, GxEPD_BLACK);
      }
    }

    display.drawFastHLine(0, 110, display.width(), GxEPD_BLACK);
    display.setCursor(6, 119);
    if (!keyboard.isConnected()) {
      display.print("Keyboard disconnected");
    } else if (!storageReady) {
      display.print("RAM ONLY - flash unavailable");
    } else if (dirty) {
      display.print("Editing... autosave pending");
    } else {
      display.print("Saved internally");
    }
  } while (display.nextPage());

  lastRenderAt = millis();
  renderPending = false;
}

bool loadDraft() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed; writer will be RAM-only");
    return false;
  }

  if (!LittleFS.exists(kDraftPath)) return true;

  File file = LittleFS.open(kDraftPath, FILE_READ);
  if (!file) return false;

  document.clear();
  document.reserve(std::min(static_cast<size_t>(file.size()), kMaxDraftBytes));
  while (file.available() && document.size() < kMaxDraftBytes) {
    document.push_back(static_cast<char>(file.read()));
  }
  file.close();
  cursor = document.size();
  return true;
}

bool saveDraft() {
  if (!storageReady) return false;

  LittleFS.remove(kDraftTempPath);
  File file = LittleFS.open(kDraftTempPath, FILE_WRITE);
  if (!file) return false;

  const size_t written = file.write(reinterpret_cast<const uint8_t*>(document.data()), document.size());
  file.flush();
  file.close();

  if (written != document.size()) {
    LittleFS.remove(kDraftTempPath);
    return false;
  }

  LittleFS.remove(kDraftPath);
  if (!LittleFS.rename(kDraftTempPath, kDraftPath)) return false;

  dirty = false;
  return true;
}

void markEdited() {
  dirty = true;
  renderPending = true;
  lastEditAt = millis();
  lastKeyAt = lastEditAt;
}

void insertCharacter(const char c) {
  if (document.size() >= kMaxDraftBytes) return;
  document.insert(document.begin() + static_cast<std::string::difference_type>(cursor), c);
  ++cursor;
  markEdited();
}

void insertText(const char* text) {
  if (!text) return;
  while (*text && document.size() < kMaxDraftBytes) {
    insertCharacter(*text++);
  }
}

void backspace() {
  if (cursor == 0 || document.empty()) return;
  document.erase(cursor - 1, 1);
  --cursor;
  markEdited();
}

void deleteForward() {
  if (cursor >= document.size()) return;
  document.erase(cursor, 1);
  markEdited();
}

void moveCursorLeft() {
  if (cursor > 0) {
    --cursor;
    renderPending = true;
    lastKeyAt = millis();
  }
}

void moveCursorRight() {
  if (cursor < document.size()) {
    ++cursor;
    renderPending = true;
    lastKeyAt = millis();
  }
}

void moveCursorVertical(const int direction) {
  const std::vector<VisualLine> lines = buildVisualLines();
  if (lines.empty()) return;

  const size_t current = cursorVisualLine(lines);
  const int target = static_cast<int>(current) + direction;
  if (target < 0 || target >= static_cast<int>(lines.size())) return;

  const int column = cursorColumn(lines[current]);
  const VisualLine& destination = lines[static_cast<size_t>(target)];
  cursor = destination.start + std::min(static_cast<size_t>(column), destination.end - destination.start);
  renderPending = true;
  lastKeyAt = millis();
}

void handleKeyboardEvents() {
  BleWriterKeyboard::KeyEvent event;
  while (keyboard.popEvent(event)) {
    switch (event.type) {
      case BleWriterKeyboard::KeyType::Character:
        insertCharacter(event.character);
        break;
      case BleWriterKeyboard::KeyType::Enter:
        insertCharacter('\n');
        break;
      case BleWriterKeyboard::KeyType::Backspace:
        backspace();
        break;
      case BleWriterKeyboard::KeyType::DeleteKey:
        deleteForward();
        break;
      case BleWriterKeyboard::KeyType::Tab:
        insertText("    ");
        break;
      case BleWriterKeyboard::KeyType::Left:
        moveCursorLeft();
        break;
      case BleWriterKeyboard::KeyType::Right:
        moveCursorRight();
        break;
      case BleWriterKeyboard::KeyType::Up:
        moveCursorVertical(-1);
        break;
      case BleWriterKeyboard::KeyType::Down:
        moveCursorVertical(1);
        break;
      case BleWriterKeyboard::KeyType::Escape:
        break;
    }
  }
}

void handlePairingButtons() {
  const ButtonEvent next = nextButton.poll();
  const ButtonEvent select = selectButton.poll();

  if (keyboard.isConnected()) return;

  if (next != ButtonEvent::None) {
    if (keyboard.candidateCount() > 0 && next == ButtonEvent::Short) {
      keyboard.selectNextCandidate();
    } else {
      keyboard.requestScan();
    }
    renderPairing();
  }

  if (select == ButtonEvent::Short && keyboard.candidateCount() > 0) {
    renderPairing();
    keyboard.connectSelectedCandidate();
    if (keyboard.isConnected()) {
      renderWriter(true);
    } else {
      renderPairing(true);
    }
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
  Serial.printf("E213 writer bring-up: flash=%lu psram=%lu\n",
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getPsramSize()));

  SPI.begin(kDisplaySck, -1, kDisplayMosi, kDisplayCs);
  display.init(115200);
  display.setRotation(1);
  Serial.printf("E-ink: %dx%d\n", display.width(), display.height());

  storageReady = loadDraft();
  Serial.printf("Internal draft storage: %s (%u bytes loaded)\n",
                storageReady ? "ready" : "unavailable",
                static_cast<unsigned>(document.size()));

  keyboard.begin();
  lastKeyboardStatusVersion = keyboard.statusVersion();
  renderPairing(true);
}

void loop() {
  keyboard.update();
  handlePairingButtons();

  const bool connected = keyboard.isConnected();
  if (connected) {
    handleKeyboardEvents();

    if (dirty && storageReady && millis() - lastEditAt >= kAutosaveDelayMs) {
      if (saveDraft()) {
        Serial.printf("Draft autosaved (%u bytes)\n", static_cast<unsigned>(document.size()));
        renderPending = true;
      } else {
        Serial.println("Draft autosave failed");
      }
    }

    const unsigned long now = millis();
    if (renderPending &&
        ((now - lastKeyAt >= kRenderIdleDelayMs) || (now - lastRenderAt >= kRenderMaxDelayMs))) {
      renderWriter();
    }
  }

  const uint32_t statusVersion = keyboard.statusVersion();
  if (statusVersion != lastKeyboardStatusVersion) {
    lastKeyboardStatusVersion = statusVersion;
    if (!keyboard.isConnected() && millis() - lastPairingRenderAt >= kPairingRefreshMs) {
      renderPairing();
    } else if (keyboard.isConnected()) {
      renderPending = true;
    }
  }

  delay(5);
}
