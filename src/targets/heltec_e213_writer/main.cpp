#include <Arduino.h>
#include <HeltecE213Display.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include "WriterStore.h"
#include "WriterBrowser.h"
#include "WriterButton.h"

#include "BleWriterKeyboard.h"

// E213 writer bring-up:
// - SD files with internal recovery checkpoints
// - BLE HID keyboard input
// - bounded text editor and paged file browser
// - e-ink partial refresh with a typing-friendly debounce
namespace {
constexpr uint8_t kNextButton = 42;    // External button 1 to GND, active LOW.
constexpr uint8_t kSelectButton = 47;  // External button 2 to GND, active LOW.

constexpr size_t kMaxDraftBytes = 64 * 1024;

constexpr int kTextLeft = 6;
constexpr int kTextTop = 20;
// The built-in 6x8 font uses the top of the character cell, not a baseline.
constexpr int kCharacterWidth = 6;
constexpr int kCharacterHeight = 8;
constexpr int kTextRowStep = 9;
constexpr int kHeaderTop = 4;
constexpr int kHeaderDivider = 14;
constexpr int kFooterHeight = 14;
constexpr int kCharactersPerLine = 39;
constexpr int kVisibleRows = 9;

constexpr unsigned long kRenderIdleDelayMs = 120;
constexpr unsigned long kRenderMaxDelayMs = 500;
constexpr unsigned long kCleanupIdleDelayMs = 3000;
constexpr unsigned long kAutosaveDelayMs = 1800;
constexpr unsigned long kPairingRefreshMs = 1000;

// The panel fitted to this V1.1 board was physically verified as FC1-compatible.
// Select its driver in platformio.ini; the PCB revision alone is insufficient.
HeltecE213Display display;
bool writerReady = false;
bool sdTestPassed = false;

BleWriterKeyboard keyboard;

struct Button : WriterButton {
  uint8_t pin;
  explicit Button(uint8_t value) : pin(value) {}
  ButtonEvent poll() { return update(digitalRead(pin) == LOW, millis()); }
};

Button nextButton{kNextButton};
Button selectButton{kSelectButton};

WriterStore writerStore;
WriterText& document = writerStore.text;
WriterBrowser browser;
enum class Screen { Pairing, Editor, Files };
Screen screen = Screen::Pairing;
bool fileError = false;
size_t cursor = 0;
bool storageReady = false;
bool saveFailed = false;
bool sleepFailed = false;
bool renderPending = true;
uint32_t lastKeyboardStatusVersion = 0;
unsigned long lastEditAt = 0;
unsigned long lastKeyAt = 0;
unsigned long lastRenderAt = 0;
unsigned long lastPairingRenderAt = 0;
unsigned long lastSaveAttemptAt = 0;

void prepareDisplayFrame(const bool forceFull, const bool allowCleanup = true) {
  display.prepareFrame(forceFull, allowCleanup);
  display.setTextColor(BLACK);
  display.setTextSize(1);
  display.setTextWrap(false);
}

// Scan row boundaries without allocating a row/string for every line.
size_t activeRow() {
  size_t row = 0;
  size_t at = 0;
  while (true) {
    const auto line = writerLine(document, at);
    if (cursor < line.end || (cursor == line.end && (!line.more || document[cursor] == '\n'))) return row;
    if (!line.more) return row;
    at = line.next;
    ++row;
  }
}

void printCropped(const char* value, size_t maxChars) {
  for (size_t i = 0; i < maxChars && value[i]; ++i) display.print(value[i]);
}

void renderPairing(const bool forceFull = false) {
  prepareDisplayFrame(forceFull);

    display.setCursor(kTextLeft, kHeaderTop);
    display.println("E213 MICRO WRITER");
    display.setCursor(display.width() - 7 * kCharacterWidth - 6, kHeaderTop);
    display.print(sdTestPassed ? tr(STR_WRITER_SD_TEST_OK) : tr(STR_WRITER_SD_TEST_FAIL));
    display.drawLine(0, kHeaderDivider, display.width() - 1, kHeaderDivider, BLACK);

    display.setCursor(6, 27);
    display.println(keyboard.statusText());

    const int count = keyboard.candidateCount();
    if (keyboard.hasAutomaticReconnect()) {
      display.setCursor(6, 45);
      display.println(tr(STR_WRITER_RECONNECT_WAKE));
      display.setCursor(6, 58);
      display.println(tr(STR_WRITER_RECONNECT_SLOT));
      display.setCursor(6, 89);
      display.println(tr(STR_WRITER_RECONNECT_MANUAL));
    } else if (count == 0) {
      display.setCursor(6, 45);
      display.println("1. Turn keyboard on");
      display.setCursor(6, 58);
      display.println("2. Put it in pairing mode");
      display.setCursor(6, 71);
      display.println("3. Wait for it to appear");
      display.setCursor(6, 89);
      display.println(tr(STR_WRITER_BUTTON_SCAN));
    } else {
      const int selected = keyboard.selectedCandidateIndex();
      const int first = std::max(0, selected - 2);
      for (int row = 0; row < 5 && first + row < count; ++row) {
        BleWriterKeyboard::Candidate candidate;
        if (!keyboard.getCandidate(first + row, candidate)) continue;
        display.setCursor(6, 43 + row * 12);
        display.print(first + row == selected ? "> " : "  ");
        printCropped(candidate.name, 30);
      }
    }

    const int footerDivider = display.height() - kFooterHeight;
    display.drawLine(0, footerDivider, display.width() - 1, footerDivider, BLACK);
    display.setCursor(kTextLeft, display.height() - kCharacterHeight - 2);
    if (keyboard.hasAutomaticReconnect()) {
      display.print(tr(STR_WRITER_RECONNECT_MANUAL));
    } else if (keyboard.isConnecting()) {
      display.print("Connecting...");
    } else if (count > 0) {
      display.print(tr(STR_WRITER_BUTTON_PAIR));
    } else {
      display.print("BLE keyboards only");
    }
  display.update();

  lastRenderAt = millis();
  lastPairingRenderAt = lastRenderAt;
}

void renderWriter(const bool forceFull = false) {
  const size_t activeLine = activeRow();
  const size_t firstLine = activeLine >= static_cast<size_t>(kVisibleRows - 1)
                               ? activeLine - static_cast<size_t>(kVisibleRows - 1) : 0;
  // Automatic cleanup is scheduled by loop() only after typing has paused.
  prepareDisplayFrame(forceFull, false);

    display.setCursor(kTextLeft, kHeaderTop);
    printCropped(writerStore.onCard() ? writerStore.name() : tr(STR_WRITER_DRAFT), 20);
    display.setCursor(132, kHeaderTop);
    display.print(keyboard.isConnected() ? "BT:OK" : "BT:--");
    display.setCursor(178, kHeaderTop);
    display.print(document.size());

    display.drawLine(0, kHeaderDivider, display.width() - 1, kHeaderDivider, BLACK);

    size_t at = 0;
    for (size_t index = 0; index < firstLine + kVisibleRows; ++index) {
      const auto line = writerLine(document, at);
      if (index >= firstLine) {
        const int rowTop = kTextTop + (index - firstLine) * kTextRowStep;
        display.setCursor(kTextLeft, rowTop);
        for (size_t c = line.start; c < line.end; c = nextCharacter(document, c)) {
          if (document[c] == '\r') continue;
          // Built-in panel font is ASCII; UTF-8 bytes remain intact on disk.
          display.print(static_cast<uint8_t>(document[c]) >= 128 ? '?' : document[c] == '\t' ? ' ' : document[c]);
        }
        if (index == activeLine) {
          const int column = writerColumn(document, line.start, cursor);
          const int x = std::min(display.width() - 2, kTextLeft + column * kCharacterWidth);
          display.drawLine(x, rowTop, x, rowTop + kCharacterHeight - 1, BLACK);
        }
      }
      if (!line.more) break;
      at = line.next;
    }

    const int footerDivider = display.height() - kFooterHeight;
    display.drawLine(0, footerDivider, display.width() - 1, footerDivider, BLACK);
    display.setCursor(kTextLeft, display.height() - kCharacterHeight - 2);
    if (!storageReady) {
      display.print("RAM ONLY - flash unavailable");
    } else if (saveFailed) {
      display.print(tr(STR_WRITER_SAVE_FAILED));
    } else if (sleepFailed) {
      display.print(tr(STR_WRITER_SLEEP_FAILED));
    } else if (writerStore.dirty()) {
      display.print("Editing... autosave pending");
    } else {
      display.print(writerStore.onCard() ? tr(STR_WRITER_SAVED_SD) : tr(STR_WRITER_SAVED_FLASH));
    }
  display.update();

  lastRenderAt = millis();
  renderPending = false;
}

bool saveNow() {
  lastSaveAttemptAt = millis();
  saveFailed = !writerStore.save();
  renderPending = true;
  if (saveFailed) {
    LOG_ERR("E213", "Draft save failed; keeping draft in RAM");
    return false;
  }
  LOG_INF("E213", "Draft saved (%u bytes): %s", static_cast<unsigned>(document.size()),
          writerStore.onCard() ? writerStore.path() : "internal recovery");
  return true;
}

void saveAndSleep() {
  screen = Screen::Editor;
  if (!saveNow()) {
    renderWriter();
    return;
  }
  if (!display.blankForSleep() || !keyboard.end()) {
    sleepFailed = true;
    LOG_ERR("E213", "Sleep cancelled: display or BLE shutdown failed");
    renderWriter();
    return;
  }
  writerStore.shutdown();
  LOG_INF("E213", "Draft saved; screen blank; entering deep sleep. RESET wakes.");
  logSerial.flush();
  display.sleepUntilReset();
}

void markEdited() {
  writerStore.edited();
  renderPending = true;
  lastEditAt = millis();
  lastKeyAt = lastEditAt;
}

void insertCharacter(const char c) {
  if (document.size() >= kMaxDraftBytes) return;
  if (!document.insert(cursor, c)) return;
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
  const size_t previous = previousCharacter(document, cursor);
  document.erase(previous, cursor - previous);
  cursor = previous;
  markEdited();
}

void deleteForward() {
  if (cursor >= document.size()) return;
  document.erase(cursor, nextCharacter(document, cursor) - cursor);
  markEdited();
}

void moveCursorLeft() {
  if (cursor > 0) {
    cursor = previousCharacter(document, cursor);
    renderPending = true;
    lastKeyAt = millis();
  }
}

void moveCursorRight() {
  if (cursor < document.size()) {
    cursor = nextCharacter(document, cursor);
    renderPending = true;
    lastKeyAt = millis();
  }
}

void moveCursorVertical(const int direction) {
  const size_t current = activeRow();
  if (!current && direction < 0) return;
  const size_t target = direction < 0 ? current - 1 : current + 1;
  WriterLine source{}, destination{};
  size_t at = 0;
  for (size_t row = 0; row <= std::max(current, target); ++row) {
    const auto line = writerLine(document, at);
    if (row == current) source = line;
    if (row == target) destination = line;
    if (!line.more && row < std::max(current, target)) return;
    at = line.next;
  }
  size_t column = writerColumn(document, source.start, cursor);
  cursor = destination.start;
  while (column && cursor < destination.end) {
    if (document[cursor] != '\r') --column;
    cursor = nextCharacter(document, cursor);
  }
  renderPending = true;
  lastKeyAt = millis();
}

void renderBrowser() {
  prepareDisplayFrame(false, false);
  display.setCursor(kTextLeft, kHeaderTop);
  printCropped(browser.directory, 39);
  display.drawLine(0, kHeaderDivider, display.width() - 1, kHeaderDivider, BLACK);
  const size_t first = browser.selected >= 8 ? browser.selected - 7 : 0;
  for (size_t row = 0; row < 9 && first + row < browser.total(); ++row) {
    const size_t index = first + row;
    display.setCursor(kTextLeft, kTextTop + row * kTextRowStep);
    display.print(index == browser.selected ? ">" : " ");
    switch (index) {
      case 0: display.print(tr(STR_WRITER_RESUME)); break;
      case 1: display.print(tr(STR_WRITER_NEW)); break;
      case 2: display.print(tr(STR_WRITER_PARENT)); break;
      case 3: display.print(browser.more ? tr(STR_WRITER_NEXT_PAGE) : tr(STR_WRITER_FIRST_PAGE)); break;
      default:
        display.print(browser.entries[index - 4].directory ? "/" : " ");
        printCropped(browser.entries[index - 4].name, 36);
        break;
    }
  }
  display.setCursor(kTextLeft, display.height() - kCharacterHeight - 2);
  display.print(fileError || browser.failed ? tr(STR_WRITER_FILE_ERROR) : tr(STR_WRITER_FILE_KEYS));
  display.update();
}

void showFiles() {
  if (!saveNow()) { screen = Screen::Editor; renderWriter(); return; }
  screen = Screen::Files;
  fileError = false;
  browser.skip = 0;
  browser.load();
  renderBrowser();
}

void chooseFile() {
  fileError = false;
  switch (browser.selected) {
    case 0: screen = Screen::Editor; renderWriter(); return;
    case 1:
      if (!writerStore.create()) { fileError = true; break; }
      cursor = 0;
      screen = Screen::Editor;
      saveNow();
      renderWriter();
      return;
    case 2: browser.parent(); break;
    case 3: browser.page(); break;
    default:
      if (!browser.choosePath()) { fileError = true; break; }
      if (browser.entries[browser.selected - 4].directory) { browser.enter(); break; }
      if (!writerStore.open(browser.selectedPath)) { fileError = true; break; }
      cursor = document.size();
      screen = Screen::Editor;
      renderWriter();
      return;
  }
  renderBrowser();
}

void handleKeyboardEvents() {
  BleWriterKeyboard::KeyEvent event;
  while (keyboard.popEvent(event)) {
    if (screen == Screen::Files && event.type != BleWriterKeyboard::KeyType::Sleep) {
      switch (event.type) {
        case BleWriterKeyboard::KeyType::Down: browser.next(); renderBrowser(); break;
        case BleWriterKeyboard::KeyType::Up: browser.previous(); renderBrowser(); break;
        case BleWriterKeyboard::KeyType::Enter: chooseFile(); break;
        case BleWriterKeyboard::KeyType::Escape:
        case BleWriterKeyboard::KeyType::Files: screen = Screen::Editor; renderWriter(); break;
        default: break; // Text keys cannot edit a document behind the browser.
      }
      continue;
    }
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
      case BleWriterKeyboard::KeyType::Save:
        saveNow();
        renderWriter();
        break;
      case BleWriterKeyboard::KeyType::Files:
        showFiles();
        break;
      case BleWriterKeyboard::KeyType::Sleep:
        saveAndSleep();
        return;
    }
  }
}

