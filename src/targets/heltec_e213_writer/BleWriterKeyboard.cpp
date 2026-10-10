#include "BleWriterKeyboard.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include <host/ble_sm.h>

namespace {
constexpr unsigned long kScanDurationMs = 8000;
constexpr unsigned long kScanRetryMs = 2500;
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint16_t kAppearanceGenericHid = 0x03C0;
constexpr uint16_t kAppearanceKeyboard = 0x03C1;

portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
BleWriterKeyboard* activeKeyboard = nullptr;

bool nameLooksLikeKeyboard(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name.find("keyboard") != std::string::npos || name.find("keychron") != std::string::npos ||
         name.find("keys") != std::string::npos || name.find("kbd") != std::string::npos ||
         name.find("logi") != std::string::npos || name.find("magic keyboard") != std::string::npos;
}

void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  if (activeKeyboard) activeKeyboard->handleReport(data, length);
}

class ScanCallbacks final : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (activeKeyboard) activeKeyboard->handleAdvertisement(device);
  }

  void onScanEnd(const NimBLEScanResults&, int) override {
    if (activeKeyboard) activeKeyboard->handleScanEnd();
  }
};

class ClientCallbacks final : public NimBLEClientCallbacks {
 public:
  void onDisconnect(NimBLEClient* client, int reason) override {
    if (activeKeyboard) activeKeyboard->handleDisconnect(client, reason);
  }
};

ScanCallbacks scanCallbacks;
ClientCallbacks clientCallbacks;
}  // namespace

void BleWriterKeyboard::begin() {
  activeKeyboard = this;
  clearQueue();
  clearCandidates();
  memset(previousKeys, 0, sizeof(previousKeys));
  connected = false;
  connecting = false;
  scanning = false;
  scanRequested = true;

  NimBLEDevice::init("E213 Writer");
  NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  auto* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  bumpVersion();
}

void BleWriterKeyboard::update() {
  if (connected || connecting || scanning) return;

  const unsigned long now = millis();
  if (!scanRequested && now - lastScanAt < kScanRetryMs) return;

  scanRequested = false;
  lastScanAt = now;
  scanning = NimBLEDevice::getScan()->start(kScanDurationMs, false, true);
  bumpVersion();
}

bool BleWriterKeyboard::isKeyboardCandidate(const NimBLEAdvertisedDevice* device) const {
  if (!device) return false;
  if (device->isAdvertisingService(NimBLEUUID(kHidServiceUuid))) return true;

  if (device->haveAppearance()) {
    const uint16_t appearance = device->getAppearance();
    if (appearance == kAppearanceGenericHid || appearance == kAppearanceKeyboard) return true;
  }

  return device->haveName() && nameLooksLikeKeyboard(device->getName());
}

void BleWriterKeyboard::handleAdvertisement(const NimBLEAdvertisedDevice* device) {
  if (!isKeyboardCandidate(device)) return;

  Candidate candidate;
  const std::string name = device->haveName() ? device->getName() : "Unnamed keyboard";
  const NimBLEAddress& address = device->getAddress();
  std::strncpy(candidate.name, name.c_str(), sizeof(candidate.name) - 1);
  std::strncpy(candidate.address, address.toString().c_str(), sizeof(candidate.address) - 1);
  candidate.addressType = address.getType();
  candidate.rssi = device->getRSSI();

  bool changed = false;
  taskENTER_CRITICAL(&stateMux);
  int index = -1;
  for (int i = 0; i < candidateTotal; ++i) {
    if (std::strncmp(candidates[i].address, candidate.address, sizeof(candidate.address)) == 0) {
      index = i;
      break;
    }
  }

  if (index < 0 && candidateTotal < kMaxCandidates) {
    index = candidateTotal++;
    changed = true;
  }
  if (index >= 0) {
    candidates[index] = candidate;
  }
  taskEXIT_CRITICAL(&stateMux);

  if (changed) bumpVersion();
}

void BleWriterKeyboard::handleScanEnd() {
  scanning = false;
  bumpVersion();
}

