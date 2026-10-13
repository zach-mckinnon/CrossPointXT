#pragma once
#include <Arduino.h>
#include <string>

// USB-only, explicit synchronization transport; never connects to a network.
// Responses are prefixed so startup/diagnostic logs cannot be confused with data.
class WriterSyncSerial {
 public:
  void poll();
  bool busy() const { return receiving_; }
  // Host activity enables USB SYNC screen and pauses BLE scanning.
  // A grace period covers users inspecting the PC UI between transfers.
  bool hostActive() const { return hostSeen_ && millis() - lastHostAt_ < 60000UL; }
  void releaseHostSession() { hostSeen_ = false; }
  std::string takeUpdatedName();
 private:
  std::string incoming_;
  std::string payload_;
  std::string destination_;
  std::string expected_;
  std::string changed_;
  size_t expectedLength_ = 0;
  bool receiving_ = false;
  bool hostSeen_ = false;
  unsigned long lastHostAt_ = 0;
  unsigned long lastChunkAt_ = 0;
  void onLine(const std::string& line);
  void abort(const char* reason);
  void sendGet(const std::string& name);
  void beginPut(const std::string& name, const std::string& expectedHash, size_t size);
  void finishPut();
};