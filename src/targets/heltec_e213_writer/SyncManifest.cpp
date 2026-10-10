#include "SyncManifest.h"

namespace e213::sync {

ParseResult parseManifest(const std::string& json) {
  ParseResult result;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    result.status = ParseStatus::InvalidJson;
    return result;
  }

  const int version = doc["version"] | 0;
  if (version != kManifestVersion) {
    result.status = ParseStatus::UnsupportedVersion;
    return result;
  }

  const char* provider = doc["provider"] | "";
  if (std::string(provider) != kProvider) {
    result.status = ParseStatus::WrongProvider;
    return result;
  }

  const int64_t projectId = doc["project_id"] | static_cast<int64_t>(0);
  if (projectId <= 0) {
    result.status = ParseStatus::MissingProjectId;
    return result;
  }

  result.manifest.version = static_cast<uint8_t>(version);
  result.manifest.provider = provider;
  result.manifest.projectId = projectId;
  result.manifest.projectName = std::string(doc["project_name"] | "");

  JsonObject scenes = doc["scenes"].as<JsonObject>();
  for (JsonPair pair : scenes) {
    JsonObject value = pair.value().as<JsonObject>();
    const int64_t sceneId = value["scene_id"] | static_cast<int64_t>(0);
    const int64_t chapterId = value["chapter_id"] | static_cast<int64_t>(0);
    const char* revision = value["revision"] | "";
    if (sceneId <= 0 || chapterId <= 0 || revision[0] == '\0') {
      result.manifest = {};
      result.status = ParseStatus::InvalidScene;
      return result;
    }

    result.manifest.scenes.push_back(
        {std::string(pair.key().c_str()), sceneId, chapterId, std::string(revision)});
  }

  result.status = ParseStatus::Ok;
  return result;
}

std::string serializeManifest(const Manifest& manifest) {
  JsonDocument doc;
  doc["version"] = kManifestVersion;
  doc["provider"] = kProvider;
  doc["project_id"] = manifest.projectId;
  doc["project_name"] = manifest.projectName;

  JsonObject scenes = doc["scenes"].to<JsonObject>();
  for (const SceneBinding& scene : manifest.scenes) {
    JsonObject value = scenes[scene.filename].to<JsonObject>();
    value["scene_id"] = scene.sceneId;
    value["chapter_id"] = scene.chapterId;
    value["revision"] = scene.revision;
  }

  std::string output;
  serializeJsonPretty(doc, output);
  return output;
}

}  // namespace e213::sync