bool BleWriterKeyboard::connectSelectedCandidate() {
  Candidate candidate;
  if (!getCandidate(selectedIndex, candidate)) {
    requestScan();
    return false;
  }

  NimBLEDevice::getScan()->stop();
  scanning = false;
  connecting = true;
  bumpVersion();

  client = NimBLEDevice::createClient();
  if (!client) {
    connecting = false;
    requestScan();
    return false;
  }

  client->setClientCallbacks(&clientCallbacks, false);
  client->setConnectTimeout(kConnectTimeoutMs);
  client->setConnectRetries(0);
  client->setConnectionParams(24, 48, 0, 60);

  const NimBLEAddress address(std::string(candidate.address), candidate.addressType);
  if (!client->connect(address)) {
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    connecting = false;
    requestScan();
    return false;
  }

  auto* hidService = client->getService(NimBLEUUID(kHidServiceUuid));
  if (!hidService) {
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    connecting = false;
    requestScan();
    return false;
  }

  // Use the same security approach as CrossPointXT's existing BLE editor.
  client->secureConnection(false);

  if (auto* protocol = hidService->getCharacteristic(NimBLEUUID(kProtocolModeUuid))) {
    protocol->writeValue(&kBootProtocol, 1, true);
  }

  int subscribed = 0;
  if (auto* bootInput = hidService->getCharacteristic(NimBLEUUID(kBootKeyboardInputUuid))) {
    if ((bootInput->canNotify() || bootInput->canIndicate()) && bootInput->subscribe(true, notifyCallback)) {
      subscribed++;
    }
  }

  for (auto* characteristic : hidService->getCharacteristics(true)) {
    if (!characteristic || characteristic->getUUID() != NimBLEUUID(kReportUuid)) continue;
    if ((characteristic->canNotify() || characteristic->canIndicate()) &&
        characteristic->subscribe(true, notifyCallback)) {
      subscribed++;
    }
  }

  if (subscribed == 0) {
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    connecting = false;
    requestScan();
    return false;
  }

  connecting = false;
  connected = true;
  clearQueue();
  memset(previousKeys, 0, sizeof(previousKeys));
  bumpVersion();
  return true;
}

void BleWriterKeyboard::handleDisconnect(NimBLEClient* disconnectedClient, int reason) {
  Serial.printf("BLE keyboard disconnected (reason=%d)\n", reason);
  if (disconnectedClient == client) client = nullptr;
  connected = false;
  connecting = false;
  scanRequested = true;
  memset(previousKeys, 0, sizeof(previousKeys));
  bumpVersion();
}

void BleWriterKeyboard::handleReport(const uint8_t* data, size_t length) {
  if (!data || length < 8) return;

  // Some HID devices prepend a report ID.
  if (length >= 9 && data[0] != 0 && data[1] <= 0x22) {
    data++;
    length--;
  }
  if (length < 8) return;

  const uint8_t modifiers = data[0];
  const bool shifted = (modifiers & 0x22) != 0;
  const bool ctrl = (modifiers & 0x11) != 0;
  const uint8_t* keys = data + 2;

  for (int i = 0; i < 6; ++i) {
    const uint8_t keyCode = keys[i];
    if (keyCode == 0 || keyAlreadyDown(keyCode, previousKeys)) continue;

    // Ctrl shortcuts are commands, not text: never insert a literal O/N/S.
    if (ctrl) {
      if (keyCode == 0x12) pushEvent(KeyEvent{KeyType::OpenFiles}); // Ctrl+O
      if (keyCode == 0x11) pushEvent(KeyEvent{KeyType::NewFile}); // Ctrl+N
      if (keyCode == 0x16) pushEvent(KeyEvent{KeyType::SaveFile}); // Ctrl+S
      continue;
    }
    if (const char character = keyCodeToAscii(keyCode, shifted); character != '\0') {
      pushEvent(KeyEvent{KeyType::Character, character});
      continue;
    }

    switch (keyCode) {
      case 0x28:
        pushEvent(KeyEvent{KeyType::Enter});
        break;
      case 0x29:
        pushEvent(KeyEvent{KeyType::Escape});
        break;
      case 0x2A:
        pushEvent(KeyEvent{KeyType::Backspace});
        break;
      case 0x2B:
        pushEvent(KeyEvent{KeyType::Tab});
        break;
      case 0x4C:
        pushEvent(KeyEvent{KeyType::DeleteKey});
        break;
      case 0x4F:
        pushEvent(KeyEvent{KeyType::Right});
        break;
      case 0x50:
        pushEvent(KeyEvent{KeyType::Left});
        break;
      case 0x51:
        pushEvent(KeyEvent{KeyType::Down});
        break;
      case 0x52:
        pushEvent(KeyEvent{KeyType::Up});
        break;
      default:
        break;
    }
  }

  memcpy(previousKeys, keys, sizeof(previousKeys));
}

