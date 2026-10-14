#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace writerfiles {
// Files live on removable microSD, not the E213's internal flash.
// A single file is RAM-capped for safe editing on ESP32-S3; card capacity is separate.
constexpr size_t kMaxDocumentBytes = 128 * 1024;
constexpr size_t kMaxDocuments = 200;

// Hardware: external SPI microSD module. Proposed E213 V1.1 GPIOs; validate wiring.
bool begin();
bool ready();
// Read FAT volume capacity and allocation, including backup/temp files.
// These are filesystem usable bytes, not the SD card's marketed raw capacity.
bool storageUsage(uint64_t& totalBytes, uint64_t& usedBytes);
bool importLegacyFlashDrafts(); // One-time, copy-only import; never erases flash.
bool validName(const std::string& name);
std::vector<std::string> list();
std::string nextName();
bool exists(const std::string& name);
bool read(const std::string& name, std::string& content);
bool save(const std::string& name, const std::string& content);
bool renameDraft(const std::string& previous, const std::string& replacement);
bool readActiveName(std::string& name);
bool saveActiveName(const std::string& name);
bool readManifest(std::string& content);
bool saveManifest(const std::string& content);
// Diagnostic reason for the most recent failed SD metadata write.
const char* lastStorageError();
}