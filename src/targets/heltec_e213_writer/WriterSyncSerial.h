#pragma once
#include <Arduino.h>
#include <string>

// USB-only, explicit synchronization transport; never connects to a network.
// Responses are prefixed so startup/diagnostic logs cannot be confused with data.
class WriterSyncSerial {
 public:
  void poll();
  bool busy() const { return receiving_; }
  std::string takeUpdatedName();
 private:
  std::string incoming_;
  std::string payload_;
  std::string destination_;
  std::string expected_;
  std::string changed_;
  size_t expectedLength_ = 0;
  bool receiving_ = false;
  unsigned long lastChunkAt_ = 0;
  void onLine(const std::string& line);
  void abort(const char* reason);
  void sendGet(const std::string& name);
  void beginPut(const std::string& name, const std::string& expectedHash, size_t size);
  void finishPut();
};