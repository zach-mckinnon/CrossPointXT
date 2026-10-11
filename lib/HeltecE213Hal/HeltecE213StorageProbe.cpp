#include "HeltecE213StorageProbe.h"

#include <SD.h>
#include <SPI.h>
#include <Logging.h>
#include <esp_random.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace {
constexpr uint8_t SD_CS = 38;
constexpr uint8_t SD_MOSI = 39;
constexpr uint8_t SD_CLK = 40;
constexpr uint8_t SD_MISO = 41;
constexpr uint32_t SD_CLOCK_HZ = 1'000'000;
constexpr size_t TEST_BYTES = 1024;
constexpr size_t CHUNK_BYTES = 64;

// The verified FC1 display library owns HSPI. Reusing that controller here
// would silently remap its pins. LoRa is unused by this writer.
SPIClass sdBus(FSPI);

uint8_t testByte(const size_t offset) {
  return static_cast<uint8_t>((offset * 37U) ^ (offset >> 8U) ^ 0xA5U);
}

bool writeAndVerify(const int writeFd, const char* path) {
  // One bounded stack buffer; no application heap allocation or draft copy.
  uint8_t chunk[CHUNK_BYTES];
  bool written = true;
  for (size_t offset = 0; offset < TEST_BYTES; offset += sizeof(chunk)) {
    for (size_t i = 0; i < sizeof(chunk); ++i) chunk[i] = testByte(offset + i);
    if (::write(writeFd, chunk, sizeof(chunk)) != static_cast<ssize_t>(sizeof(chunk))) {
      written = false;
      break;
    }
  }
  if (::fsync(writeFd) != 0) written = false;
  if (::close(writeFd) != 0) written = false;
  if (!written) {
    LOG_ERR("E213-SD", "Test write/flush/close failed (errno=%d)", errno);
    return false;
  }

  const int readFd = ::open(path, O_RDONLY);
  if (readFd < 0) {
    LOG_ERR("E213-SD", "Cannot reopen test file (errno=%d)", errno);
    return false;
  }
  bool verified = true;
  for (size_t offset = 0; offset < TEST_BYTES && verified; offset += sizeof(chunk)) {
    if (::read(readFd, chunk, sizeof(chunk)) != static_cast<ssize_t>(sizeof(chunk))) {
      verified = false;
      break;
    }
    for (size_t i = 0; i < sizeof(chunk); ++i) {
      if (chunk[i] != testByte(offset + i)) {
        verified = false;
        break;
      }
    }
  }
  if (verified && ::read(readFd, chunk, 1) != 0) verified = false;
  if (::close(readFd) != 0) verified = false;
  if (!verified) LOG_ERR("E213-SD", "Test readback mismatch or read/close failure");
  return verified;
}

bool testMountedCard() {
  char path[40];
  int fd = -1;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    snprintf(path, sizeof(path), "/sd/.e213-test-%08lx.tmp", static_cast<unsigned long>(esp_random()));
    // Exclusive creation guarantees that a pre-existing file is never replaced.
    fd = ::open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd >= 0) break;
    if (errno != EEXIST) {
      LOG_ERR("E213-SD", "Cannot create test file (errno=%d)", errno);
      return false;
    }
  }
  if (fd < 0) {
    LOG_ERR("E213-SD", "No unused test filename available");
    return false;
  }

  const bool verified = writeAndVerify(fd, path);
  // Only remove the file successfully created by this invocation.
  if (::unlink(path) != 0) {
    LOG_ERR("E213-SD", "Test cleanup failed: %s (errno=%d)", path, errno);
    return false;
  }
  return verified;
}
}  // namespace

bool HeltecE213StorageProbe::run() {
  LOG_INF("E213-SD", "Checking CS=38 MOSI=39 CLK=40 MISO=41 at 1 MHz; no formatting");
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  if (!sdBus.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS)) {
    LOG_ERR("E213-SD", "SPI initialization failed");
    return false;
  }

  // SDK mount/file bookkeeping is allocated only for this startup check and
  // released by SD.end(); limit open files to one. No persistent test buffers.
  const bool mounted = SD.begin(SD_CS, sdBus, SD_CLOCK_HZ, "/sd", 1, false);
  bool passed = false;
  if (mounted && SD.cardType() != CARD_NONE) {
    LOG_INF("E213-SD", "Card mounted: %llu MiB", static_cast<unsigned long long>(SD.cardSize() / (1024 * 1024)));
    passed = testMountedCard();
  } else {
    LOG_ERR("E213-SD", "Mount failed: check card, FAT32 format, power and wiring");
  }
  SD.end();
  sdBus.end();
  digitalWrite(SD_CS, HIGH);
  LOG_INF("E213-SD", "Startup write/read/cleanup test: %s; card unmounted", passed ? "PASS (1024 bytes)" : "FAIL");
  return passed;
}
