#include <Arduino.h>
#include "HeltecE213Display.h"
#include <esp_sleep.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BleWriterKeyboard.h"
#include "WriterFiles.h"
#include "WriterSyncSerial.h"
#include "WordWrap.h"
#include "WriterStats.h"
#include "WriterScreenGeometry.h"
#include "WriterEditing.h"
#include "BatteryStatus.h"
#include "WriterNames.h"
#include "WriterOutline.h"
#include "SyncManifest.h"

// E213 writer bring-up:
// - external SPI microSD required for all working documents and sync metadata
// - BLE HID keyboard input
// - USB serial transfer reads/writes that same microSD card
// - e-ink partial refresh with a typing-friendly debounce
namespace {
constexpr uint8_t kNextButton = 42;    // Existing wired external B1, active LOW.
constexpr uint8_t kSelectButton = 47;  // Existing wired external B2, active LOW.

constexpr size_t kMaxDraftBytes = writerfiles::kMaxDocumentBytes;

constexpr int kTextLeft = writergeometry::kLeft;
constexpr int kCharactersPerLine = 39;
constexpr int kVisibleRows = writergeometry::kNormalRows;
constexpr int kFocusRows = writergeometry::kFocusRows;

constexpr unsigned long kRenderIdleDelayMs = 220;
constexpr unsigned long kRenderMaxDelayMs = 900;
constexpr unsigned long kAutosaveDelayMs = 1800;
constexpr unsigned long kPairingRefreshMs = 1000;

// This assembled E213 V1.1 has the FC1 panel verified during hardware bring-up.
// The adapter bounds BUSY waits and handles Vext sequencing and sleep.
HeltecE213Display display;

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

    return now - pressedAt >= 1200 ? ButtonEvent::Long : ButtonEvent::Short;
  }
};

using writerlayout::VisualLine;

Button nextButton{kNextButton};
Button selectButton{kSelectButton};

std::string document;
std::string activeName = "Draft-001.txt";
std::vector<std::string> fileList;
std::vector<writeroutline::Section> fileSections;
std::vector<writeroutline::Project> projectList;
size_t fileIndex = 0;
size_t sectionIndex = 0;
size_t projectIndex = 0;
bool fileMenu = false;
bool chapterMenu = false;
bool projectMenu = false;
enum class NameMode { None, Create, Rename };
NameMode nameMode = NameMode::None;
std::string nameInput;
std::string renameSource;
std::string nameError;
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
bool usbSyncMode = false;
bool manualUsbSyncMode = false;
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

