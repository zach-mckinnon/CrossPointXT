#pragma once
// Small, Arduino-independent edit engine. History is in RAM; saved manuscripts
// remain on microSD. Store inverse changes (not full manuscript snapshots).
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace writerediting {

enum class Kind { Insert, Backspace, DeleteForward, Other };
struct Operation {
  size_t at = 0;
  std::string removed, inserted;
  size_t before = 0, after = 0;
  uint32_t lastAt = 0;
  Kind kind = Kind::Other;
};
class History {
 public:
  static constexpr size_t maxOperations = 48;
  static constexpr size_t maxBytes = 32768;

  void clear() { undos_.clear(); redos_.clear(); }

  bool change(std::string& doc, size_t& cursor, size_t at, size_t remove,
              const std::string& insert, uint32_t now, Kind kind) {
    if (at > doc.size() || remove > doc.size() - at) return false;
    if (remove == 0 && insert.empty()) return false;
    Operation op;
    op.at = at;
    op.removed = doc.substr(at, remove);
    op.inserted = insert;
    op.before = cursor;
    op.after = at + insert.size();
    op.lastAt = now;
    op.kind = kind;
    doc.replace(at, remove, insert);
    cursor = op.after;
    redos_.clear();

    if (!undos_.empty()) {
      auto& last = undos_.back();
      const bool recent = static_cast<uint32_t>(now - last.lastAt) <= 700;
      if (recent && kind == last.kind && kind != Kind::Other) {
        if (kind == Kind::Insert && op.at == last.at + last.inserted.size()) {
          last.inserted += op.inserted;
          last.after = op.after;
          last.lastAt = now;
          return true;
        }
        if (kind == Kind::Backspace && op.at + op.removed.size() == last.at) {
          last.at = op.at;
          last.removed.insert(0, op.removed);
          last.after = op.after;
          last.lastAt = now;
          return true;
        }
        if (kind == Kind::DeleteForward && op.at == last.at) {
          last.removed += op.removed;
          last.after = op.after;
          last.lastAt = now;
          return true;
        }
      }
    }
    undos_.push_back(std::move(op));
    trim(undos_);
    return true;
  }

  bool undo(std::string& doc, size_t& cursor) {
    if (undos_.empty()) return false;
    Operation op = std::move(undos_.back());
    undos_.pop_back();
    if (!replaceChecked(doc, op.at, op.inserted, op.removed)) {
      undos_.push_back(std::move(op));
      return false;
    }
    cursor = std::min(op.before, doc.size());
    redos_.push_back(std::move(op));
    trim(redos_);
    return true;
  }

  bool redo(std::string& doc, size_t& cursor) {
    if (redos_.empty()) return false;
    Operation op = std::move(redos_.back());
    redos_.pop_back();
    if (!replaceChecked(doc, op.at, op.removed, op.inserted)) {
      redos_.push_back(std::move(op));
      return false;
    }
    cursor = std::min(op.after, doc.size());
    undos_.push_back(std::move(op));
    trim(undos_);
    return true;
  }

  size_t undoCount() const { return undos_.size(); }
  size_t redoCount() const { return redos_.size(); }

 private:
  std::vector<Operation> undos_, redos_;
  static bool replaceChecked(std::string& doc, size_t at,
                             const std::string& expected, const std::string& replacement) {
    if (at > doc.size() || expected.size() > doc.size() - at ||
        doc.compare(at, expected.size(), expected) != 0) return false;
    doc.replace(at, expected.size(), replacement);
    return true;
  }
  static void trim(std::vector<Operation>& ops) {
    size_t bytes = 0;
    for (const auto& op : ops) bytes += op.removed.size() + op.inserted.size();
    while (!ops.empty() && (ops.size() > maxOperations ||
                            (bytes > maxBytes && ops.size() > 1))) {
      bytes -= ops.front().removed.size() + ops.front().inserted.size();
      ops.erase(ops.begin());
    }
  }
};

// Backspace should not destroy half of a multibyte UTF-8 character.
inline size_t previousCodepoint(const std::string& text, size_t pos) {
  if (pos == 0) return 0;
  size_t at = std::min(pos, text.size()) - 1;
  while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return at;
}
inline size_t nextCodepoint(const std::string& text, size_t pos) {
  if (pos >= text.size()) return text.size();
  size_t at = pos + 1;
  while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
  return at;
}
inline bool sentenceTerminator(char c) { return c == '.' || c == '!' || c == '?'; }
inline bool sentenceTail(char c) { return c == '"' || c == '\'' || c == ')' || c == ']'; }

// Home/End go to the beginning/end of the current prose sentence.
// Ctrl+Home/End are handled in the editor as document boundaries.
inline size_t sentenceStart(const std::string& text, size_t cursor) {
  size_t pos = std::min(cursor, text.size());
  // Only treat punctuation followed by whitespace/end as sentence boundaries.
  for (size_t i = pos; i > 0; --i) {
    size_t term = i - 1;
    if (!sentenceTerminator(text[term])) continue;
    size_t after = term + 1;
    while (after < text.size() && sentenceTail(text[after])) ++after;
    if (after > pos) continue;
    if (after != text.size() && !std::isspace(static_cast<unsigned char>(text[after]))) continue;
    if (after == pos && pos > 0) continue;
    while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) ++after;
    if (after <= pos) return after;
  }
  return 0;
}
inline size_t sentenceEnd(const std::string& text, size_t cursor) {
  size_t pos = std::min(cursor, text.size());
  for (size_t i = pos; i < text.size(); ++i) {
    if (!sentenceTerminator(text[i])) continue;
    size_t after = i + 1;
    while (after < text.size() && sentenceTail(text[after])) ++after;
    if (after == text.size() ||
        std::isspace(static_cast<unsigned char>(text[after]))) return after;
  }
  return text.size();
}
} // namespace writerediting
