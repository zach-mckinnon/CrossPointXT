#include "WriterStore.h"
#include <HalStorage.h>
#include <LittleFS.h>
#include <Logging.h>
#include <esp_random.h>

namespace {
constexpr char SESSION[] = "/writer-session.dat";
constexpr char TEMP[] = "/writer-session.tmp";
constexpr char MAGIC[] = "E213-SESSION-1";
bool readLine(File& file, char* out, size_t capacity) {
  size_t n = 0;
  int c;
  while ((c = file.read()) >= 0) {
    if (c == '\n') { out[n] = 0; return true; }
    if (n + 1 >= capacity) return false;
    out[n++] = c;
  }
  return false;
}
bool safePath(const char* path) {
  return path && path[0] == '/' && strlen(path) < WriterStore::PATH_SIZE &&
         !strstr(path, "..") && !strchr(path, '\n') && !strchr(path, '\r') && !strchr(path, '\\');
}
bool matches(File& file, const char* expected, size_t size) {
  char chunk[64];
  while (size) {
    const size_t count = std::min(size, sizeof(chunk));
    if (file.readBytes(chunk, count) != count || memcmp(chunk, expected, count)) return false;
    expected += count;
    size -= count;
  }
  return true;
}
}

const char* WriterStore::name() const {
  const char* slash = strrchr(activePath, '/');
  return slash ? slash + 1 : activePath;
}

bool WriterStore::begin() {
  flashReady = LittleFS.begin(false);
  cardReady = Storage.begin();
  if (!flashReady) { LOG_ERR("E213", "Internal recovery storage unavailable"); return false; }
  if (LittleFS.exists(SESSION)) {
    if (!loadCheckpoint()) { LOG_ERR("E213", "Invalid recovery session; refusing to overwrite it"); flashReady = false; }
    // A clean session may have been updated by the PC while powered off.
    // Pending device edits always retain priority in the recovery checkpoint.
    if (flashReady && cardReady && !pending && activePath[0] && readCard(activePath, staging)) {
      const uint32_t hash = writerHash(staging.data(), staging.size());
      if (checkpoint(staging, activePath, false, true, hash)) {
        text.assign(staging.data(), staging.size());
        baseHash = hash;
        hasBase = true;
      }
    }
    return flashReady;
  }
  // Preserve the original bring-up draft; migration never deletes it.
  auto old = LittleFS.open("/draft.txt", FILE_READ);
  if (old) {
    const size_t size = old.size();
    if (size > WriterText::CAPACITY || old.readBytes(staging.bytes, size) != size ||
        !validWriterText(staging.bytes, size)) {
      LOG_ERR("E213", "Legacy draft cannot be loaded safely");
      flashReady = false;
      return false;
    }
    text.assign(staging.bytes, size);
  }
  changed = true;
  return true;
}

bool WriterStore::checkpoint(const WriterText& value, const char* path, bool needsSD, bool base, uint32_t hash) {
  if (!flashReady) return false;
  char header[96];
  const int n = snprintf(header, sizeof(header), "%s\n%08lx %08lx %u %u\n", MAGIC,
      static_cast<unsigned long>(writerHash(value.data(), value.size())), static_cast<unsigned long>(hash), needsSD, base);
  {
    auto file = LittleFS.open(TEMP, FILE_WRITE);
    if (!file) return false;
    if (file.write(reinterpret_cast<const uint8_t*>(header), n) != static_cast<size_t>(n) ||
        file.print(path) != strlen(path) || file.write('\n') != 1 ||
        file.write(reinterpret_cast<const uint8_t*>(value.data()), value.size()) != value.size()) return false;
    file.flush();
  }
  {
    auto check = LittleFS.open(TEMP, FILE_READ);
    if (!check || check.size() != n + strlen(path) + 1 + value.size() ||
        !matches(check, header, n) || !matches(check, path, strlen(path)) ||
        check.read() != '\n' || !matches(check, value.data(), value.size())) return false;
  }
  // LittleFS rename is atomic. A failed write leaves the previous session intact.
  return LittleFS.rename(TEMP, SESSION);
}

bool WriterStore::loadCheckpoint() {
  auto file = LittleFS.open(SESSION, FILE_READ);
  char header[96];
  unsigned long hash = 0, base = 0;
  unsigned needsSD = 0, knownBase = 0;
  if (!file || !readLine(file, header, sizeof(header)) || strcmp(header, MAGIC) ||
      !readLine(file, header, sizeof(header)) || sscanf(header, "%lx %lx %u %u", &hash, &base, &needsSD, &knownBase) != 4 ||
      needsSD > 1 || knownBase > 1 || !readLine(file, candidate, sizeof(candidate)) ||
      (candidate[0] && !safePath(candidate))) return false;
  const size_t size = file.size() - file.position();
  if (size > WriterText::CAPACITY || file.readBytes(staging.bytes, size) != size ||
      writerHash(staging.bytes, size) != hash || !validWriterText(staging.bytes, size)) return false;
  text.assign(staging.bytes, size);
  strcpy(activePath, candidate);
  pending = needsSD;
  hasBase = knownBase;
  baseHash = base;
  return true;
}