void prepareDisplayFrame(bool forceFull = false) {
  display.prepareFrame(forceFull);
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
  prepareDisplayFrame(forceFull);

    display.setCursor(kTextLeft, writergeometry::kHeaderTop);
    display.println("E213 MICRO WRITER");
    display.drawLine(0, writergeometry::kHeaderDivider,
                     display.width() - 1, writergeometry::kHeaderDivider, BLACK);

    display.setCursor(6, 27);
    display.println(keyboard.statusText());

    const int count = keyboard.candidateCount();
    if (count == 0) {
      if (keyboard.hasAutomaticReconnect()) {
        display.setCursor(6, 45);
        display.println("Saved keyboard: waiting for BLE");
        display.setCursor(6, 58);
        display.println("Tap a key to wake the keyboard");
        display.setCursor(6, 71);
        display.println("Keep the same Bluetooth slot");
        display.setCursor(6, 89);
        display.println("B1: manual scan / other device");
      } else {
        display.setCursor(6, 45);
        display.println("1. Turn keyboard on");
        display.setCursor(6, 58);
        display.println("2. Put it in pairing mode");
        display.setCursor(6, 71);
        display.println("3. Wait for it to appear");
        display.setCursor(6, 89);
        display.println("B1: scan again");
      }
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

    display.drawLine(0, writergeometry::footerDivider(display.height()),
                     display.width() - 1, writergeometry::footerDivider(display.height()), BLACK);
    display.setCursor(kTextLeft, writergeometry::footerTop(display.height()));
    if (!storageReady) {
      display.print("SD ERROR - WRITING DISABLED");
    } else if (keyboard.isConnecting()) {
      display.print("Connecting...");
    } else if (count > 0) {
      display.print("B1 next | B2 connect");
    } else {
      display.print("BLE keyboards only");
    }
  display.update();

  lastRenderAt = millis();
  lastPairingRenderAt = lastRenderAt;
}

// Prefer Plot n Jot's persisted chapter/scene order; fallback to the original
// flat alphabetical picker for standalone drafts or older .sync manifests.
// The file picker is a non-destructive VIRTUAL hierarchy. All SD manuscripts
// remain in /drafts, so old project links and personal diaries remain intact.
void openFileBrowser() {
  fileList = writerfiles::list();
  std::vector<writeroutline::Project> desired;
  std::string json;
  if (writerfiles::readManifest(json)) {
    JsonDocument doc;
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
      auto appendProject = [&](JsonObject project) {
        writeroutline::Project folder;
        folder.title = std::string(project["project_name"] | "Plot n Jot project");
        // An existing v1 manifest may not have chapter metadata yet. Keep
        // all its linked scenes grouped under the project until refreshed.
        std::vector<std::string> included;
        if (project["chapters"].is<JsonArray>()) {
          for (JsonObject chapter : project["chapters"].as<JsonArray>()) {
            writeroutline::Section group;
            group.title = std::string(chapter["title"] | "Untitled chapter");
            if (chapter["files"].is<JsonArray>()) {
              for (JsonVariant entry : chapter["files"].as<JsonArray>()) {
                const char* filename = entry.as<const char*>();
                if (filename && writerfiles::validName(filename) &&
                    project["scenes"][filename].is<JsonObject>() &&
                    std::find(included.begin(), included.end(), filename) == included.end()) {
                  group.files.push_back(filename);
                  included.push_back(filename);
                }
              }
            }
            if (!group.files.empty()) folder.chapters.push_back(group);
          }
        }
        writeroutline::Section other{"Other scenes", {}};
        if (project["scenes"].is<JsonObject>()) {
          for (JsonPair pair : project["scenes"].as<JsonObject>()) {
            const std::string filename = pair.key().c_str();
            if (writerfiles::validName(filename) &&
                std::find(included.begin(), included.end(), filename) == included.end())
              other.files.push_back(filename);
          }
        }
        if (!other.files.empty()) folder.chapters.push_back(other);
        if (!folder.chapters.empty()) desired.push_back(folder);
      };
      if ((doc["version"] | 0) == 2 && doc["projects"].is<JsonArray>()) {
        for (JsonObject project : doc["projects"].as<JsonArray>())
          appendProject(project);
      } else if ((doc["version"] | 0) == 1 &&
                 doc["provider"] == "plot-and-jot") {
        appendProject(doc.as<JsonObject>());
      }
    }
  }
  projectList = writeroutline::organizeProjects(fileList, desired);
  fileSections.clear();
  projectIndex = 0;
  for (size_t p = 0; p < projectList.size(); ++p) {
    for (const auto& section : projectList[p].chapters) {
      if (std::find(section.files.begin(), section.files.end(), activeName)
          != section.files.end()) {
        projectIndex = p;
        break;
      }
    }
  }
  sectionIndex = 0;
  fileIndex = 0;
  fileMenu = true;
  projectMenu = true;
  chapterMenu = false;
  renderPending = true;
}

void enterSelectedProject() {
  if (projectIndex >= projectList.size()) return;
  const auto& project = projectList[projectIndex];
  fileSections = project.chapters;
  sectionIndex = 0;
  if (project.personal) {
    fileList = fileSections.empty()
        ? std::vector<std::string>{} : fileSections.front().files;
    auto found = std::find(fileList.begin(), fileList.end(), activeName);
    fileIndex = found == fileList.end() ? 0
        : static_cast<size_t>(found - fileList.begin());
    chapterMenu = false;
  } else {
    for (size_t i = 0; i < fileSections.size(); ++i) {
      const auto& filenames = fileSections[i].files;
      if (std::find(filenames.begin(), filenames.end(), activeName) != filenames.end()) {
        sectionIndex = i;
        break;
      }
    }
    chapterMenu = true;
  }
  projectMenu = false;
  renderPending = true;
}

