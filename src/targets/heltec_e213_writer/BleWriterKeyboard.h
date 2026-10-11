#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "HeltecKeyboardIdentity.h"
#include "KeyboardReconnect.h"

#include <atomic>
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
    Sleep,
    Undo,
    Redo,
    FocusMode,
    HomeSentence,
    EndSentence,
    HomeDocument,
    EndDocument,
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
  bool end();
  void update();
  bool isConnected() const { return connected.load(); }
  bool isScanning() const { return scanning; }
  bool isConnecting() const { return connecting || reconnectPending; }
  bool isAutoReconnecting() const { return reconnectPending || automaticAttempt; }
  bool hasAutomaticReconnect() const { return reconnect.enabled(); }
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

  std::atomic<bool> connected{false};
  bool connecting = false;  // Owned by the Arduino loop.
  std::atomic<bool> scanning{false};
  std::atomic<bool> scanRequested{false};
  std::atomic<bool> disconnectedEvent{false};
  std::atomic<bool> lostReadyConnection{false};
  // Set in BLE advertisement callback, consumed on Arduino loop.
  std::atomic<bool> knownAdvertisementSeen{false};
  bool knownAdvertisementLogged = false;
  uint32_t autoAttemptCount = 0;
  std::atomic<uint32_t> version{0};
  uint32_t lastScanAt = 0;
  HeltecKeyboardIdentity rememberedPeer;
  KeyboardReconnect reconnect;
  bool reconnectPending = false;
  bool automaticAttempt = false;

  Candidate candidates[kMaxCandidates];
  int candidateTotal = 0;
  int selectedIndex = 0;

  KeyEvent queue[kQueueSize];
  volatile uint8_t queueHead = 0;
  volatile uint8_t queueTail = 0;
  uint8_t previousKeys[6] = {};
  uint8_t heldDeleteCode = 0;
  uint32_t deleteHeldSince = 0;
  uint32_t nextDeleteAt = 0;
  void processDeleteRepeat();

  NimBLEClient* client = nullptr;

  bool connectAddress(const NimBLEAddress& address);
  bool connectionFailed(const char* reason);
  void loadRememberedPeer();
  void rememberConnectedPeer();
  void clearQueue();
  void clearCandidates();
  void pushEvent(KeyEvent event);
  void bumpVersion();
  bool isKeyboardCandidate(const NimBLEAdvertisedDevice* device) const;
  static char keyCodeToAscii(uint8_t keyCode, bool shifted);
  static bool keyAlreadyDown(uint8_t keyCode, const uint8_t* keys);
};