void handlePairingButtons() {
  const ButtonEvent next = nextButton.poll();
  const ButtonEvent select = selectButton.poll();

  if (next != ButtonEvent::None) {
    LOG_INF("E213", "Button 1 GPIO42: %s", next == ButtonEvent::Short ? "short" : "long");
  }
  if (select != ButtonEvent::None) {
    LOG_INF("E213", "Button 2 GPIO47: %s", select == ButtonEvent::Short ? "short" : "long");
  }

  // Sleep is available even with BLE disconnected or a browser open.
  if (select == ButtonEvent::Long) { saveAndSleep(); return; }
  if (screen == Screen::Files) {
    if (next == ButtonEvent::Short) { browser.next(); renderBrowser(); }
    if (next == ButtonEvent::Long) {
      if (browser.parent()) renderBrowser();
      else { screen = Screen::Editor; renderWriter(); }
    }
    if (select == ButtonEvent::Short) chooseFile();
    return;
  }
  if (next == ButtonEvent::Long || (screen == Screen::Editor && next == ButtonEvent::Short)) {
    showFiles();
    return;
  }
  if (screen == Screen::Editor) {
    if (select == ButtonEvent::Short) { saveNow(); renderWriter(); }
    return;
  }
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
      screen = Screen::Editor;
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
  logSerial.begin(115200);
  const uint32_t serialStartedAt = millis();
  while (!logSerial && millis() - serialStartedAt < 10000) {
    delay(10);
  }
  delay(250);
  LOG_INF("E213", "Writer bring-up: flash=%lu psram=%lu", static_cast<unsigned long>(ESP.getFlashChipSize()),
          static_cast<unsigned long>(ESP.getPsramSize()));
  LOG_INF("E213", "External buttons: GPIO42=%d GPIO47=%d (released=1, pressed=0)",
          digitalRead(kNextButton), digitalRead(kSelectButton));

  LOG_INF("E213", "Initializing E213 display...");
  logSerial.flush();
  display.begin();
  if (display.hasTimedOut()) return;
  display.landscape();
  display.clear();
  if (display.hasTimedOut()) return;
  LOG_INF("E213", "E-ink ready: %dx%d", display.width(), display.height());

  storageReady = writerStore.begin();
  cursor = document.size();
  LOG_INF("E213", "Internal draft storage: %s (%u bytes loaded)", storageReady ? "ready" : "unavailable",
          static_cast<unsigned>(document.size()));

  sdTestPassed = writerStore.sdReady();

  LOG_INF("E213", "Starting BLE keyboard...");
  logSerial.flush();
  keyboard.begin();
  lastKeyboardStatusVersion = keyboard.statusVersion();
  renderPairing(true);
  writerReady = !display.hasTimedOut();
  LOG_INF("E213", "Writer startup %s", writerReady ? "complete" : "failed");
}

