#include <Arduino.h>
#include <heltec-eink-modules.h>
#include <LittleFS.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BleWriterKeyboard.h"
#include "WriterFiles.h"
#include "WriterSyncSerial.h"

// E213 writer bring-up:
// - no microSD required
// - BLE HID keyboard input
// - recoverable multi-document library stored in internal flash
// - e-ink partial refresh with a typing-friendly debounce
namespace {
constexpr uint8_t kNextButton = 21;   // USER, active LOW.
constexpr uint8_t kSelectButton = 0;  // BOOT; do not hold during reset.

constexpr size_t kMaxDraftBytes = writerfiles::kMaxDocumentBytes;

constexpr int kTextLeft = 6;
constexpr int kTextTop = 20;
constexpr int kTextBaselineStep = 9;
constexpr int kCharactersPerLine = 39;
constexpr int kVisibleRows = 9;

constexpr unsigned long kRenderIdleDelayMs = 220;
constexpr unsigned long kRenderMaxDelayMs = 900;
constexpr unsigned long kAutosaveDelayMs = 1800;
constexpr unsigned long kPairingRefreshMs = 1000;

// Vision Master E213 V1.1 uses the E0213A367-BW panel. This board-specific
// driver also owns the Vext power sequencing required by the integrated panel.
EInkDisplay_VisionMasterE213V1_1 display;

BleWriterKeyboard keyboard;
WriterSyncSerial serialSync;

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
std::string activeName = "Draft-001.txt";
std::vector<std::string> fileList;
size_t fileIndex = 0;
bool fileMenu = false;
size_t cursor = 0;
bool storageReady = false;
bool dirty = false;
bool renderPending = true;
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

void prepareDisplayFrame() {
  display.fullscreen();
  display.clearMemory();
  display.setTextColor(BLACK);
  display.setTextSize(1);
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
  (void)forceFull;
  prepareDisplayFrame();

    display.setCursor(6, 9);
    display.println("E213 MICRO WRITER");
    display.drawLine(0, 14, display.width() - 1, 14, BLACK);

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

    display.drawLine(0, 110, display.width() - 1, 110, BLACK);
    display.setCursor(6, 119);
    if (keyboard.isConnecting()) {
      display.print("Connecting...");
    } else if (count > 0) {
      display.print("USER next | BOOT connect");
    } else {
      display.print("BLE keyboards only");
    }
  display.update();

  lastRenderAt = millis();
  lastPairingRenderAt = lastRenderAt;
}

void renderFilePicker() {
  prepareDisplayFrame();
  display.setCursor(6, 9);
  display.print("FILES  ENTER:OPEN  ESC:BACK");
  display.drawLine(0, 14, display.width() - 1, 14, BLACK);
  const size_t first = fileIndex >= 6 ? fileIndex - 6 : 0;
  for (size_t row = 0; row < 8 && first + row < fileList.size(); ++row) {
    const size_t i = first + row;
    display.setCursor(6, 25 + static_cast<int>(row) * 11);
    display.print(i == fileIndex ? "> " : "  ");
    display.print(cropped(fileList[i], 34).c_str());
  }
  display.drawLine(0, 110, display.width() - 1, 110, BLACK);
  display.setCursor(6, 119);
  display.print("Ctrl+N: NEW   Ctrl+O: TOGGLE");
  display.update();
  renderPending = false;
  lastRenderAt = millis();
}

void renderWriter(const bool forceFull = false) {
  if (fileMenu) { renderFilePicker(); return; }
  const std::vector<VisualLine> lines = buildVisualLines();
  const size_t activeLine = cursorVisualLine(lines);
  const size_t firstLine = activeLine >= static_cast<size_t>(kVisibleRows - 1)
                               ? activeLine - static_cast<size_t>(kVisibleRows - 1)
                               : 0;

  (void)forceFull;
  prepareDisplayFrame();

    display.setCursor(6, 9);
    display.print(cropped(activeName, 18).c_str());
    display.setCursor(76, 9);
    display.print(keyboard.isConnected() ? "BT:OK" : "BT:--");
    display.setCursor(146, 9);
    display.print(document.size());
    display.print(" bytes");
    display.drawLine(0, 14, display.width() - 1, 14, BLACK);

    for (int row = 0; row < kVisibleRows; ++row) {
      const size_t index = firstLine + static_cast<size_t>(row);
      if (index >= lines.size()) break;
      const int baseline = kTextTop + row * kTextBaselineStep;
      display.setCursor(kTextLeft, baseline);
      display.print(lines[index].text.c_str());

      if (index == activeLine) {
        const int column = std::min(cursorColumn(lines[index]), kCharactersPerLine);
        const int x = std::min(display.width() - 2, kTextLeft + column * 6);
        display.drawLine(x, baseline - 7, x, baseline, BLACK);
      }
    }

    display.drawLine(0, 110, display.width() - 1, 110, BLACK);
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
  display.update();

  lastRenderAt = millis();
  renderPending = false;
}

bool loadDraft() {
  // Never auto-format on a mount failure: that could erase the user's drafts.
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed; writer will be RAM-only");
    return false;
  }
  if (!LittleFS.exists("/drafts")) LittleFS.mkdir("/drafts");
  fileList = writerfiles::list();
  if (fileList.empty()) {
    // Migrate non-destructively. Keep /draft.txt as a recovery copy.
    std::string legacy;
    if (LittleFS.exists("/draft.txt")) {
      File old = LittleFS.open("/draft.txt", "r");
      if (old && !old.isDirectory() && old.size() <= kMaxDraftBytes) {
        while (old.available()) legacy.push_back(static_cast<char>(old.read()));
      }
      old.close();
    }
    if (!writerfiles::save(activeName, legacy)) return false;
    fileList = writerfiles::list();
  }
  if (fileList.empty()) return false;
  activeName = fileList.front();
  return writerfiles::read(activeName, document);
}

