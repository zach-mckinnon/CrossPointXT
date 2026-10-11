#pragma once

// Setup-only hardware check for the isolated E213 writer. Owns all SD access
// during the check; no file handles or mounted filesystem escape this HAL.
// This does not replace the reader's HalStorage/HalFile API or store drafts.
class HeltecE213StorageProbe final {
 public:
  static bool run();
};
