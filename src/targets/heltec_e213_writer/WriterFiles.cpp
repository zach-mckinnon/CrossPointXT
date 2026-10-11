#include "WriterFiles.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <LittleFS.h> // Read-only access during the optional one-time legacy import.

#include <algorithm>
#include <cstdio>

namespace writerfiles {
namespace {
// Proposed external-SPI wiring on Heltec Vision Master E213 V1.1.
// These are NOT the built-in E213 e-ink SPI pins and still require a wiring test.
// SD VCC must use 3.3V only when the module is rated for it.
constexpr int kSdCS = 38;
constexpr int kSdMOSI = 39;
constexpr int kSdSCK = 40;
constexpr int kSdMISO = 41;
constexpr uint32_t kSdClockHz = 4000000;
constexpr char kDraftDir[] = "/drafts";
constexpr char kImportMarker[] = "/.flash-imported";

SPIClass sdSpi(FSPI);
bool mounted = false;
bool migrationBlocked = false; // Block USB on an incomplete legacy import.

std::string pathFor(const std::string& name) { return std::string(kDraftDir) + "/" + name; }

// Write to the SD temp file first and keep the last version on the SD as .bak.
// Internal flash is never the destination of a manuscript or sync metadata.
bool atomicWrite(const std::string& path, const std::string& content) {
  if (!mounted) return false;
  const std::string temp = path + ".tmp";
  const std::string backup = path + ".bak";
  SD.remove(temp.c_str());
  File file = SD.open(temp.c_str(), "w");
  if (!file) return false;
  const size_t written = file.write(
      reinterpret_cast<const uint8_t*>(content.data()), content.size());
  file.flush();
  file.close();
  if (written != content.size()) {
    SD.remove(temp.c_str());
    return false;
  }
  if (SD.exists(path.c_str())) {
    // Renaming the original creates a recovery point before the new file appears.
    SD.remove(backup.c_str());
    if (!SD.rename(path.c_str(), backup.c_str())) return false;
  }
  if (!SD.rename(temp.c_str(), path.c_str())) {
    if (SD.exists(backup.c_str()) && !SD.exists(path.c_str()))
      SD.rename(backup.c_str(), path.c_str());
    return false;
  }
  return true;
}

bool readPath(const std::string& path, std::string& content, size_t limit) {
  if (!mounted) return false;
  std::string actual = path;
  if (!SD.exists(actual.c_str()) && SD.exists((path + ".bak").c_str()))
    actual += ".bak"; // Recover an interrupted rename without deleting the backup.
  File file = SD.open(actual.c_str(), FILE_READ);
  if (!file || file.isDirectory() || static_cast<size_t>(file.size()) > limit)
    return false;
  std::string next;
  next.reserve(file.size());
  while (file.available()) next.push_back(static_cast<char>(file.read()));
  const bool complete = next.size() == static_cast<size_t>(file.size());
  file.close();
  if (!complete) return false;
  content.swap(next);
  return true;
}

bool readLegacy(const std::string& path, std::string& data, size_t limit) {
  File file = LittleFS.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory() || static_cast<size_t>(file.size()) > limit)
    return false;
  std::string copy;
  copy.reserve(file.size());
  while (file.available()) copy.push_back(static_cast<char>(file.read()));
  const bool complete = copy.size() == static_cast<size_t>(file.size());
  file.close();
  if (complete) data.swap(copy);
  return complete;
}

// Import, never replace an existing SD version. A differently edited SD document
// is protected by putting the older flash version in a Recovered-NNN draft.
bool copyLegacyDraft(const std::string& requestedName, const std::string& bytes) {
  if (!validName(requestedName)) return false;
  if (!exists(requestedName)) return save(requestedName, bytes);
  std::string current;
  if (read(requestedName, current) && current == bytes) return true;
  for (int n = 1; n <= 999; ++n) {
    char candidate[24];
    snprintf(candidate, sizeof(candidate), "Recovered-%03d.txt", n);
    if (exists(candidate)) {
      if (read(candidate, current) && current == bytes) return true;
      continue;
    }
    return save(candidate, bytes);
  }
  return false;
}
}  // namespace

bool begin() {
  if (mounted) return true;
  sdSpi.begin(kSdSCK, kSdMISO, kSdMOSI, kSdCS);
  if (!SD.begin(kSdCS, sdSpi, kSdClockHz)) {
    Serial.println("SD: mount failed. Check FAT32 card and external SPI wiring.");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("SD: no microSD card detected.");
    SD.end();
    return false;
  }
  mounted = true;
  if (!SD.exists(kDraftDir) && !SD.mkdir(kDraftDir)) {
    Serial.println("SD: could not create /drafts");
    mounted = false;
    SD.end();
    return false;
  }
  Serial.printf("SD: mounted card, size=%llu bytes\n",
      static_cast<unsigned long long>(SD.cardSize()));
  return true;
}

