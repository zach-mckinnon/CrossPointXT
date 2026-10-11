#include <Arduino.h>
#include <heltec-eink-modules.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BleWriterKeyboard.h"
#include "WriterFiles.h"
#include "WriterSyncSerial.h"
#include "WordWrap.h"
#include "WriterStats.h"
#include "WriterEditing.h"
#include "BatteryStatus.h"

// E213 writer bring-up:
// - external SPI microSD required for all working documents and sync metadata
// - BLE HID keyboard input
// - USB serial transfer reads/writes that same microSD card
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
constexpr int kFocusRows = 13;
constexpr int kFocusTop = 4;

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

using writerlayout::VisualLine;

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
bool saveFailed = false;
bool focusMode = false;
writerediting::History history;
int batteryMillivolts = 0;
uint32_t batteryReadAt = 0;
bool renderPending = true;
bool syncWasBusy = false;
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
  display.setTextWrap(false); // We wrap whole words ourselves, not by Adafruit GFX.
}

std::vector<VisualLine> buildVisualLines() {
  return writerlayout::wrapWords(document, kCharactersPerLine);
}

size_t cursorVisualLine(const std::vector<VisualLine>& lines) {
  return writerlayout::lineForCursor(lines, cursor);
}

int cursorColumn(const VisualLine& line) {
  return static_cast<int>(writerlayout::columnForCursor(line, cursor));
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
    if (!storageReady) {
      display.print("SD ERROR - WRITING DISABLED");
    } else if (keyboard.isConnecting()) {
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
  if (!storageReady) {
    prepareDisplayFrame();
    display.setCursor(6, 10);
    display.println("MICRO WRITER - SD REQUIRED");
    display.drawLine(0, 14, display.width()-1, 14, BLACK);
    display.setCursor(6, 40);
    display.println("No writable SD card found.");
    display.setCursor(6, 57);
    display.println("Check SPI wiring + FAT32.");
    display.setCursor(6, 73);
    display.println("Then restart the E213.");
    display.setCursor(6, 119);
    display.println("Internal flash left untouched.");
    display.update();
    renderPending = false;
    lastRenderAt = millis();
    return;
  }
  if (fileMenu) { renderFilePicker(); return; }
  const std::vector<VisualLine> lines = buildVisualLines();
  const size_t activeLine = cursorVisualLine(lines);
  const int rows = focusMode ? kFocusRows : kVisibleRows;
  const size_t firstLine = activeLine >= static_cast<size_t>(rows - 1)
                               ? activeLine - static_cast<size_t>(rows - 1) : 0;

  (void)forceFull;
  prepareDisplayFrame();

    if (!focusMode) {
    // Top: manuscript name left, save state right. Never let them overlap.
    const char* status = serialSync.busy() ? "USB SYNC" :
                         saveFailed ? "SAVE ERR" :
                         dirty ? "UNSAVED" : "SAVED";
    const std::string saveLabel(status);
    const int saveX = writerstats::rightAlignedX(display.width(), saveLabel);
    const size_t maxTitle = writerstats::titleColumns(saveX);
    display.setCursor(kTextLeft, 9);
    display.print(cropped(activeName, maxTitle).c_str());
    display.setCursor(saveX, 9);
    display.print(saveLabel.c_str());
    display.drawLine(0, 14, display.width() - 1, 14, BLACK);
    }

    for (int row = 0; row < rows; ++row) {
      const size_t index = firstLine + static_cast<size_t>(row);
      if (index >= lines.size()) break;
      const int baseline = (focusMode ? kFocusTop : kTextTop) + row * kTextBaselineStep;
      display.setCursor(kTextLeft, baseline);
      display.print(lines[index].text.c_str());

      if (index == activeLine) {
        const int column = std::min(cursorColumn(lines[index]), kCharactersPerLine);
        const int x = std::min(display.width() - 2, kTextLeft + column * 6);
        display.drawLine(x, baseline - 7, x, baseline, BLACK);
      }
    }

    if (!focusMode) {
    // Battery left, Bluetooth center, live word count right.
    display.drawLine(0, 110, display.width() - 1, 110, BLACK);
    display.setCursor(6, 119);
    display.print(writerbattery::displayLabel(batteryMillivolts).c_str());
    const std::string btLabel = keyboard.isConnected() ? "BT:OK" : "BT:--";
    display.setCursor(writerstats::centeredX(display.width(), btLabel), 119);
    display.print(btLabel.c_str());
    const std::string wordLabel = std::to_string(writerstats::countWords(document)) + " words";
    display.setCursor(writerstats::rightAlignedX(display.width(), wordLabel), 119);
    display.print(wordLabel.c_str());
    }
  display.update();

  lastRenderAt = millis();
  renderPending = false;
}

bool loadDraft() {
  // SD is the source of truth. Do not silently fall back to limited flash.
  if (!writerfiles::begin()) {
    Serial.println("SD unavailable: writing and manuscript sync disabled.");
    return false;
  }
  // Copy any old internal-flash drafts to the card, never delete their originals.
  // A failed import stops new writing until the source can be recovered safely.
  if (!writerfiles::importLegacyFlashDrafts()) {
    Serial.println("Legacy migration incomplete; recover old flash files before writing.");
    return false;
  }
  fileList = writerfiles::list();
  if (fileList.empty()) {
    // Fresh card: create the first document directly on SD, not LittleFS.
    if (!writerfiles::save(activeName, "")) return false;
    fileList = writerfiles::list();
  }
  if (fileList.empty()) return false;
  std::string previous;
  activeName = writerfiles::readActiveName(previous) && writerfiles::exists(previous)
                 ? previous : fileList.front();
  return writerfiles::read(activeName, document);
}

bool saveDraft() {
  if (!storageReady || !writerfiles::save(activeName, document)) {
    // A failed save must be visible, not reported as merely "unsaved".
    saveFailed = true;
    renderPending = true;
    return false;
  }
  dirty = false;
  saveFailed = false;
  renderPending = true;
  return true;
}

bool switchDraft(const std::string& name) {
  if (dirty && !saveDraft()) return false; // Never switch away from unsaved work.
  std::string next;
  if (!writerfiles::read(name, next)) return false;
  activeName = name;
  document.swap(next);
  history.clear(); // Never carry editing history into a different draft.
  saveFailed = false;
  cursor = document.size();
  if (!writerfiles::saveActiveName(name))
    Serial.println("Warning: active draft preference not saved");
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
  if (history.change(document, cursor, cursor, 0, std::string(1, c),
                     millis(), writerediting::Kind::Insert)) markEdited();
}

void insertText(const char* text) {
  if (!text) return;
  const size_t available = kMaxDraftBytes - document.size();
  const std::string bytes(text);
  if (bytes.empty() || available == 0) return;
  if (history.change(document, cursor, cursor, 0, bytes.substr(0, available),
                     millis(), writerediting::Kind::Other)) markEdited();
}

void backspace() {
  if (cursor == 0 || document.empty()) return;
  const size_t start = writerediting::previousCodepoint(document, cursor);
  if (history.change(document, cursor, start, cursor - start, "",
                     millis(), writerediting::Kind::Backspace)) markEdited();
}

void deleteForward() {
  if (cursor >= document.size()) return;
  const size_t end = writerediting::nextCodepoint(document, cursor);
  if (history.change(document, cursor, cursor, end - cursor, "",
                     millis(), writerediting::Kind::DeleteForward)) markEdited();
}

void moveCursorLeft() {
  if (cursor > 0) {
    cursor = writerediting::previousCodepoint(document, cursor);
    renderPending = true;
    lastKeyAt = millis();
  }
}

void moveCursorRight() {
  if (cursor < document.size()) {
    cursor = writerediting::nextCodepoint(document, cursor);
    renderPending = true;
    lastKeyAt = millis();
  }
}

void navigateTo(const size_t position) {
  cursor = std::min(position, document.size());
  renderPending = true;
  lastKeyAt = millis();
}

void undoEdit() {
  if (history.undo(document, cursor)) markEdited();
}
void redoEdit() {
  if (history.redo(document, cursor)) markEdited();
}

void moveCursorVertical(const int direction) {
  const std::vector<VisualLine> lines = buildVisualLines();
  if (lines.empty()) return;

  const size_t current = cursorVisualLine(lines);
  const int target = static_cast<int>(current) + direction;
  if (target < 0 || target >= static_cast<int>(lines.size())) return;

  const int column = cursorColumn(lines[current]);
  const VisualLine& destination = lines[static_cast<size_t>(target)];
  cursor = destination.start + std::min(static_cast<size_t>(column), destination.text.size());
  renderPending = true;
  lastKeyAt = millis();
}

void handleKeyboardEvents() {
  BleWriterKeyboard::KeyEvent event;
  while (keyboard.popEvent(event)) {
    // An absent or failed SD never turns the editor into unsafe RAM-only mode.
    if (!storageReady) continue;
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
      case BleWriterKeyboard::KeyType::Undo: undoEdit(); break;
      case BleWriterKeyboard::KeyType::Redo: redoEdit(); break;
      case BleWriterKeyboard::KeyType::FocusMode:
        focusMode = !focusMode;
        renderPending = true;
        lastKeyAt = millis();
        break;
      case BleWriterKeyboard::KeyType::HomeSentence:
        navigateTo(writerediting::sentenceStart(document, cursor)); break;
      case BleWriterKeyboard::KeyType::EndSentence:
        navigateTo(writerediting::sentenceEnd(document, cursor)); break;
      case BleWriterKeyboard::KeyType::HomeDocument: navigateTo(0); break;
      case BleWriterKeyboard::KeyType::EndDocument: navigateTo(document.size()); break;
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

  writerbattery::begin();
  batteryMillivolts = writerbattery::sampleMillivolts();
  batteryReadAt = millis();
  storageReady = loadDraft();
  Serial.printf("Battery sense: %d mV (0=unavailable)\n", batteryMillivolts);
  Serial.printf("SD draft storage: %s (%u bytes loaded)\n",
                storageReady ? "ready" : "unavailable",
                static_cast<unsigned>(document.size()));

  keyboard.begin();
  lastKeyboardStatusVersion = keyboard.statusVersion();
  renderPairing(true);
}

void loop() {
  if (millis() - batteryReadAt >= writerbattery::kReadIntervalMs && !serialSync.busy()) {
    batteryMillivolts = writerbattery::sampleMillivolts();
    batteryReadAt = millis();
    renderPending = true;
  }
  // USB transfers never overwrite unsaved keystrokes. Failure leaves the draft intact.
  if (Serial.available() && dirty && storageReady && !saveDraft()) {
    // Do not acknowledge a transfer while a dirty document cannot be saved.
    Serial.println("USB sync blocked: unsaved draft");
  } else {
    // Even without SD, answer USB HELLO with an actionable sd-unavailable error.
    serialSync.poll();
    const std::string updated = serialSync.takeUpdatedName();
    if (updated == activeName && !dirty) {
      std::string replacement;
      if (writerfiles::read(activeName, replacement)) {
        document.swap(replacement);
        history.clear(); // USB replaced the active manuscript.
        saveFailed = false;
        cursor = std::min(cursor, document.size());
        renderPending = true;
      }
    }
  }
  const bool syncBusy = serialSync.busy();
  if (syncBusy != syncWasBusy) {
    syncWasBusy = syncBusy;
    renderPending = true;
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

    const unsigned long now = millis();
    if (renderPending &&
        ((now - lastKeyAt >= kRenderIdleDelayMs) || (now - lastRenderAt >= kRenderMaxDelayMs))) {
      renderWriter();
    }
  }

  // A keyboard can disconnect immediately after typing; autosave must not be
  // conditional on the BLE connection remaining active.
  if (dirty && storageReady && !serialSync.busy() &&
      millis() - lastEditAt >= kAutosaveDelayMs) {
    if (saveDraft()) {
      Serial.printf("Draft autosaved (%u bytes)\n", static_cast<unsigned>(document.size()));
      renderPending = true;
    } else {
      Serial.println("Draft autosave failed");
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