void loop() {
  static uint32_t lastHeartbeatAt = 0;
  if (millis() - lastHeartbeatAt >= 5000) {
    lastHeartbeatAt = millis();
    LOG_INF("E213", "Writer alive: ready=%d displayTimeout=%d heap=%lu sdMounted=%d buttons=%d/%d", writerReady,
            display.hasTimedOut(), static_cast<unsigned long>(ESP.getFreeHeap()), sdTestPassed,
            digitalRead(kNextButton), digitalRead(kSelectButton));
  }
  if (!writerReady) {
    delay(10);
    return;
  }
  handlePairingButtons();
  // Don't start a synchronous BLE connection while a button is being held.
  if (digitalRead(kNextButton) != LOW && digitalRead(kSelectButton) != LOW) keyboard.update();

  const bool connected = keyboard.isConnected();
  if (connected) handleKeyboardEvents();
  if (screen == Screen::Editor) {

    if (writerStore.dirty() && storageReady && millis() - lastEditAt >= kAutosaveDelayMs &&
        (!saveFailed || millis() - lastSaveAttemptAt >= 5000)) {
      saveNow();
    }

    const unsigned long now = millis();
    if (display.cleanupDue() && now - lastKeyAt >= kCleanupIdleDelayMs) {
      renderWriter(true);
    } else if (renderPending &&
        ((now - lastKeyAt >= kRenderIdleDelayMs) || (now - lastRenderAt >= kRenderMaxDelayMs))) {
      renderWriter();
    }
  }

  const uint32_t statusVersion = keyboard.statusVersion();
  if (statusVersion != lastKeyboardStatusVersion && screen != Screen::Files) {
    if (!keyboard.isConnected() &&
        (keyboard.isConnecting() || millis() - lastPairingRenderAt >= kPairingRefreshMs)) {
      screen = Screen::Pairing;
      renderPairing();
      lastKeyboardStatusVersion = statusVersion;
    } else if (keyboard.isConnected()) {
      screen = Screen::Editor;
      renderPending = true;
      lastKeyboardStatusVersion = statusVersion;
    }
  }

  delay(5);
}
