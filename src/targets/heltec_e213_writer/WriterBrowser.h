#pragma once
#include <HalStorage.h>
#include "WriterStore.h"

class WriterBrowser {
 public:
  static constexpr size_t PAGE_SIZE = 8;
  struct Entry { char name[96]{}; bool directory = false; };
  Entry entries[PAGE_SIZE];
  char directory[WriterStore::PATH_SIZE] = "/";
  char selectedPath[WriterStore::PATH_SIZE]{};
  size_t count = 0;
  size_t selected = 0;
  size_t skip = 0;
  bool more = false;
  bool failed = false;

  // Four fixed actions precede the paged directory entries: resume, new, up,
  // next page. Only eight names are retained, regardless of directory size.
  size_t total() const { return count + 4; }
  void next() { selected = (selected + 1) % total(); }
  void previous() { selected = selected ? selected - 1 : total() - 1; }
  void load() {
    count = 0;
    more = false;
    failed = false;
    selected = 0;
    auto dir = Storage.open(directory);
    if (!dir || !dir.isDirectory()) { failed = true; return; }
    size_t seen = 0;
    while (true) {
      auto file = dir.openNextFile();
      if (!file) break;
      char name[96];
      const size_t length = file.getName(name, sizeof(name));
      if (!length || length >= sizeof(name) - 1 || name[0] == '.' || strchr(name, '\n') || strchr(name, '\r')) continue;
      const char* extension = strrchr(name, '.');
      if (!file.isDirectory() && (!extension || (strcasecmp(extension, ".txt") && strcasecmp(extension, ".md")))) continue;
      if (seen++ < skip) continue;
      if (count == PAGE_SIZE) { more = true; break; }
      strcpy(entries[count].name, name);
      entries[count++].directory = file.isDirectory();
    }
  }
  void page() { skip = more ? skip + PAGE_SIZE : 0; load(); }
  bool parent() {
    if (!strcmp(directory, "/")) return false;
    char* slash = strrchr(directory, '/');
    if (slash == directory) directory[1] = 0;
    else if (slash) *slash = 0;
    skip = 0;
    load();
    return true;
  }
  bool choosePath() {
    if (selected < 4 || selected - 4 >= count) return false;
    const int n = snprintf(selectedPath, sizeof(selectedPath), "%s%s%s", directory,
                          !strcmp(directory, "/") ? "" : "/", entries[selected - 4].name);
    return n > 0 && static_cast<size_t>(n) < sizeof(selectedPath);
  }
  void enter() { strcpy(directory, selectedPath); skip = 0; load(); }
};