void enterSelectedChapter() {
  if (sectionIndex >= fileSections.size()) return;
  fileList = fileSections[sectionIndex].files;
  auto found = std::find(fileList.begin(), fileList.end(), activeName);
  fileIndex = found == fileList.end() ? 0
      : static_cast<size_t>(found - fileList.begin());
  chapterMenu = false;
  renderPending = true;
}

void leaveFileBrowserLevel() {
  if (projectMenu) {
    fileMenu = false;
    projectMenu = false;
    chapterMenu = false;
  } else if (chapterMenu) {
    projectMenu = true;
    chapterMenu = false;
  } else if (projectIndex < projectList.size() &&
             !projectList[projectIndex].personal) {
    chapterMenu = true;
  } else {
    projectMenu = true;
  }
  renderPending = true;
}

void renderFilePicker() {
  const size_t total = projectMenu ? projectList.size()
      : chapterMenu ? fileSections.size() : fileList.size();
  const size_t selected = projectMenu ? projectIndex
      : chapterMenu ? sectionIndex : fileIndex;
  prepareDisplayFrame();
  display.setCursor(kTextLeft, writergeometry::kHeaderTop);
  if (projectMenu) display.print("DOCUMENTS & PROJECTS");
  else if (chapterMenu && projectIndex < projectList.size())
    display.print(cropped(projectList[projectIndex].title, 34).c_str());
  else if (projectIndex < projectList.size() &&
           projectList[projectIndex].personal)
    display.print("MY DOCUMENTS");
  else if (sectionIndex < fileSections.size())
    display.print(cropped(fileSections[sectionIndex].title, 34).c_str());
  else display.print("FILES");
  display.drawLine(0, writergeometry::kHeaderDivider,
                   display.width() - 1, writergeometry::kHeaderDivider, BLACK);
  const size_t first = selected >= 6 ? selected - 6 : 0;
  for (size_t row = 0; row < 8 && first + row < total; ++row) {
    const size_t i = first + row;
    display.setCursor(kTextLeft, writergeometry::fileRowTop(static_cast<int>(row)));
    display.print(i == selected ? "> " : "  ");
    if (projectMenu) {
      const auto& project = projectList[i];
      display.print(cropped(project.title, 32).c_str());
    } else if (chapterMenu) {
      display.print(cropped(fileSections[i].title, 28).c_str());
      display.print(" (");
      display.print(fileSections[i].files.size());
      display.print(")");
    } else {
      display.print(cropped(fileList[i], 34).c_str());
    }
  }
  display.drawLine(0, writergeometry::footerDivider(display.height()),
                   display.width() - 1, writergeometry::footerDivider(display.height()), BLACK);
  display.setCursor(kTextLeft, writergeometry::footerTop(display.height()));
  display.print(projectMenu ? "ENTER: PROJECT  ESC: CLOSE"
                            : chapterMenu ? "ENTER: SCENES  ESC: BACK"
                                          : "ENTER: OPEN  ESC: BACK");
  display.update();
  renderPending = false;
  lastRenderAt = millis();
}

void renderNameEditor() {
  prepareDisplayFrame();
  display.setCursor(kTextLeft, writergeometry::kHeaderTop);
  display.print(nameMode == NameMode::Create ? "NEW DOCUMENT" : "RENAME DOCUMENT");
  display.drawLine(0, writergeometry::kHeaderDivider,
                   display.width() - 1, writergeometry::kHeaderDivider, BLACK);
  display.setCursor(kTextLeft, 24);
  display.print("Document title (without .txt):");
  display.setCursor(kTextLeft, 43);
  display.print(nameMode == NameMode::Create ? "New title:" : "Rename to:");
  display.setCursor(kTextLeft, 57);
  const std::string shown = nameInput.size() > 36 ? "~" + nameInput.substr(nameInput.size() - 35) : nameInput;
  display.print(shown.empty() ? "_" : shown.c_str());
  const int caretX = std::min(display.width() - 2,
                             kTextLeft + static_cast<int>(shown.size()) * writergeometry::kGlyphWidth);
  if (!shown.empty()) display.drawLine(caretX, 57, caretX, 64, BLACK);
  display.setCursor(kTextLeft, 77);
  if (!nameError.empty()) display.print(cropped(nameError, 39).c_str());
  else if (nameMode == NameMode::Create)
    display.print("Blank title = next Draft number");
  else
    display.print("Linked Plot n Jot files stay named");
  display.drawLine(0, writergeometry::footerDivider(display.height()),
                   display.width()-1, writergeometry::footerDivider(display.height()), BLACK);
  display.setCursor(kTextLeft, writergeometry::footerTop(display.height()));
  display.print("ENTER: save  ESC: cancel");
  display.update();
  lastRenderAt = millis();
  renderPending = false;
}

