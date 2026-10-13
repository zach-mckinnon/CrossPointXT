#include "../../src/targets/heltec_e213_writer/WriterOutline.h"
#include <cassert>
#include <string>
#include <vector>

int main() {
  using writeroutline::Section;
  const std::vector<std::string> alphabetical{
    "Act One.txt", "Alpha.txt", "Draft-001.txt", "Zebra.txt"
  };
  const std::vector<Section> plotOrder{
    {"Chapter 2 - Later", {"Zebra.txt", "Alpha.txt"}},
    {"Chapter 1 - First", {"Act One.txt"}}
  };
  const auto grouped = writeroutline::organize(alphabetical, plotOrder);
  assert(grouped.size() == 3);
  assert(grouped[0].title == "Chapter 2 - Later");
  assert(grouped[0].files.size() == 2);
  assert(grouped[0].files[0] == "Zebra.txt");
  assert(grouped[0].files[1] == "Alpha.txt");
  assert(grouped[1].title == "Chapter 1 - First");
  assert(grouped[1].files[0] == "Act One.txt");
  assert(grouped[2].title == "Local drafts");
  assert(grouped[2].files[0] == "Draft-001.txt");

  const auto empty = writeroutline::organize({"Draft-001.txt", "Notes.txt"}, {});
  assert(empty.size() == 1);
  assert(empty[0].files.size() == 2);
  assert(empty[0].title == "Local drafts");

  const auto malformed = writeroutline::organize({"Safe.txt"}, {
    {"Ghost", {"Missing.txt"}},
    {"Repeated", {"Safe.txt", "Safe.txt"}},
    {"Again", {"Safe.txt"}}
  });
  assert(malformed.size() == 1);
  assert(malformed[0].title == "Repeated");
  assert(malformed[0].files.size() == 1);
  assert(malformed[0].files[0] == "Safe.txt");

  // The browser must keep personal, non-synced drafts separate from each
  // Plot n Jot project's scenes, even if the TXT filenames are alphabetical.
  using writeroutline::Project;
  const std::vector<Project> projects{
    {"Novel Two", {Section{"Ending", {"Zebra.txt"}}}, false},
    {"Novel One", {Section{"Beginnings", {"Act One.txt", "Alpha.txt"}}}, false}
  };
  const auto tree = writeroutline::organizeProjects(alphabetical, projects);
  assert(tree.size() == 3);
  assert(tree[0].personal && tree[0].title == "My Documents");
  assert(tree[0].chapters[0].files.size() == 1);
  assert(tree[0].chapters[0].files[0] == "Draft-001.txt");
  assert(tree[1].title == "Novel Two");
  assert(tree[1].chapters[0].title == "Ending");
  assert(tree[1].chapters[0].files[0] == "Zebra.txt");
  assert(tree[2].title == "Novel One");
  assert(tree[2].chapters[0].files[0] == "Act One.txt");
  assert(tree[2].chapters[0].files[1] == "Alpha.txt");

  const auto duplicate = writeroutline::organizeProjects(
      {"One.txt"}, {
        {"First", {Section{"C1", {"One.txt"}}}, false},
        {"Second", {Section{"C2", {"One.txt"}}}, false}
      });
  assert(duplicate.size() == 2);
  assert(duplicate[0].personal && duplicate[0].chapters[0].files.empty());
  assert(duplicate[1].title == "First");

  const auto noFiles = writeroutline::organize({}, plotOrder);
  assert(noFiles.empty());
}