bool WriterStore::readCard(const char* path, WriterText& value) {
  auto file = Storage.open(path);
  if (!file || file.isDirectory() || file.size() > WriterText::CAPACITY) return false;
  const size_t size = file.size();
  if (file.read(value.bytes, size) != static_cast<int>(size) || !validWriterText(value.bytes, size)) return false;
  value.length = size;
  value.bytes[size] = 0;
  return true;
}

bool WriterStore::newPath() {
  if (!cardReady || !Storage.ensureDirectoryExists("/Writer")) return false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    snprintf(candidate, sizeof(candidate), "/Writer/draft-%08lx.txt", static_cast<unsigned long>(esp_random()));
    if (!Storage.exists(candidate)) return true;
  }
  return false;
}

bool WriterStore::saveCard() {
  snprintf(temporary, sizeof(temporary), "%s.e213-tmp", activePath);
  snprintf(backup, sizeof(backup), "%s.e213-bak", activePath);
  // Recover interruption between the two FAT renames. The old file is never
  // removed until the replacement has been fully written, synced and verified.
  if (!Storage.exists(activePath) && Storage.exists(backup) && !Storage.rename(backup, activePath)) return false;
  const bool exists = Storage.exists(activePath);
  if (exists) {
    if (!readCard(activePath, staging)) return false;
    if (staging.size() == text.size() && !memcmp(staging.data(), text.data(), text.size())) return true;
    if (!hasBase || writerHash(staging.data(), staging.size()) != baseHash) {
      LOG_ERR("SD", "File changed outside writer; preserving both SD and recovery draft: %s", activePath);
      return false;
    }
  } else if (hasBase) {
    LOG_ERR("SD", "Previously opened file missing; refusing to recreate on another card");
    return false;
  }
  {
    auto file = Storage.open(temporary, O_WRONLY | O_CREAT | O_TRUNC);
    if (!file || file.write(text.data(), text.size()) != text.size() || !file.sync()) return false;
  }
  if (!readCard(temporary, staging) || staging.size() != text.size() || memcmp(staging.data(), text.data(), text.size())) return false;
  if (exists) {
    if (Storage.exists(backup) && !Storage.remove(backup)) return false;
    if (!Storage.rename(activePath, backup)) return false;
  }
  if (!Storage.rename(temporary, activePath)) {
    if (exists) Storage.rename(backup, activePath);
    return false;
  }
  return true;  // Keep previous version in .e213-bak for recovery.
}

bool WriterStore::save() {
  if (!flashReady) return false;
  if (!activePath[0] && cardReady) {
    if (!newPath()) return false;
    strcpy(activePath, candidate);
    hasBase = false;
    changed = pending = true;
  }
  if (changed || pending) {
    if (!checkpoint(text, activePath, activePath[0] && pending, hasBase, baseHash)) return false;
    changed = false;
    if (!activePath[0]) pending = false;
  }
  if (!pending || !activePath[0]) return true;
  if (!cardReady || !saveCard()) return false;
  const uint32_t hash = writerHash(text.data(), text.size());
  if (!checkpoint(text, activePath, false, true, hash)) return false;
  pending = false;
  hasBase = true;
  baseHash = hash;
  return true;
}

bool WriterStore::open(const char* path) {
  if (!safePath(path) || !cardReady || !save()) return false;
  // An interrupted save of any browsed file can be recovered from its backup.
  snprintf(backup, sizeof(backup), "%s.e213-bak", path);
  if (!Storage.exists(path) && Storage.exists(backup) && !Storage.rename(backup, path)) return false;
  if (!readCard(path, staging)) return false;
  const uint32_t hash = writerHash(staging.data(), staging.size());
  if (!checkpoint(staging, path, false, true, hash)) return false;
  text.assign(staging.data(), staging.size());
  strcpy(activePath, path);
  baseHash = hash;
  hasBase = true;
  changed = pending = false;
  return true;
}

bool WriterStore::create() {
  if (!save() || !newPath()) return false;
  staging.assign("", 0);
  if (!checkpoint(staging, candidate, true, false, 0)) return false;
  text.assign("", 0);
  strcpy(activePath, candidate);
  hasBase = false;
  pending = true;
  changed = false;
  return true;  // The caller saves to SD; the new draft already has recovery.
}

void WriterStore::shutdown() { LittleFS.end(); }
