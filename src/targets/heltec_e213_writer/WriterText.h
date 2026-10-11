#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

// Fixed lifetime buffers: no allocation or string growth while typing/rendering.
// This standalone S3 target reserves 64 KiB for the text and another in WriterStore
// for validated file loads. These buffers are never compiled into the C3 reader.
struct WriterText {
  static constexpr size_t CAPACITY = 65536;
  char bytes[CAPACITY + 1]{};
  size_t length = 0;
  size_t size() const { return length; }
  bool empty() const { return length == 0; }
  const char* data() const { return bytes; }
  const char* c_str() const { return bytes; }
  char operator[](size_t i) const { return bytes[i]; }
  void assign(const char* data, size_t size) {
    memcpy(bytes, data, size);
    length = size;
    bytes[size] = 0;
  }
  bool insert(size_t offset, char c) {
    if (offset > length || length == CAPACITY) return false;
    memmove(bytes + offset + 1, bytes + offset, length - offset + 1);
    bytes[offset] = c;
    ++length;
    return true;
  }
  void erase(size_t offset, size_t count) {
    count = std::min(count, length - offset);
    memmove(bytes + offset, bytes + offset + count, length - offset - count + 1);
    length -= count;
  }
};

inline uint32_t writerHash(const char* data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ static_cast<uint8_t>(data[i])) * 16777619u;
  return hash;
}

inline size_t nextCharacter(const WriterText& text, size_t at) {
  if (at >= text.size()) return text.size();
  do { ++at; } while (at < text.size() && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80);
  return at;
}
inline size_t previousCharacter(const WriterText& text, size_t at) {
  if (!at) return 0;
  do { --at; } while (at && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80);
  return at;
}

// Reject binary/control bytes and malformed UTF-8 before replacing a draft.
inline bool validWriterText(const char* text, size_t size) {
  for (size_t i = 0; i < size;) {
    const uint8_t c = text[i++];
    if (c < 0x80) {
      if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return false;
      continue;
    }
    const int extra = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 :
                      c >= 0xF0 && c <= 0xF4 ? 3 : -1;
    if (extra < 0 || i + extra > size) return false;
    const uint8_t first = text[i];
    if ((c == 0xE0 && first < 0xA0) || (c == 0xED && first >= 0xA0) ||
        (c == 0xF0 && first < 0x90) || (c == 0xF4 && first >= 0x90)) return false;
    for (int j = 0; j < extra; ++j) if ((static_cast<uint8_t>(text[i++]) & 0xC0) != 0x80) return false;
  }
  return true;
}

struct WriterLine { size_t start; size_t end; size_t next; bool more; };
inline WriterLine writerLine(const WriterText& text, size_t start, size_t columns = 39) {
  size_t end = start;
  size_t count = 0;
  while (end < text.size() && text[end] != '\n' && count < columns) {
    if (text[end] != '\r') ++count;
    end = nextCharacter(text, end);
  }
  const bool newline = end < text.size() && text[end] == '\n';
  return {start, end, end + (newline ? 1 : 0), newline || end < text.size()};
}
inline size_t writerColumn(const WriterText& text, size_t start, size_t end) {
  size_t count = 0;
  while (start < end) {
    if (text[start] != '\r') ++count;
    start = nextCharacter(text, start);
  }
  return count;
}