bool ready() { return mounted && !migrationBlocked; }

bool validName(const std::string& name) {
  if (name.size() < 5 || name.size() > 48 ||
      name.substr(name.size() - 4) != ".txt")
    return false;
  for (size_t i = 0; i < name.size() - 4; ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-'))
      return false;
  }
  return true;
}

std::vector<std::string> list() {
  std::vector<std::string> names;
  if (!mounted) return names;
  File folder = SD.open(kDraftDir);
  if (!folder || !folder.isDirectory()) return names;
  File file = folder.openNextFile();
  while (file && names.size() < kMaxDocuments) {
    std::string path = file.name();
    const size_t slash = path.find_last_of('/');
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (!file.isDirectory()) {
      if (validName(name)) names.push_back(name);
      else if (name.size() > 4 && name.substr(name.size() - 4) == ".bak") {
        const std::string original = name.substr(0, name.size() - 4);
        if (validName(original) && !SD.exists(pathFor(original).c_str()))
          names.push_back(original);
      }
    }
    file = folder.openNextFile();
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

std::string nextName() {
  if (list().size() >= kMaxDocuments) return "";
  for (int i = 1; i <= 999; ++i) {
    char name[20];
    snprintf(name, sizeof(name), "Draft-%03d.txt", i);
    if (!exists(name)) return name;
  }
  return "";
}

bool exists(const std::string& name) {
  return mounted && validName(name) &&
      (SD.exists(pathFor(name).c_str()) ||
       SD.exists((pathFor(name) + ".bak").c_str()));
}
bool read(const std::string& name, std::string& content) {
  return validName(name) && readPath(pathFor(name), content, kMaxDocumentBytes);
}
bool save(const std::string& name, const std::string& content) {
  if (!validName(name) || content.size() > kMaxDocumentBytes) return false;
  return atomicWrite(pathFor(name), content);
}
bool readActiveName(std::string& name) {
  return readPath("/.active", name, 48) && validName(name);
}
bool saveActiveName(const std::string& name) {
  return validName(name) && atomicWrite("/.active", name);
}
bool readManifest(std::string& content) {
  return readPath("/.sync", content, 16384);
}
bool saveManifest(const std::string& content) {
  return content.size() <= 16384 && atomicWrite("/.sync", content);
}

bool importLegacyFlashDrafts() {
  if (!mounted) return false;
  if (SD.exists(kImportMarker)) return true;
  // Never format internal flash. If it is inaccessible, SD-only starts safely.
  if (!LittleFS.begin(false)) {
    Serial.println("Legacy internal drafts not mounted; left untouched.");
    return true;
  }

  bool ok = true;
  File folder = LittleFS.open("/drafts");
  if (folder && folder.isDirectory()) {
    File old = folder.openNextFile();
    while (old) {
      const std::string path = old.name();
      const size_t slash = path.find_last_of('/');
      const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
      if (!old.isDirectory() && validName(name)) {
        std::string contents;
        if (!readLegacy(path, contents, kMaxDocumentBytes) ||
            !copyLegacyDraft(name, contents)) {
          Serial.printf("Legacy import failed for %s; flash copy preserved.\n", name.c_str());
          ok = false;
        }
      }
      old = folder.openNextFile();
    }
  }
  // Earlier single-document prototype used /draft.txt rather than /drafts/.
  if (LittleFS.exists("/draft.txt")) {
    std::string legacy;
    if (!readLegacy("/draft.txt", legacy, kMaxDocumentBytes) ||
        !copyLegacyDraft("Draft-001.txt", legacy)) {
      Serial.println("Legacy /draft.txt import failed; flash copy preserved.");
      ok = false;
    }
  }
  if (!SD.exists("/.sync") && LittleFS.exists("/.sync")) {
    std::string manifest;
    if (!readLegacy("/.sync", manifest, 16384) || !saveManifest(manifest)) ok = false;
  }
  if (!SD.exists("/.active") && LittleFS.exists("/.active")) {
    std::string active;
    if (!readLegacy("/.active", active, 48) || !saveActiveName(active)) ok = false;
  }
  LittleFS.end();
  // A completed import marker prevents rereading stale flash on each reboot.
  // Failure is non-destructive: the original flash remains available for recovery.
  if (ok && !atomicWrite(kImportMarker, "imported\n")) ok = false;
  migrationBlocked = !ok;
  return ok;
}
