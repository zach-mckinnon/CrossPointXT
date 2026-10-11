#include "../../src/targets/heltec_e213_writer/KeyboardReconnect.h"
#include "../../src/targets/heltec_e213_writer/HeltecKeyboardIdentity.h"

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

int main() { return reconnectLifecycle() && reconnectAcrossMillisWrap() ? 0 : 1; }