char BleWriterKeyboard::keyCodeToAscii(const uint8_t keyCode, const bool shifted) {
  if (keyCode >= 0x04 && keyCode <= 0x1D) {
    const char c = static_cast<char>('a' + keyCode - 0x04);
    return shifted ? static_cast<char>(c - 'a' + 'A') : c;
  }

  static constexpr char normalDigits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
  static constexpr char shiftedDigits[] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
  if (keyCode >= 0x1E && keyCode <= 0x27) {
    const int index = keyCode - 0x1E;
    return shifted ? shiftedDigits[index] : normalDigits[index];
  }

  switch (keyCode) {
    case 0x2C: return ' ';
    case 0x2D: return shifted ? '_' : '-';
    case 0x2E: return shifted ? '+' : '=';
    case 0x2F: return shifted ? '{' : '[';
    case 0x30: return shifted ? '}' : ']';
    case 0x31: return shifted ? '|' : '\\';
    case 0x33: return shifted ? ':' : ';';
    case 0x34: return shifted ? '"' : '\'';
    case 0x35: return shifted ? '~' : '`';
    case 0x36: return shifted ? '<' : ',';
    case 0x37: return shifted ? '>' : '.';
    case 0x38: return shifted ? '?' : '/';
    default: return '\0';
  }
}

bool BleWriterKeyboard::keyAlreadyDown(const uint8_t keyCode, const uint8_t* keys) {
  for (int i = 0; i < 6; ++i) {
    if (keys[i] == keyCode) return true;
  }
  return false;
}

void BleWriterKeyboard::clearQueue() {
  taskENTER_CRITICAL(&queueMux);
  queueHead = 0;
  queueTail = 0;
  taskEXIT_CRITICAL(&queueMux);
}

void BleWriterKeyboard::pushEvent(const KeyEvent event) {
  taskENTER_CRITICAL(&queueMux);
  const uint8_t next = (queueHead + 1) % kQueueSize;
  if (next != queueTail) {
    queue[queueHead] = event;
    queueHead = next;
  }
  taskEXIT_CRITICAL(&queueMux);
}

bool BleWriterKeyboard::popEvent(KeyEvent& event) {
  bool found = false;
  taskENTER_CRITICAL(&queueMux);
  if (queueTail != queueHead) {
    event = queue[queueTail];
    queueTail = (queueTail + 1) % kQueueSize;
    found = true;
  }
  taskEXIT_CRITICAL(&queueMux);
  return found;
}

void BleWriterKeyboard::clearCandidates() {
  taskENTER_CRITICAL(&stateMux);
  candidateTotal = 0;
  selectedIndex = 0;
  memset(candidates, 0, sizeof(candidates));
  taskEXIT_CRITICAL(&stateMux);
}

int BleWriterKeyboard::candidateCount() const {
  int count;
  taskENTER_CRITICAL(&stateMux);
  count = candidateTotal;
  taskEXIT_CRITICAL(&stateMux);
  return count;
}

bool BleWriterKeyboard::getCandidate(const int index, Candidate& out) const {
  bool found = false;
  taskENTER_CRITICAL(&stateMux);
  if (index >= 0 && index < candidateTotal) {
    out = candidates[index];
    found = true;
  }
  taskEXIT_CRITICAL(&stateMux);
  return found;
}

void BleWriterKeyboard::selectNextCandidate() {
  taskENTER_CRITICAL(&stateMux);
  if (candidateTotal > 0) selectedIndex = (selectedIndex + 1) % candidateTotal;
  taskEXIT_CRITICAL(&stateMux);
  bumpVersion();
}

void BleWriterKeyboard::requestScan() {
  if (connected || connecting) return;
  clearCandidates();
  scanRequested = true;
  bumpVersion();
}

const char* BleWriterKeyboard::statusText() const {
  if (connected) return "Keyboard connected";
  if (connecting) return "Connecting...";
  if (scanning) return candidateTotal > 0 ? "Scanning - keyboard found" : "Scanning for BLE keyboard";
  if (candidateTotal > 0) return "Choose keyboard";
  return "Put keyboard in pairing mode";
}

void BleWriterKeyboard::bumpVersion() { version = version + 1; }