void renderUsbSyncScreen() {
  prepareDisplayFrame();
  display.setCursor(kTextLeft, writergeometry::kHeaderTop);
  display.print("USB SYNC // E213");
  display.drawLine(0, writergeometry::kHeaderDivider,
                   display.width() - 1, writergeometry::kHeaderDivider, BLACK);
  display.setCursor(kTextLeft, 25);
  display.print(storageReady ? "SD: READY" : "SD: NOT AVAILABLE");
  display.setCursor(kTextLeft, 40);
  display.print(serialSync.hostActive() ? "LINUX: CONNECTED" : "Waiting for Linux sync app");
  display.setCursor(kTextLeft, 60);
  display.print("Open E213 Personal Sync");
  display.setCursor(kTextLeft, 73);
  display.print("Select scene / Import to E213");
  display.setCursor(kTextLeft, 89);
  display.print("Bluetooth scanning paused");
  display.drawLine(0, writergeometry::footerDivider(display.height()),
                   display.width() - 1, writergeometry::footerDivider(display.height()), BLACK);
  display.setCursor(kTextLeft, writergeometry::footerTop(display.height()));
  display.print("HOLD B1: EXIT USB SYNC");
  display.update();
  lastRenderAt = millis();
  renderPending = false;
}

void renderWriter(const bool forceFull = false) {
  if (usbSyncMode) { renderUsbSyncScreen(); return; }
  if (nameMode != NameMode::None) { renderNameEditor(); return; }
  if (!storageReady) {
    prepareDisplayFrame(forceFull);
    display.setCursor(kTextLeft, writergeometry::kHeaderTop);
    display.println("MICRO WRITER - SD REQUIRED");
    display.drawLine(0, writergeometry::kHeaderDivider,
                     display.width() - 1, writergeometry::kHeaderDivider, BLACK);
    display.setCursor(6, 40);
    display.println("No writable SD card found.");
    display.setCursor(6, 57);
    display.println("Check SPI wiring + FAT32.");
    display.setCursor(6, 73);
    display.println("Then restart the E213.");
    display.setCursor(kTextLeft, writergeometry::footerTop(display.height()));
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

  prepareDisplayFrame(forceFull);

    if (!focusMode) {
    // Top: manuscript name left, save state right. Never let them overlap.
    const char* status = serialSync.busy() ? "USB SYNC" :
                         saveFailed ? "SAVE ERR" :
                         dirty ? "UNSAVED" : "SAVED";
    const std::string saveLabel(status);
    const int saveX = writerstats::rightAlignedX(display.width(), saveLabel);
    const size_t maxTitle = writerstats::titleColumns(saveX);
    display.setCursor(kTextLeft, writergeometry::kHeaderTop);
    display.print(cropped(activeName, maxTitle).c_str());
    display.setCursor(saveX, writergeometry::kHeaderTop);
    display.print(saveLabel.c_str());
    display.drawLine(0, writergeometry::kHeaderDivider,
                     display.width() - 1, writergeometry::kHeaderDivider, BLACK);
    }

    for (int row = 0; row < rows; ++row) {
      const size_t index = firstLine + static_cast<size_t>(row);
      if (index >= lines.size()) break;
      const int rowTop = writergeometry::bodyRowTop(row, focusMode);
      display.setCursor(kTextLeft, rowTop);
      display.print(lines[index].text.c_str());

      if (index == activeLine) {
        const int column = std::min(cursorColumn(lines[index]), kCharactersPerLine);
        const int x = std::min(display.width() - 2, kTextLeft + column * 6);
        display.drawLine(x, rowTop, x, writergeometry::caretBottom(rowTop), BLACK);
      }
    }

    if (!focusMode) {
    // Battery left, Bluetooth center, live word count right.
    display.drawLine(0, writergeometry::footerDivider(display.height()),
                     display.width() - 1, writergeometry::footerDivider(display.height()), BLACK);
    const int footerTop = writergeometry::footerTop(display.height());
    display.setCursor(kTextLeft, footerTop);
    display.print(writerbattery::displayLabel(batteryMillivolts).c_str());
    const std::string btLabel = keyboard.isConnected() ? "BT:OK" : "BT:--";
    display.setCursor(writerstats::centeredX(display.width(), btLabel), footerTop);
    display.print(btLabel.c_str());
    const std::string wordLabel = std::to_string(writerstats::countWords(document)) + " words";
    display.setCursor(writerstats::rightAlignedX(display.width(), wordLabel), footerTop);
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

// Sleep only after pending work has been safely committed to SD; RESET wakes.
void saveAndSleep() {
  if (!storageReady || serialSync.busy()) {
    Serial.println("Sleep refused: storage unavailable or USB transfer active.");
    renderPending = true;
    return;
  }
  if (dirty && !saveDraft()) {
    Serial.println("Sleep refused: manuscript save failed.");
    return;
  }
  if (!display.blankForSleep() || !keyboard.end()) {
    Serial.println("Sleep refused: display blanking or Bluetooth shutdown failed.");
    renderPending = true;
    return;
  }
  Serial.println("E213: saved, display blank, entering deep sleep; RESET wakes.");
  Serial.flush();
  display.sleepUntilReset();
}

void beginNameEditor(NameMode mode) {
  if (!storageReady || serialSync.busy()) return;
  if (dirty && !saveDraft()) {
    Serial.println("Cannot edit title while document save is failing");
    return;
  }
  renameSource = (mode == NameMode::Rename && fileMenu && !chapterMenu && !projectMenu &&
                  fileIndex < fileList.size())
      ? fileList[fileIndex] : activeName;
  nameInput = mode == NameMode::Rename ? writernames::stem(renameSource) : "";
  nameError.clear();
  nameMode = mode;
  renderPending = true;
  lastKeyAt = millis();
  renderNameEditor();
}

void confirmNameEditor() {
  if (nameMode == NameMode::None || serialSync.busy()) return;
  std::string newName = nameMode == NameMode::Create && nameInput.empty()
      ? writerfiles::nextName() : writernames::filename(nameInput);
  if (newName.empty()) {
    nameError = "Title: 1-44 letters/numbers/-/_";
    return;
  }
  if (nameMode == NameMode::Rename && newName == renameSource) {
    nameMode = NameMode::None; nameError.clear(); return;
  }
  for (const auto& existing : writerfiles::list()) {
    if (writernames::nameTaken(existing, newName)) {
      nameError = "A document already uses that title";
      return;
    }
  }
  if (nameMode == NameMode::Create) {
    if (!writerfiles::save(newName, "") || !switchDraft(newName)) {
      nameError = "Could not create document on SD";
      return;
    }
  } else {
    // Existing Plot n Jot .sync links use the exact filename. Do not silently
    // break a scene link or modify a remote manuscript by renaming here.
    std::string manifestText;
    if (writerfiles::readManifest(manifestText)) {
      const auto parsed = e213::sync::parseManifest(manifestText);
      if (!parsed.isManaged()) {
        nameError = "Cannot rename: .sync unreadable";
        return;
      }
      for (const auto& binding : parsed.manifest.scenes) {
        if (binding.filename == renameSource) {
          nameError = "Linked: unlink in sync app first";
          return;
        }
      }
    }
    if (!writerfiles::renameDraft(renameSource, newName)) {
      nameError = "Rename failed: SD file preserved";
      return;
    }
    if (renameSource == activeName) {
      activeName = newName;
      if (!writerfiles::saveActiveName(newName))
        Serial.println("Renamed draft; active preference could not be saved");
    }
    if (fileMenu) openFileBrowser();  // File now belongs to Local drafts if unlinked.
  }
  Serial.printf("SD manuscript title saved: %s\n", newName.c_str());
  nameMode = NameMode::None;
  nameInput.clear();
  nameError.clear();
  renderPending = true;
  lastKeyAt = millis();
}

void handleNameKey(const BleWriterKeyboard::KeyEvent& event) {
  switch (event.type) {
    case BleWriterKeyboard::KeyType::Escape:
      nameMode = NameMode::None; nameError.clear(); break;
    case BleWriterKeyboard::KeyType::Enter:
      confirmNameEditor(); break;
    case BleWriterKeyboard::KeyType::Backspace:
      if (!nameInput.empty()) nameInput.pop_back();
      nameError.clear(); break;
    case BleWriterKeyboard::KeyType::Character: {
      const char c = event.character;
      if (nameInput.size() < 44 &&
          ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ')) {
        nameInput.push_back(c);
        nameError.clear();
      } else {
        nameError = "Use letters, numbers, space, - or _";
      }
      break;
    }
    default: break;
  }
  renderPending = true;
  lastKeyAt = millis();
}

void enterUsbSyncMode(bool manual) {
  if (!storageReady) {
    Serial.println("USB sync cannot start: SD storage unavailable.");
    return;
  }
  if (dirty && !saveDraft()) {
    Serial.println("USB sync cannot start: draft could not be saved.");
    return;
  }
  manualUsbSyncMode = manualUsbSyncMode || manual;
  if (usbSyncMode) return;
  usbSyncMode = true;
  fileMenu = false;
  nameMode = NameMode::None;
  keyboard.pauseDiscoveryForUsb();
  Serial.println("E213 USB SYNC READY: connect Linux personal sync app.");
  renderUsbSyncScreen();
}

void exitUsbSyncMode() {
  if (serialSync.busy()) {
    Serial.println("USB transfer in progress; cannot exit sync mode.");
    return;
  }
  usbSyncMode = false;
  manualUsbSyncMode = false;
  serialSync.releaseHostSession();
  keyboard.resumeDiscoveryAfterUsb();
  Serial.println("E213 USB SYNC EXIT: keyboard scanning resumes.");
  if (keyboard.isConnected()) renderWriter(true);
  else renderPairing(true);
}

void handleKeyboardEvents() {
  BleWriterKeyboard::KeyEvent event;
  while (keyboard.popEvent(event)) {
    // An absent or failed SD never turns the editor into unsafe RAM-only mode.
    if (!storageReady) continue;
    if (event.type == BleWriterKeyboard::KeyType::Sleep) {
      if (nameMode != NameMode::None) { nameMode = NameMode::None; nameError.clear(); }
      saveAndSleep();
      continue;
    }
    if (nameMode != NameMode::None) {
      handleNameKey(event);
      continue;
    }
    if (event.type == BleWriterKeyboard::KeyType::RenameFile) {
      if (!fileMenu || (!chapterMenu && !projectMenu)) beginNameEditor(NameMode::Rename);
      continue;
    }
    if (event.type == BleWriterKeyboard::KeyType::OpenFiles) {
      if (!fileMenu && dirty && !saveDraft()) {
        Serial.println("Cannot open file picker: autosave failed");
        continue;
      }
      if (fileMenu) { fileMenu = false; chapterMenu = false; projectMenu = false; }
      else openFileBrowser();
      renderPending = true;
      lastKeyAt = millis();
      continue;
    }
    if (event.type == BleWriterKeyboard::KeyType::NewFile) {
      beginNameEditor(NameMode::Create);
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
          if (projectMenu) {
            if (projectIndex > 0) --projectIndex;
          } else if (chapterMenu) {
            if (sectionIndex > 0) --sectionIndex;
          } else if (fileIndex > 0) --fileIndex;
          break;
        case BleWriterKeyboard::KeyType::Down:
          if (projectMenu) {
            if (projectIndex + 1 < projectList.size()) ++projectIndex;
          } else if (chapterMenu) {
            if (sectionIndex + 1 < fileSections.size()) ++sectionIndex;
          } else if (fileIndex + 1 < fileList.size()) ++fileIndex;
          break;
        case BleWriterKeyboard::KeyType::Enter:
          if (projectMenu) enterSelectedProject();
          else if (chapterMenu) enterSelectedChapter();
          else if (fileIndex < fileList.size() && !switchDraft(fileList[fileIndex]))
            Serial.println("Open failed; current draft preserved");
          break;
        case BleWriterKeyboard::KeyType::Escape:
          leaveFileBrowserLevel();
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

void handleButtons() {
  const ButtonEvent next = nextButton.poll();
  const ButtonEvent select = selectButton.poll();
  if (usbSyncMode) {
    if (next == ButtonEvent::Long) exitUsbSyncMode();
    // Never put the E213 to sleep in the middle of a USB sync session.
    return;
  }
  if (next == ButtonEvent::Long) {
    if (fileMenu) {
      leaveFileBrowserLevel();
      if (fileMenu) renderFilePicker();
      else if (keyboard.isConnected()) renderWriter();
      else renderPairing();
    } else {
      enterUsbSyncMode(true); // B1 held 1.2 s enters USB transfer mode.
    }
    return;
  }
  if (select == ButtonEvent::Long) {
    if (nameMode != NameMode::None) { nameMode = NameMode::None; nameError.clear(); }
    saveAndSleep();
    return;
  }
  if (next == ButtonEvent::None && select == ButtonEvent::None) return;
  if (nameMode != NameMode::None) return; // Naming requires the BLE keyboard.

  if (fileMenu) {
    if (next == ButtonEvent::Long) {
      leaveFileBrowserLevel();
    } else if (next == ButtonEvent::Short) {
      if (projectMenu && !projectList.empty()) {
        projectIndex = (projectIndex + 1) % projectList.size();
      } else if (chapterMenu && !fileSections.empty()) {
        sectionIndex = (sectionIndex + 1) % fileSections.size();
      } else if (!fileList.empty()) {
        fileIndex = (fileIndex + 1) % fileList.size();
      }
    } else if (select == ButtonEvent::Short) {
      if (projectMenu) enterSelectedProject();
      else if (chapterMenu) enterSelectedChapter();
      else if (fileIndex < fileList.size() && !switchDraft(fileList[fileIndex]))
        Serial.println("Cannot open file from button: save/read failed");
    }
    renderPending = true;
    lastKeyAt = millis();
    return;
  }

  if (keyboard.isConnected()) {
    if (next == ButtonEvent::Short || next == ButtonEvent::Long) {
      if (dirty && !saveDraft()) return;
      openFileBrowser();
      renderPending = true;
    } else if (select == ButtonEvent::Short && !saveDraft()) {
      Serial.println("B2 save failed");
    }
    return;
  }

  // No keyboard: wired B1/B2 select and pair keyboards.
  // A long B1 press enters USB sync even without a Bluetooth keyboard.
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
    if (keyboard.isConnected()) renderWriter(true);
    else renderPairing(true);
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

  Serial.println("Initializing E213 LCMEN2R13EFC1 (FC1) display...");
  display.begin();
  display.landscape();
  display.clear();
  Serial.printf("External buttons B1 GPIO42=%d B2 GPIO47=%d (idle 1)\n",
                digitalRead(kNextButton), digitalRead(kSelectButton));
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
    // Don't hide a genuine low-battery warning behind distraction-free mode.
    if (batteryMillivolts > 0 && batteryMillivolts < 3450) focusMode = false;
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
  // A HELLO from Linux automatically enters USB sync mode without a pairing
  // step. In this mode reconnect attempts cannot monopolize the serial loop.
  if (serialSync.hostActive() && !usbSyncMode) enterUsbSyncMode(false);
  if (usbSyncMode && !manualUsbSyncMode && !serialSync.hostActive() &&
      !serialSync.busy()) exitUsbSyncMode();
  const bool syncBusy = serialSync.busy();
  if (syncBusy != syncWasBusy) {
    syncWasBusy = syncBusy;
    renderPending = true;
  }
  // A synchronous reconnect must not block while a wired button is being held.
  if (!usbSyncMode &&
      digitalRead(kNextButton) != LOW && digitalRead(kSelectButton) != LOW)
    keyboard.update();
  handleButtons();

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
    if (usbSyncMode) { delay(5); return; }
    if (!keyboard.isConnected() && millis() - lastPairingRenderAt >= kPairingRefreshMs) {
      renderPairing();
    } else if (keyboard.isConnected()) {
      renderPending = true;
    }
  }

  delay(5);
}
