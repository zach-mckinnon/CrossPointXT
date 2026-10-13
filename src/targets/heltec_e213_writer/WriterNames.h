#pragma once
#include <string>
#include <cctype>

namespace writernames {
// Names are intentionally simple for FAT32 SD and the USB sync protocol.
inline std::string trim(const std::string& name) {
  const auto a = name.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  const auto b = name.find_last_not_of(' ');
  return name.substr(a, b - a + 1);
}
inline std::string lower(std::string name) {
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name;
}
inline bool validStem(const std::string& stem) {
  if (stem.empty() || stem.size() > 44 || trim(stem) != stem) return false;
  for (char c : stem) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-')) return false;
  }
  return true;
}
inline std::string filename(const std::string& stem) {
  return validStem(stem) ? stem + ".txt" : "";
}
inline std::string stem(const std::string& filename) {
  return filename.size() > 4 && filename.substr(filename.size() - 4) == ".txt"
      ? filename.substr(0, filename.size() - 4) : "";
}
inline bool nameTaken(const std::string& candidate, const std::string& existing) {
  return lower(candidate) == lower(existing);
}
}
