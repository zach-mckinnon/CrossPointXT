#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <cstddef>
#include <cstdint>

class BleWriterKeyboard {
 public:
  enum class KeyType : uint8_t {
    Character,
    Enter,
    Backspace,
    DeleteKey,
    Tab,
    Escape,
    OpenFiles,
    NewFile,
    SaveFile,
    Left,
    Right,
    Up,
    Down
  };

  struct KeyEvent {
    KeyType type = KeyType::Character;
    char character = '\0';
  };

  struct Candidate {
    char name[40] = {};
    char address[18] = {};
    uint8_t addressType = 0;
    int rssi = 0;
  };

  void begin();
  void update();
  bool isConnected() const { return connected; }
  bool isScanning() const { return scanning; }
  bool isConnecting() const { return connecting; }
  bool popEvent(KeyEvent& event);

  int candidateCount() const;
  int selectedCandidateIndex() const { return selectedIndex; }
  bool getCandidate(int index, Candidate& out) const;
  void selectNextCandidate();
  void requestScan();
  bool connectSelectedCandidate();

  const char* statusText() const;
  uint32_t statusVersion() const { return version; }

  // Internal hooks used by NimBLE callbacks.
  void handleAdvertisement(const NimBLEAdvertisedDevice* device);
  void handleReport(const uint8_t* data, size_t length);
  void handleDisconnect(NimBLEClient* client, int reason);
  void handleScanEnd();

 private:
  static constexpr int kQueueSize = 64;
  static constexpr int kMaxCandidates = 8;
  static constexpr uint16_t kHidServiceUuid = 0x1812;
  static constexpr uint16_t kProtocolModeUuid = 0x2A4E;
  static constexpr uint16_t kBootKeyboardInputUuid = 0x2A22;
  static constexpr uint16_t kReportUuid = 0x2A4D;
  static constexpr uint8_t kBootProtocol = 0x00;

  volatile bool connected = false;
  volatile bool connecting = false;
  volatile bool scanning = false;
  volatile bool scanRequested = false;
  volatile uint32_t version = 0;
  uint32_t lastScanAt = 0;

  Candidate candidates[kMaxCandidates];
  int candidateTotal = 0;
  int selectedIndex = 0;

  KeyEvent queue[kQueueSize];
  volatile uint8_t queueHead = 0;
  volatile uint8_t queueTail = 0;
  uint8_t previousKeys[6] = {};

  NimBLEClient* client = nullptr;

  void clearQueue();
  void clearCandidates();
  void pushEvent(KeyEvent event);
  void bumpVersion();
  bool isKeyboardCandidate(const NimBLEAdvertisedDevice* device) const;
  static char keyCodeToAscii(uint8_t keyCode, bool shifted);
  static bool keyAlreadyDown(uint8_t keyCode, const uint8_t* keys);
};
