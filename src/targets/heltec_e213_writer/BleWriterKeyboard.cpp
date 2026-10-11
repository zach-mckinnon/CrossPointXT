#include "BleWriterKeyboard.h"
#include "WriterShortcut.h"
#include <Logging.h>
#include <I18n.h>

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
  loadRememberedPeer();
  reconnect.begin(rememberedPeer.valid(), millis());
  reconnectPending = false;
  disconnectedEvent = false;
  bumpVersion();
}

void BleWriterKeyboard::update() {
  if (disconnectedEvent.exchange(false)) {
    if (lostReadyConnection.exchange(false)) reconnect.disconnected(millis());
    scanRequested = true;
    clearQueue();
  }
  if (connected || connecting) return;

  // Queue on one loop and connect on the next so the screen can show progress.
  // No connect, service discovery, NVS writes or deletion runs in a BLE callback.
  if (reconnectPending) {
    reconnectPending = false;
    automaticAttempt = true;
    connectAddress(NimBLEAddress(rememberedPeer.address, rememberedPeer.type));
    automaticAttempt = false;
    return;
  }

  const unsigned long now = millis();
  if (reconnect.due(now)) {
    NimBLEDevice::getScan()->stop();
    scanning = false;
    reconnectPending = true;
    bumpVersion();
    return;
  }
  if (scanning) return;
  if (!scanRequested && now - lastScanAt < kScanRetryMs) return;

  scanRequested = false;
  lastScanAt = now;
  scanning = NimBLEDevice::getScan()->start(kScanDurationMs, false, true);
  bumpVersion();
}

bool BleWriterKeyboard::end() {
  if (!NimBLEDevice::deinit(true)) {
    LOG_ERR("E213", "BLE shutdown failed");
    return false;
  }
  activeKeyboard = nullptr;
  client = nullptr;
  connected = false;
  connecting = false;
  scanning = false;
  scanRequested = false;
  reconnectPending = false;
  automaticAttempt = false;
  disconnectedEvent = false;
  lostReadyConnection = false;
  clearQueue();
  return true;
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
  if (connected || connecting) return false;
  reconnect.manualSelection();
  reconnectPending = false;
  Candidate candidate;
  if (!getCandidate(selectedIndex, candidate)) {
    requestScan();
    return false;
  }

  return connectAddress(NimBLEAddress(std::string(candidate.address), candidate.addressType));
}

bool BleWriterKeyboard::connectAddress(const NimBLEAddress& address) {
  NimBLEDevice::getScan()->stop();
  scanning = false;
  connecting = true;
  disconnectedEvent = false;
  bumpVersion();

  // Keep one SDK-owned client for this keyboard manager's lifetime. Dropping
  // the pointer on disconnect leaked clients until the NimBLE limit was hit.
  // Service discovery needs SDK allocations; reuse the client and let connect
  // discard the old attribute cache instead of creating an unbounded pool.
  if (!client) client = NimBLEDevice::createClient();
  if (!client) {
    return connectionFailed("client allocation failed");
  }

  client->setClientCallbacks(&clientCallbacks, false);
  client->setSelfDelete(false, false);
  client->setConnectTimeout(kConnectTimeoutMs);
  client->setConnectRetries(0);
  client->setConnectionParams(24, 48, 0, 60);

  LOG_INF("E213", "%s keyboard %s (type=%u)", automaticAttempt ? "Reconnecting to" : "Connecting to",
          address.toString().c_str(), address.getType());
  if (!client->connect(address)) {
    return connectionFailed("connection failed or timed out");
  }

  auto* hidService = client->getService(NimBLEUUID(kHidServiceUuid));
  if (!hidService) {
    return connectionFailed("HID service unavailable");
  }

  // Use the same security approach as CrossPointXT's existing BLE editor.
  if (!client->secureConnection(false)) {
    LOG_INF("E213", "Keyboard security failed; checking whether HID reports remain accessible");
  }

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

  if (subscribed == 0 || !client->isConnected() || disconnectedEvent) {
    return connectionFailed("no usable HID subscription or peer disconnected");
  }

  clearQueue();
  memset(previousKeys, 0, sizeof(previousKeys));
  rememberConnectedPeer();
  if (rememberedPeer.valid()) reconnect.connected(millis());
  else reconnect.manualSelection();
  // Serialize publication with the disconnect callback so a lost connection
  // cannot be overwritten by the end of the synchronous setup sequence.
  taskENTER_CRITICAL(&stateMux);
  connected = !disconnectedEvent.load();
  taskEXIT_CRITICAL(&stateMux);
  connecting = false;
  if (!connected) return connectionFailed("peer disconnected during setup");
  LOG_INF("E213", "Keyboard ready: %d report subscriptions, bonded=%d", subscribed, client->getConnInfo().isBonded());
  bumpVersion();
  return true;
}

