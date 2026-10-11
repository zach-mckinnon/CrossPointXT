#include "../../src/targets/heltec_e213_writer/KeyboardReconnect.h"
#include "../../src/targets/heltec_e213_writer/HeltecKeyboardIdentity.h"
#include "../../src/targets/heltec_e213_writer/KnownKeyboardAdvertisement.h"

constexpr bool reconnectLifecycle() {
  KeyboardReconnect policy;
  policy.begin(false, 100);
  if (policy.due(100) || policy.due(100000)) return false;
  policy.begin(true, 100);
  if (!policy.due(100)) return false;
  policy.failed(1000);
  if (policy.due(20999) || !policy.due(21000)) return false;
  policy.failed(21000);
  if (policy.due(40999) || !policy.due(41000)) return false;
  policy.manualSelection();
  policy.disconnected(42000);
  if (policy.due(100000)) return false;  // A late callback cannot undo manual choice.
  policy.connected(100000);
  policy.disconnected(110000);
  return !policy.due(112499) && policy.due(112500);
}
static_assert(reconnectLifecycle());

constexpr bool reconnectAcrossMillisWrap() {
  KeyboardReconnect policy;
  policy.begin(true, 0xfffffff0U);
  policy.failed(0xfffffff0U);
  return !policy.due(0) && !policy.due(19983) && policy.due(19984);
}
static_assert(reconnectAcrossMillisWrap());

constexpr HeltecKeyboardIdentity validPeer{1, 1, {1, 2, 3, 4, 5, 0xc6}};
static_assert(validPeer.valid());
static_assert(validPeer == validPeer);
static_assert(!HeltecKeyboardIdentity{}.valid());
static_assert(!HeltecKeyboardIdentity{2, 1, {1, 2, 3, 4, 5, 6}}.valid());
static_assert(!HeltecKeyboardIdentity{1, 4, {1, 2, 3, 4, 5, 6}}.valid());
static_assert(!HeltecKeyboardIdentity{1, 0, {255, 255, 255, 255, 255, 255}}.valid());
static_assert(!(validPeer == HeltecKeyboardIdentity{1, 0, {1, 2, 3, 4, 5, 0xc6}}));
static_assert(!(validPeer == HeltecKeyboardIdentity{1, 1, {1, 2, 3, 4, 5, 0xc7}}));


// Known keyboard advertisements can omit HID service, appearance and name.
// Matching the persisted identity is enough; another device is never trusted
// just because it advertises the same name.
constexpr uint8_t knownAddress[6] = {1, 2, 3, 4, 5, 0xc6};
constexpr uint8_t otherAddress[6] = {1, 2, 3, 4, 5, 0xc7};
static_assert(writerkeyboard::matchesKnownPeer(validPeer, knownAddress, 1));
static_assert(!writerkeyboard::matchesKnownPeer(validPeer, otherAddress, 1));
static_assert(!writerkeyboard::matchesKnownPeer(validPeer, knownAddress, 0));
static_assert(!writerkeyboard::matchesKnownPeer(HeltecKeyboardIdentity{}, knownAddress, 1));
static_assert(!writerkeyboard::matchesKnownPeer(validPeer, nullptr, 1));

constexpr bool advertisementRetryLifecycle() {
  KeyboardReconnect policy;
  policy.begin(false, 10);
  if (policy.knownAdvertisementDue(50000)) return false;
  policy.begin(true, 10);
  if (policy.knownAdvertisementDue(5009) || !policy.knownAdvertisementDue(5010)) return false;
  policy.failed(7000);
  if (policy.knownAdvertisementDue(11999) || !policy.knownAdvertisementDue(12000)) return false;
  policy.manualSelection();
  if (policy.knownAdvertisementDue(70000)) return false;
  policy.connected(100000);
  return policy.knownAdvertisementDue(105000);
}
static_assert(advertisementRetryLifecycle());
static_assert(KeyboardReconnect::ADVERTISEMENT_RETRY_MS == 5000);

int main() { return reconnectLifecycle() && reconnectAcrossMillisWrap() && advertisementRetryLifecycle() ? 0 : 1; }
