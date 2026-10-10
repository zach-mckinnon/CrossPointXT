#include "WriterFiles.h"
#include <LittleFS.h>
#include <algorithm>
#include <cstdio>

namespace writerfiles {
namespace {
std::string pathFor(const std::string& name) { return "/drafts/" + name; }

// Keep a second recoverable copy until after the next successful write.
bool atomicWrite(const std::string& path, const std::string& content) {
  const std::string temp = path + ".tmp";
  const std::string backup = path + ".bak";
  LittleFS.remove(temp.c_str());
  File file = LittleFS.open(temp.c_str(), "w");
  if (!file) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(content.data()), content.size());
  file.flush();
  file.close();
  if (written != content.size()) {
    LittleFS.remove(temp.c_str());
    return false;
  }
  // Never delete the only valid copy before a replacement is ready.
  if (LittleFS.exists(path.c_str())) {
    LittleFS.remove(backup.c_str());
    if (!LittleFS.rename(path.c_str(), backup.c_str())) return false;
  }
  if (!LittleFS.rename(temp.c_str(), path.c_str())) {
    if (LittleFS.exists(backup.c_str()) && !LittleFS.exists(path.c_str()))
      LittleFS.rename(backup.c_str(), path.c_str());
    return false;
  }
  return true;
}
bool readPath(const std::string& path, std::string& content, const size_t limit) {
  std::string actual = path;
  if (!LittleFS.exists(actual.c_str()) && LittleFS.exists((path + ".bak").c_str()))
    actual += ".bak";
  File file = LittleFS.open(actual.c_str(), "r");
  if (!file || file.isDirectory() || static_cast<size_t>(file.size()) > limit) return false;
  std::string next;
  next.reserve(file.size());
  while (file.available()) next.push_back(static_cast<char>(file.read()));
  file.close();
  content.swap(next);
  return true;
}
}

bool validName(const std::string& name) {
  if (name.size() < 5 || name.size() > 48 || name.substr(name.size()-4) != ".txt") return false;
  for (size_t i=0; i<name.size()-4; ++i) {
    const char c=name[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
          (c>='0'&&c<='9')||c==' '||c=='_'||c=='-')) return false;
  }
  return true;
}

std::vector<std::string> list() {
  std::vector<std::string> names;
  File folder=LittleFS.open("/drafts");
  if (!folder || !folder.isDirectory()) return names;
  File file=folder.openNextFile();
  while (file && names.size()<kMaxDocuments) {
    std::string path=file.name();
    size_t slash=path.find_last_of('/');
    const std::string name=path.substr(slash==std::string::npos ? 0 : slash+1);
    if (!file.isDirectory()) {
      if (validName(name)) names.push_back(name);
      else if (name.size() > 4 && name.substr(name.size() - 4) == ".bak") {
        const std::string original = name.substr(0, name.size() - 4);
        // Power loss between backup and replace must not hide the only draft.
        if (validName(original) && !LittleFS.exists(pathFor(original).c_str()))
          names.push_back(original);
      }
    }
    file=folder.openNextFile();
  }
  std::sort(names.begin(),names.end());
  return names;
}

std::string nextName() {
  if (list().size() >= kMaxDocuments) return "";
  for (int i=1; i<=999; ++i) {
    char name[20];
    snprintf(name,sizeof(name),"Draft-%03d.txt",i);
    if (!exists(name)) return name;
  }
  return "";
}
bool exists(const std::string& name) {
  return validName(name) && (LittleFS.exists(pathFor(name).c_str()) ||
    LittleFS.exists((pathFor(name)+".bak").c_str()));
}
bool read(const std::string& name, std::string& content) {
  return validName(name) && readPath(pathFor(name),content,kMaxDocumentBytes);
}
bool save(const std::string& name, const std::string& content) {
  if (!validName(name) || content.size()>kMaxDocumentBytes) return false;
  return atomicWrite(pathFor(name),content);
}
bool readActiveName(std::string& name) {
  if (!readPath("/.active", name, 48)) return false;
  return validName(name);
}
bool saveActiveName(const std::string& name) {
  return validName(name) && atomicWrite("/.active", name);
}
bool readManifest(std::string& content) {
  return readPath("/.sync",content,16384);
}
bool saveManifest(const std::string& content) {
  return content.size()<=16384 && atomicWrite("/.sync",content);
}
}