bool BleWriterKeyboard::connectionFailed(const char* reason) {
  LOG_ERR("E213", "Keyboard connection: %s", reason);
  if (client && client->isConnected()) client->disconnect();
  connecting = false;
  connected = false;
  scanRequested = true;
  reconnect.failed(millis());
  bumpVersion();
  return false;
}

void BleWriterKeyboard::loadRememberedPeer() {
  if (HeltecKeyboardIdentityStore::load(rememberedPeer)) {
    LOG_INF("E213", "Loaded last keyboard identity");
    return;
  }
  // Older writer builds stored NimBLE bonds but not the last peer. A single
  // bond is unambiguous; with multiple bonds require one manual selection.
  if (NimBLEDevice::getNumBonds() == 1) {
    const auto address = NimBLEDevice::getBondedAddress(0);
    rememberedPeer.version = 1;
    rememberedPeer.type = address.getType();
    memcpy(rememberedPeer.address, address.getVal(), sizeof(rememberedPeer.address));
    LOG_INF("E213", "Using the only existing keyboard bond for initial reconnect");
  }
}

void BleWriterKeyboard::rememberConnectedPeer() {
  const auto info = client->getConnInfo();
  // Persist the identity rather than a rotating over-the-air privacy address.
  const auto address = info.isBonded() ? info.getIdAddress() : client->getPeerAddress();
  if (address.isNull() || address.isRpa() || address.isNrpa()) {
    rememberedPeer = {};
    LOG_ERR("E213", "Keyboard has no stable identity; manual pairing will be needed after restart");
    return;
  }
  rememberedPeer.version = 1;
  rememberedPeer.type = address.getType();
  memcpy(rememberedPeer.address, address.getVal(), sizeof(rememberedPeer.address));
  if (!HeltecKeyboardIdentityStore::save(rememberedPeer)) {
    LOG_ERR("E213", "Last keyboard is remembered for this session only");
  }
}

void BleWriterKeyboard::handleDisconnect(NimBLEClient* disconnectedClient, int reason) {
  if (disconnectedClient != client) return;
  LOG_INF("E213", "BLE keyboard disconnected (reason=%d)", reason);
  taskENTER_CRITICAL(&stateMux);
  if (connected.exchange(false)) lostReadyConnection = true;
  disconnectedEvent = true;
  taskEXIT_CRITICAL(&stateMux);
  memset(previousKeys, 0, sizeof(previousKeys));
  bumpVersion();
}

void BleWriterKeyboard::handleReport(const uint8_t* data, size_t length) {
  if (!data || length < 8) return;

  // Some HID devices prepend a report ID.
  if (length == 9 && data[0] != 0) {
    data++;
    length--;
  }
  if (length < 8) return;

  const uint8_t modifiers = data[0];
  const bool shifted = (modifiers & 0x22) != 0;
  const uint8_t* keys = data + 2;

  for (int i = 0; i < 6; ++i) {
    const uint8_t keyCode = keys[i];
    if (keyCode == 0 || keyAlreadyDown(keyCode, previousKeys)) continue;

    switch (decodeWriterShortcut(modifiers, keyCode)) {
      case WriterShortcut::Save:
        pushEvent(KeyEvent{KeyType::Save});
        continue;
      case WriterShortcut::Sleep:
        pushEvent(KeyEvent{KeyType::Sleep});
        continue;
      case WriterShortcut::Files:
        pushEvent(KeyEvent{KeyType::Files});
        continue;
      case WriterShortcut::Suppress:
        continue;
      case WriterShortcut::None:
        break;
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
  reconnect.manualSelection();
  reconnectPending = false;
  taskENTER_CRITICAL(&stateMux);
  if (candidateTotal > 0) selectedIndex = (selectedIndex + 1) % candidateTotal;
  taskEXIT_CRITICAL(&stateMux);
  bumpVersion();
}

void BleWriterKeyboard::requestScan() {
  if (connected || connecting) return;
  reconnect.manualSelection();
  reconnectPending = false;
  clearCandidates();
  scanRequested = true;
  bumpVersion();
}

const char* BleWriterKeyboard::statusText() const {
  if (connected) return "Keyboard connected";
  if (isAutoReconnecting()) return tr(STR_WRITER_RECONNECTING);
  if (connecting) return "Connecting...";
  if (reconnect.enabled()) return tr(STR_WRITER_RECONNECT_WAIT);
  if (scanning) return candidateTotal > 0 ? "Scanning - keyboard found" : "Scanning for BLE keyboard";
  if (candidateTotal > 0) return "Choose keyboard";
  return "Put keyboard in pairing mode";
}

void BleWriterKeyboard::bumpVersion() { ++version; }
