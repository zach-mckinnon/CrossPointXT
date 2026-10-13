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

}  // namespace writeroutline
