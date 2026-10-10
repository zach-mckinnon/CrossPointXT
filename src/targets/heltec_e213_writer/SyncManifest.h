#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

namespace e213::sync {

inline constexpr uint8_t kManifestVersion = 1;
inline constexpr const char* kManifestFilename = ".sync";
inline constexpr const char* kProvider = "plot-and-jot";

struct SceneBinding {
  std::string filename;
  int64_t sceneId = 0;
  int64_t chapterId = 0;
  std::string revision;
};

struct Manifest {
  uint8_t version = 0;
  std::string provider;
  int64_t projectId = 0;
  std::string projectName;
  std::vector<SceneBinding> scenes;
};

enum class ParseStatus {
  Ok,
  InvalidJson,
  UnsupportedVersion,
  WrongProvider,
  MissingProjectId,
  InvalidScene
};

struct ParseResult {
  ParseStatus status = ParseStatus::InvalidJson;
  Manifest manifest;

  bool isManaged() const { return status == ParseStatus::Ok; }
};

ParseResult parseManifest(const std::string& json);
std::string serializeManifest(const Manifest& manifest);

}  // namespace e213::sync
