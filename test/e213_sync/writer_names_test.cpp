#include "../../src/targets/heltec_e213_writer/WriterNames.h"
#include <cassert>

int main() {
  using namespace writernames;
  assert(filename("Chapter 1") == "Chapter 1.txt");
  assert(filename("The Long Night") == "The Long Night.txt");
  assert(filename("A-B_9") == "A-B_9.txt");
  assert(filename("") == "");
  assert(filename(" ") == "");
  assert(filename(" bad") == "");
  assert(filename("bad ") == "");
  assert(filename("bad/name") == "");
  assert(filename("../escape") == "");
  assert(filename("bad.txt") == "");
  assert(filename(std::string(45, 'a')) == "");
  assert(filename(std::string(44, 'a')).size() == 48);
  assert(nameTaken("Chapter 1.txt", "chapter 1.TXT"));
  assert(!nameTaken("Chapter 1.txt", "Chapter 2.txt"));
  assert(stem("Chapter 1.txt") == "Chapter 1");
}