bool saveDraft() {
  if (!storageReady) return false;
  if (!writerfiles::save(activeName, document)) return false;
  dirty = false;
  return true;
}

bool switchDraft(const std::string& name) {
  if (dirty && !saveDraft()) return false; // Never switch away from unsaved work.
  std::string next;
  if (!writerfiles::read(name, next)) return false;
  activeName = name;
  document.swap(next);
  cursor = document.size();
  fileMenu = false;
  renderPending = true;
  lastKeyAt = millis();
  return true;
}

bool createDraft() {
  if (!storageReady || (dirty && !saveDraft())) return false;
  const std::string name = writerfiles::nextName();
  if (name.empty() || !writerfiles::save(name, "")) return false;
  return switchDraft(name);
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
    if (event.type == BleWriterKeyboard::KeyType::OpenFiles) {
      if (!fileMenu && dirty && !saveDraft()) {
        Serial.println("Cannot open file picker: autosave failed");
        continue;
      }
      fileMenu = !fileMenu;
      fileList = writerfiles::list();
      const auto found = std::find(fileList.begin(), fileList.end(), activeName);
      fileIndex = found != fileList.end() ? static_cast<size_t>(found - fileList.begin()) : 0;
      renderPending = true;
      lastKeyAt = millis();
      continue;
    }
    if (event.type == BleWriterKeyboard::KeyType::NewFile) {
      if (!createDraft()) Serial.println("Cannot create draft; check storage");
      renderPending = true;
      continue;
    }
    if (event.type == BleWriterKeyboard::KeyType::SaveFile) {
      if (!saveDraft()) Serial.println("Save failed; original remains available");
      renderPending = true;
      continue;
    }
    if (fileMenu) {
      switch (event.type) {
        case BleWriterKeyboard::KeyType::Up:
          if (fileIndex > 0) --fileIndex;
          break;
        case BleWriterKeyboard::KeyType::Down:
          if (fileIndex + 1 < fileList.size()) ++fileIndex;
          break;
        case BleWriterKeyboard::KeyType::Enter:
          if (fileIndex < fileList.size() && !switchDraft(fileList[fileIndex]))
            Serial.println("Open failed; current draft preserved");
          break;
        case BleWriterKeyboard::KeyType::Escape:
          fileMenu = false;
          break;
        default:
          break;
      }
      renderPending = true;
      lastKeyAt = millis();
      continue;
    }
    switch (event.type) {
      case BleWriterKeyboard::KeyType::Character: insertCharacter(event.character); break;
      case BleWriterKeyboard::KeyType::Enter: insertCharacter('\n'); break;
      case BleWriterKeyboard::KeyType::Backspace: backspace(); break;
      case BleWriterKeyboard::KeyType::DeleteKey: deleteForward(); break;
      case BleWriterKeyboard::KeyType::Tab: insertText("    "); break;
      case BleWriterKeyboard::KeyType::Left: moveCursorLeft(); break;
      case BleWriterKeyboard::KeyType::Right: moveCursorRight(); break;
      case BleWriterKeyboard::KeyType::Up: moveCursorVertical(-1); break;
      case BleWriterKeyboard::KeyType::Down: moveCursorVertical(1); break;
      default: break;
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
  Serial.begin(115200);
  delay(250);
  Serial.printf("E213 writer bring-up: flash=%lu psram=%lu\n",
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getPsramSize()));

  Serial.println("Initializing E213 V1.1 E0213A367-BW display...");
  display.begin();
  display.landscape();
  display.clear();
  Serial.printf("E-ink ready: %dx%d\n", display.width(), display.height());

  storageReady = loadDraft();
  Serial.printf("Internal draft storage: %s (%u bytes loaded)\n",
                storageReady ? "ready" : "unavailable",
                static_cast<unsigned>(document.size()));

  keyboard.begin();
  lastKeyboardStatusVersion = keyboard.statusVersion();
  renderPairing(true);
}

void loop() {
  // USB transfers never overwrite unsaved keystrokes. Failure leaves the draft intact.
  if (Serial.available() && dirty && storageReady && !saveDraft()) {
    Serial.println("USB sync blocked: unsaved draft");
  } else if (storageReady) {
    serialSync.poll();
    const std::string updated = serialSync.takeUpdatedName();
    if (updated == activeName && !dirty) {
      std::string replacement;
      if (writerfiles::read(activeName, replacement)) {
        document.swap(replacement);
        cursor = std::min(cursor, document.size());
        renderPending = true;
      }
    }
  }
  keyboard.update();
  handlePairingButtons();

  const bool connected = keyboard.isConnected();
  if (connected) {
    if (serialSync.busy()) {
      // Do not accept keystrokes into a document while USB is replacing it.
      BleWriterKeyboard::KeyEvent discarded;
      while (keyboard.popEvent(discarded)) {}
    } else {
      handleKeyboardEvents();
    }

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
