#include "HeltecKeyboardIdentity.h"

#include <Arduino.h>
#include <Preferences.h>


namespace {
constexpr char NAMESPACE[] = "e213-keyboard";
constexpr char PEER_KEY[] = "last-peer";
}

bool HeltecKeyboardIdentityStore::load(HeltecKeyboardIdentity& identity) {
  identity = {};
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) return false;  // First use has no namespace.
  const bool validSize = preferences.getBytesLength(PEER_KEY) == sizeof(identity);
  const bool read = validSize && preferences.getBytes(PEER_KEY, &identity, sizeof(identity)) == sizeof(identity);
  preferences.end();
  if (!read || !identity.valid()) {
    identity = {};
    return false;
  }
  return true;
}

bool HeltecKeyboardIdentityStore::save(const HeltecKeyboardIdentity& identity) {
  if (!identity.valid()) {
    Serial.println("[E213 ERR] Refusing invalid keyboard identity");
    return false;
  }
  HeltecKeyboardIdentity stored;
  if (load(stored) && stored == identity) return true;  // No write for repeated reconnects.
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, false)) {
    Serial.println("[E213 ERR] Cannot open keyboard preferences for writing");
    return false;
  }
  const bool saved = preferences.putBytes(PEER_KEY, &identity, sizeof(identity)) == sizeof(identity);
  preferences.end();
  if (!saved) Serial.println("[E213 ERR] Cannot save last keyboard identity");
  return saved;
}
