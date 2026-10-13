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

  const auto noFiles = writeroutline::organize({}, plotOrder);
  assert(noFiles.empty());
}
