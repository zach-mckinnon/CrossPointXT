#pragma once

// Only HalStorage may use this backend. Every call after begin is covered by
// HalStorage's recursive mutex, including FsFile destruction.
#include <SdFat.h>
#include <SPI.h>
#include <Logging.h>

class HeltecStorageBackend : public SdFat {
 public:
  static HeltecStorageBackend& instance() {
    static HeltecStorageBackend card;
    return card;
  }
  bool begin() {
    if (mounted) return true;
    bus.begin(40, 41, 39, 38);
    // Display owns HSPI. The soldered SD wiring gets an independent FSPI bus.
    mounted = SdFat::begin(SdSpiConfig(38, SHARED_SPI, SD_SCK_MHZ(1), &bus));
    if (!mounted) LOG_ERR("SD", "E213 card mount failed");
    else LOG_INF("SD", "E213 persistent storage mounted on FSPI");
    return mounted;
  }
  bool ready() const { return mounted; }
  bool openFileForRead(const char* module, const char* path, FsFile& out) {
    out = open(path, O_RDONLY);
    if (!out) LOG_ERR(module, "Cannot read %s", path);
    return bool(out);
  }
  bool openFileForWrite(const char* module, const char* path, FsFile& out) {
    out = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!out) LOG_ERR(module, "Cannot write %s", path);
    return bool(out);
  }
  bool ensureDirectoryExists(const char* path) { return exists(path) || mkdir(path, true); }
  std::vector<String> listFiles(const char* path, int maxFiles) {
    std::vector<String> result;
    if (maxFiles <= 0) return result;
    result.reserve(maxFiles);
    auto dir = open(path);
    if (!dir || !dir.isDirectory()) return result;
    char name[128];
    while (static_cast<int>(result.size()) < maxFiles) {
      auto file = dir.openNextFile();
      if (!file) break;
      if (file.getName(name, sizeof(name))) result.emplace_back(name);
    }
    return result;
  }
  size_t readFileToBuffer(const char* path, char* buffer, size_t size, size_t maxBytes) {
    if (!buffer || !size) return 0;
    auto file = open(path);
    const auto count = file ? file.read(buffer, maxBytes ? std::min(size - 1, maxBytes) : size - 1) : -1;
    const size_t read = count > 0 ? count : 0;
    buffer[read] = 0;
    return read;
  }
  bool readFileToStream(const char* path, Print& out, size_t) {
    auto file = open(path);
    if (!file) return false;
    uint8_t buffer[128];
    int count;
    while ((count = file.read(buffer, sizeof(buffer))) > 0) {
      if (out.write(buffer, count) != static_cast<size_t>(count)) return false;
    }
    return count == 0;
  }
  String readFile(const char* path) {
    String result;
    auto file = open(path);
    if (!file || !result.reserve(file.size())) return result;
    int c;
    while ((c = file.read()) >= 0) result += static_cast<char>(c);
    return result;
  }
  bool writeFile(const char* path, const String& text) {
    auto file = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    return file && file.write(text.c_str(), text.length()) == text.length() && file.sync();
  }
  bool removeDir(const char* path) {
    // The writer never recursively deletes folders; refuse nonempty ones.
    return rmdir(path);
  }
 private:
  SPIClass bus{FSPI};
  bool mounted = false;
};
