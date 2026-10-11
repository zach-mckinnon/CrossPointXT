#include "../../src/targets/heltec_e213_writer/WriterKeyRepeat.h"
#include <cassert>
int main() {
  using namespace writerrepeat;
  static_assert(firstRepeatDelayMs == 450, "Start after deliberate hold");
  static_assert(interval(0) == 135, "Initial repeat");
  static_assert(interval(1000) == 135, "Initial acceleration threshold");
  static_assert(interval(1001) == 80, "First acceleration");
  static_assert(interval(2000) == 80, "Second threshold");
  static_assert(interval(2001) == 40, "Max acceleration");
  assert(interval(5000) == 40);
}
