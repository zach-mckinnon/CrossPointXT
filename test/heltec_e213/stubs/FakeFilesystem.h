#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Host-only fault injection. Firmware uses the real serialized HAL.
constexpr int O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4;
constexpr int FILE_READ = O_RDONLY, FILE_WRITE = O_WRONLY | O_CREAT | O_TRUNC;
struct FakeFilesystem;
struct File {
  FakeFilesystem* fs = nullptr;
  std::string path;
  size_t offset = 0;
  explicit operator bool() const;
  size_t size() const;
  size_t position() const { return offset; }
  bool isDirectory() const { return false; }
  int read();
  int read(void* out, size_t size);
  size_t readBytes(char* out, size_t size) { return read(out, size); }
  size_t write(const void* data, size_t size);
  size_t write(uint8_t byte) { return write(&byte, 1); }
  size_t print(const char* s) { return write(s, strlen(s)); }
  void flush() {}
  bool sync();
};
struct FakeFilesystem {
  bool mounted = true;
  bool failWrite = false;
  bool failSync = false;
  bool failRename = false;
  std::string failRenameSource;
  std::map<std::string, std::string> files;
  bool begin(bool = false) { return mounted; }
  void end() {}
  bool exists(const char* path) { return mounted && files.count(path); }
  bool ensureDirectoryExists(const char*) { return mounted; }
  File open(const char* path, int flags = O_RDONLY) {
    if (!mounted || (!exists(path) && !(flags & O_CREAT))) return {};
    if (flags & O_TRUNC) files[path].clear();
    return {this, path};
  }
  bool remove(const char* path) { return mounted && files.erase(path); }
  bool rename(const char* from, const char* to) {
    if (!exists(from) || failRename || from == failRenameSource) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};
inline File::operator bool() const { return fs && fs->exists(path.c_str()); }
inline size_t File::size() const { return *this ? fs->files[path].size() : 0; }
inline int File::read() { uint8_t c; return read(&c, 1) == 1 ? c : -1; }
inline int File::read(void* out, size_t count) {
  if (!*this) return -1;
  count = std::min(count, size() - offset);
  memcpy(out, fs->files[path].data() + offset, count);
  offset += count;
  return static_cast<int>(count);
}
inline size_t File::write(const void* data, size_t count) {
  if (!*this || fs->failWrite) return 0;
  fs->files[path].append(static_cast<const char*>(data), count);
  return count;
}
inline bool File::sync() { return *this && !fs->failSync; }
