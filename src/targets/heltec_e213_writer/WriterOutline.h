#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace writeroutline {

// The firmware uses the optional Plot n Jot chapter order from /.sync only
// for display. TXT files and authoritative sync links are never moved.
struct Section {
  std::string title;
  std::vector<std::string> files;
};

inline std::vector<Section> organize(const std::vector<std::string>& cardFiles,
                                      const std::vector<Section>& outline) {
  std::vector<Section> organized;
  std::vector<std::string> used;
  for (const auto& desired : outline) {
    Section section{desired.title.empty() ? "Untitled chapter" : desired.title, {}};
    for (const auto& filename : desired.files) {
      if (std::find(cardFiles.begin(), cardFiles.end(), filename) != cardFiles.end()
          && std::find(used.begin(), used.end(), filename) == used.end()) {
        section.files.push_back(filename);
        used.push_back(filename);
      }
    }
    if (!section.files.empty()) organized.push_back(section);
  }
  Section local{"Local drafts", {}};
  for (const auto& filename : cardFiles) {
    if (std::find(used.begin(), used.end(), filename) == used.end())
      local.files.push_back(filename);
  }
  if (!local.files.empty()) organized.push_back(local);
  return organized;
}


struct Project {
  std::string title;
  std::vector<Section> chapters;
  bool personal = false;
};

// Projects and chapters are display-only folders. The actual SD /drafts
// files and their device/desktop sync revisions never move or change.
// A file may appear only once, even if metadata is inconsistent.
inline std::vector<Project> organizeProjects(
    const std::vector<std::string>& cardFiles,
    const std::vector<Project>& orderedProjects) {
  std::vector<Project> results;
  std::vector<std::string> used;
  for (const Project& source : orderedProjects) {
    if (source.personal) continue;
    Project selected{source.title.empty() ? "Untitled project" : source.title,
                     {}, false};
    for (const Section& group : source.chapters) {
      Section chapter{group.title.empty() ? "Untitled chapter" : group.title, {}};
      for (const std::string& name : group.files) {
        if (std::find(cardFiles.begin(), cardFiles.end(), name) != cardFiles.end()
            && std::find(used.begin(), used.end(), name) == used.end()) {
          chapter.files.push_back(name);
          used.push_back(name);
        }
      }
      if (!chapter.files.empty()) selected.chapters.push_back(chapter);
    }
    if (!selected.chapters.empty()) results.push_back(selected);
  }
  Project personal{"My Documents", {Section{"My Documents", {}}}, true};
  for (const std::string& name : cardFiles) {
    if (std::find(used.begin(), used.end(), name) == used.end())
      personal.chapters[0].files.push_back(name);
  }
  results.insert(results.begin(), personal);
  return results;
}

}  // namespace writeroutline
