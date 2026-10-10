#include "WriterSyncSerial.h"
#include "WriterFiles.h"
#include "SyncManifest.h"
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
constexpr char kPrefix[] = "E213SYNC1|";
std::string sha(const std::string& bytes) {
  unsigned char digest[32] = {};
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest) != 0)
    return "";
  static const char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(64);
  for (const unsigned char b : digest) { output += hex[b >> 4]; output += hex[b & 15]; }
  return output;
}
bool allowed(const std::string& name) {
  return name == ".sync" || writerfiles::validName(name);
}
bool readFile(const std::string& name, std::string& content) {
  return name == ".sync" ? writerfiles::readManifest(content) : writerfiles::read(name, content);
}
void emit(const std::string& line) {
  Serial.print(kPrefix);
  Serial.println(line.c_str());
}
std::vector<std::string> fields(const std::string& line) {
  std::vector<std::string> parts;
  size_t from = 0;
  while (true) {
    size_t pos = line.find('|', from);
    parts.push_back(line.substr(from, pos == std::string::npos ? pos : pos - from));
    if (pos == std::string::npos) return parts;
    from = pos + 1;
  }
}
}

void WriterSyncSerial::abort(const char* reason) {
  receiving_ = false; payload_.clear(); destination_.clear(); expected_.clear();
  emit(std::string("ERR|") + reason);
}

void WriterSyncSerial::sendGet(const std::string& name) {
  if (!allowed(name)) { emit("ERR|invalid-name"); return; }
  std::string data;
  if (!readFile(name, data)) { emit("ERR|not-found"); return; }
  emit("BEGIN|" + std::to_string(data.size()) + "|" + sha(data));
  for (size_t i=0; i<data.size(); i+=192) {
    const size_t count = std::min(static_cast<size_t>(192), data.size()-i);
    unsigned char encoded[260] = {};
    size_t written = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written,
        reinterpret_cast<const unsigned char*>(data.data()+i), count) != 0) {
      emit("ERR|encode"); return;
    }
    emit("DATA|" + std::string(reinterpret_cast<const char*>(encoded), written));
  }
  emit("END");
}

void WriterSyncSerial::beginPut(const std::string& name, const std::string& expectedHash,
                                size_t size) {
  if (!allowed(name) || size > (name == ".sync" ? 16384 : writerfiles::kMaxDocumentBytes)) {
    emit("ERR|invalid-file-or-size"); return;
  }
  std::string current;
  const bool found = readFile(name, current);
  if ((found && sha(current) != expectedHash) || (!found && expectedHash != "*")) {
    emit("ERR|stale-version"); return;
  }
  destination_ = name;
  expected_ = expectedHash;
  expectedLength_ = size;
  payload_.clear();
  payload_.reserve(size);
  receiving_ = true;
  lastChunkAt_ = millis();
  emit("READY");
}

void WriterSyncSerial::finishPut() {
  if (payload_.size() != expectedLength_) { abort("incomplete"); return; }
  if (destination_ == ".sync" && !e213::sync::parseManifest(payload_).isManaged()) {
    abort("invalid-manifest"); return;
  }
  // Recheck before commit, so another writer cannot silently replace data.
  std::string previous;
  const bool found = readFile(destination_, previous);
  if ((found && sha(previous) != expected_) || (!found && expected_ != "*")) {
    abort("stale-version"); return;
  }
  const bool saved = destination_ == ".sync" ?
      writerfiles::saveManifest(payload_) : writerfiles::save(destination_, payload_);
  if (!saved) { abort("write-failed"); return; }
  changed_ = destination_;
  receiving_ = false;
  payload_.clear();
  emit("SAVED");
}

void WriterSyncSerial::onLine(const std::string& line) {
  if (line.rfind(kPrefix, 0) != 0) return;
  const auto args = fields(line.substr(sizeof(kPrefix)-1));
  if (args.empty()) return;
  if (receiving_) {
    if (args[0] == "END") { finishPut(); return; }
    if (args.size() != 2 || args[0] != "DATA") { abort("bad-chunk"); return; }
    unsigned char decoded[256] = {};
    size_t written = 0;
    const int rc = mbedtls_base64_decode(decoded, sizeof(decoded), &written,
      reinterpret_cast<const unsigned char*>(args[1].data()), args[1].size());
    if (rc != 0 || payload_.size() + written > expectedLength_) { abort("bad-data"); return; }
    payload_.append(reinterpret_cast<const char*>(decoded), written);
    lastChunkAt_ = millis();
    return;
  }
  if (args[0] == "HELLO" && args.size() == 1) { emit("READY"); return; }
  if (args[0] == "LIST" && args.size() == 1) {
    for (const auto& name : writerfiles::list()) {
      std::string value;
      if (writerfiles::read(name, value))
        emit("FILE|" + name + "|" + sha(value) + "|" + std::to_string(value.size()));
    }
    emit("END");
    return;
  }
  if (args[0] == "GET" && args.size() == 2) { sendGet(args[1]); return; }
  if (args[0] == "PUT" && args.size() == 4) {
    try {
      const size_t requested=static_cast<size_t>(std::stoul(args[3]));
      beginPut(args[1],args[2],requested);
    } catch (...) {
      emit("ERR|invalid-length");
    }
    return;
  }
  emit("ERR|unknown-command");
}

void WriterSyncSerial::poll() {
  if (receiving_ && millis() - lastChunkAt_ > 15000) abort("timeout");
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      if (!incoming_.empty() && incoming_.back() == '\r') incoming_.pop_back();
      onLine(incoming_);
      incoming_.clear();
    } else if (c != '\0') {
      if (incoming_.size() >= 511) {
        incoming_.clear();
        if (receiving_) abort("line-too-long");
      } else incoming_ += c;
    }
  }
}
std::string WriterSyncSerial::takeUpdatedName() {
  std::string name;
  name.swap(changed_);
  return name;
}