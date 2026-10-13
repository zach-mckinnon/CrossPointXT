#include "SyncManifest.h"
#include <algorithm>
#include <cstring>

namespace e213::sync {

ParseResult parseManifest(const std::string& json) {
  ParseResult result;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    result.status = ParseStatus::InvalidJson;
    return result;
  }

  const int version = doc["version"] | 0;
  if (version != kManifestVersion && version != 2) {
    result.status = ParseStatus::UnsupportedVersion;
    return result;
  }

  const char* provider = doc["provider"] | "";
  if (std::string(provider) != kProvider) {
    result.status = ParseStatus::WrongProvider;
    return result;
  }

  // Version 2 stores independent projects in one checked SD manifest.
  // The old version 1 projects remain readable without a card migration.
  if (version == 2) {
    if (!doc["projects"].is<JsonArray>()) {
      result.status = ParseStatus::MissingProjectId;
      return result;
    }
    JsonArray projects = doc["projects"].as<JsonArray>();
    if (projects.size() == 0) {
      result.status = ParseStatus::MissingProjectId;
      return result;
    }
    std::vector<int64_t> projectIds;
    std::vector<std::string> filenames;
    for (JsonVariant entry : projects) {
      if (!entry.is<JsonObject>()) {
        result.status = ParseStatus::InvalidScene;
        return result;
      }
      JsonObject project = entry.as<JsonObject>();
      const int64_t id = project["project_id"] | static_cast<int64_t>(0);
      if (id <= 0 ||
          std::find(projectIds.begin(), projectIds.end(), id) != projectIds.end() ||
          !project["scenes"].is<JsonObject>()) {
        result.status = ParseStatus::MissingProjectId;
        return result;
      }
      projectIds.push_back(id);
      if (projectIds.size() == 1) {
        result.manifest.projectId = id;
        result.manifest.projectName = std::string(project["project_name"] | "");
      }
      for (JsonPair pair : project["scenes"].as<JsonObject>()) {
        const std::string filename = pair.key().c_str();
        const auto isAllowedName = [&]() {
          if (filename.size() < 5 || filename.size() > 48 ||
              filename.compare(filename.size() - 4, 4, ".txt") != 0)
            return false;
          for (size_t i = 0; i + 4 < filename.size(); ++i) {
            const char c = filename[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-'))
              return false;
          }
          return true;
        };
        if (!isAllowedName() ||
            std::find(filenames.begin(), filenames.end(), filename) != filenames.end() ||
            !pair.value().is<JsonObject>()) {
          result.status = ParseStatus::InvalidScene;
          return result;
        }
        filenames.push_back(filename);
        JsonObject binding = pair.value().as<JsonObject>();
        const int64_t sceneId = binding["scene_id"] | static_cast<int64_t>(0);
        const int64_t chapterId = binding["chapter_id"] | static_cast<int64_t>(0);
        const char* revision = binding["revision"] | "";
        if (sceneId <= 0 || chapterId <= 0 || strlen(revision) != 64) {
          result.status = ParseStatus::InvalidScene;
          return result;
        }
        result.manifest.scenes.push_back({filename, sceneId, chapterId, revision});
      }
    }
    result.manifest.version = 2;
    result.manifest.provider = provider;
    result.status = ParseStatus::Ok;
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
