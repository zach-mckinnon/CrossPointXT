#pragma once
#include "WriterText.h"

class WriterStore {
 public:
  static constexpr size_t PATH_SIZE = 160;
  WriterText text;
  bool begin();
  bool save();
  bool open(const char* path);
  bool create();
  void edited() { changed = true; pending = true; }
  bool dirty() const { return changed || pending; }
  bool internalReady() const { return flashReady; }
  bool sdReady() const { return cardReady; }
  bool onCard() const { return activePath[0] != 0; }
  const char* path() const { return activePath; }
  const char* name() const;
  void shutdown();
 private:
  WriterText staging;
  char activePath[PATH_SIZE]{};
  // Member scratch avoids putting long paths on the small Arduino task stack.
  char temporary[PATH_SIZE + 16]{};
  char backup[PATH_SIZE + 16]{};
  char candidate[PATH_SIZE]{};
  bool flashReady = false;
  bool cardReady = false;
  bool changed = false;
  bool pending = false;
  bool hasBase = false;
  uint32_t baseHash = 0;
  bool checkpoint(const WriterText& value, const char* path, bool needsSD, bool base, uint32_t hash);
  bool loadCheckpoint();
  bool readCard(const char* path, WriterText& value);
  bool saveCard();
  bool newPath();
};
