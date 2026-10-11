#include "../../src/targets/heltec_e213_writer/WriterStore.h"
#include "../../src/targets/heltec_e213_writer/WriterButton.h"
#include <HalStorage.h>
#include <LittleFS.h>
#include <cassert>
#include <iostream>

static WriterStore first, rebooted;
void reset() {
  Storage = {};
  LittleFS = {};
  first = {};
  rebooted = {};
}
void edit(WriterStore& store, const char* text) {
  store.text.assign(text, strlen(text));
  store.edited();
}
int main() {
  reset();
  LittleFS.files["/draft.txt"] = "Legacy draft";
  assert(first.begin());
  assert(first.text.size() == 12);
  assert(first.save());
  const std::string path = first.path();
  assert(Storage.files[path] == "Legacy draft");
  assert(LittleFS.files["/draft.txt"] == "Legacy draft");
  edit(first, "Saved to SD");
  assert(first.save());
  assert(Storage.files[path] == "Saved to SD");
  assert(Storage.files[path + ".e213-bak"] == "Legacy draft");
  assert(rebooted.begin());
  assert(std::string(rebooted.text.data()) == "Saved to SD");
  assert(path == rebooted.path());

  // A failed write leaves the old SD version and the new recovery checkpoint.
  edit(first, "New unsaved text");
  Storage.failWrite = true;
  assert(!first.save());
  assert(Storage.files[path] == "Saved to SD");
  rebooted = {};
  assert(rebooted.begin());
  assert(std::string(rebooted.text.data()) == "New unsaved text");
  Storage.failWrite = false;
  assert(rebooted.save());
  assert(Storage.files[path] == "New unsaved text");

  // A failed sync cannot publish a replacement.
  edit(rebooted, "Sync failure");
  Storage.failSync = true;
  assert(!rebooted.save());
  assert(Storage.files[path] == "New unsaved text");
  Storage.failSync = false;

  // Simulate power loss between old->backup and temp->target.
  Storage.files[path + ".e213-bak"] = Storage.files[path];
  Storage.files.erase(path);
  first = {};
  assert(first.begin());
  assert(first.save());
  assert(Storage.files[path] == "Sync failure");

  // Failed final rename rolls back to the previous good SD file.
  edit(first, "Rename failure");
  Storage.failRenameSource = path + ".e213-tmp";
  assert(!first.save());
  assert(Storage.files[path] == "Sync failure");
  Storage.failRenameSource.clear();
  assert(first.save());
  assert(Storage.files[path] == "Rename failure");

  // PC changes cannot be overwritten by pending edits recovered from flash.
  edit(first, "Device edit");
  Storage.files[path] = "Desktop edit";
  assert(!first.save());
  assert(Storage.files[path] == "Desktop edit");
  rebooted = {};
  assert(rebooted.begin());
  assert(std::string(rebooted.text.data()) == "Device edit");
  assert(!rebooted.save());

  // Missing card: scratch still saves internally and remains clean.
  reset();
  Storage.mounted = false;
  assert(first.begin());
  edit(first, "No SD");
  assert(first.save());
  assert(!first.dirty());
  assert(rebooted.begin());
  assert(std::string(rebooted.text.data()) == "No SD");

  // File-open failures never destroy the current document.
  reset();
  assert(first.begin());
  edit(first, "Keep me");
  assert(first.save());
  Storage.files["/large.txt"] = std::string(65537, 'x');
  Storage.files["/binary.txt"] = std::string("bad\0text", 8);
  assert(!first.open("/large.txt"));
  assert(!first.open("/binary.txt"));
  assert(std::string(first.text.data()) == "Keep me");
  Storage.files["/other.txt"] = "Other file\r\n";
  LittleFS.failRename = true;
  assert(!first.open("/other.txt"));
  assert(std::string(first.text.data()) == "Keep me");
  LittleFS.failRename = false;
  assert(first.open("/other.txt"));
  assert(std::string(first.text.data()) == "Other file\r\n");

  // Clean session loads externally updated SD content at boot.
  Storage.files["/other.txt"] = "Updated on PC";
  rebooted = {};
  assert(rebooted.begin());
  assert(std::string(rebooted.text.data()) == "Updated on PC");

  // Corrupted recovery is retained, never silently replaced with an empty draft.
  LittleFS.files["/writer-session.dat"] = "Broken checkpoint";
  rebooted = {};
  assert(!rebooted.begin());
  assert(!rebooted.save());
  assert(LittleFS.files["/writer-session.dat"] == "Broken checkpoint");

  // UTF-8 boundary editing and a worst-case newline-heavy document.
  reset();
  first.text.assign("A\xc3\xa9Z", 4);
  assert(nextCharacter(first.text, 1) == 3);
  assert(previousCharacter(first.text, 3) == 1);
  first.text.erase(1, 2);
  assert(std::string(first.text.data()) == "AZ");
  assert(!validWriterText("\xc0\xaf", 2));
  assert(!validWriterText("\xed\xa0\x80", 3));
  assert(validWriterText("\xf0\x9f\x8c\xbf", 4));
  memset(first.text.bytes, '\n', WriterText::CAPACITY);
  first.text.length = WriterText::CAPACITY;
  size_t at = 0, rows = 0;
  while (true) {
    const auto line = writerLine(first.text, at);
    ++rows;
    if (!line.more) break;
    assert(line.next > at);
    at = line.next;
  }
  assert(rows == 65537);
  WriterButton button;
  assert(button.update(true, 100) == ButtonEvent::None);
  assert(button.update(false, 110) == ButtonEvent::None); // bounce
  assert(button.update(true, 120) == ButtonEvent::None);
  assert(button.update(true, 155) == ButtonEvent::None);
  assert(button.update(false, 200) == ButtonEvent::None);
  assert(button.update(false, 235) == ButtonEvent::Short);
  assert(button.update(false, 400) == ButtonEvent::None);
  assert(button.update(true, 1000) == ButtonEvent::None);
  assert(button.update(true, 1035) == ButtonEvent::None);
  assert(button.update(true, 3000) == ButtonEvent::None);
  assert(button.update(false, 3100) == ButtonEvent::None);
  assert(button.update(false, 3135) == ButtonEvent::Long);
  assert(button.update(false, 3200) == ButtonEvent::None);
  std::cout << "Writer storage, recovery, UTF-8 and bounded layout tests passed\n";
}
