// Strict CLI device-selector parsing tests. No SDK, no GPU, no device.
//
// The catalog tool must never guess whether a value is an index or a persistent id:
// a persistent id is usually a plain decimal number. These tests pin the explicit
// index:/id: syntax and its rejection of empty, malformed and out-of-range input.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "device_selector.hpp"

using namespace EYEQ;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

DeviceId selector;
std::string error;

// Parses and asserts success, returning the selector by value.
DeviceId Accepted(const char *text) {
  error.clear();
  CHECK(ParseDeviceSelector(text, &selector, &error));
  CHECK(error.empty());
  return selector;
}

void Rejected(const char *text) {
  error.clear();
  DeviceId unset;
  unset.index = -42;
  unset.persistent_id = "sentinel";
  CHECK(!ParseDeviceSelector(text, &unset, &error));
  CHECK(!error.empty());
  // A rejected selector must not leave a usable selector behind.
  CHECK(unset.index == -1);
  CHECK(unset.persistent_id.empty());
}

} // namespace

int main() {
  // Index selectors: non-negative, bounded by int.
  CHECK(Accepted("index:0").index == 0);
  CHECK(Accepted("index:1").index == 1);
  CHECK(Accepted("index:2147483647").index == 2147483647);
  CHECK(Accepted("index:007").index == 7); // leading zeros are accepted and normalized
  CHECK(Accepted("index:0").persistent_id.empty());

  // Persistent-id selectors: full signed int64 range, including the large ids real
  // devices report (2697367084 > INT32_MAX) and negative/large values.
  CHECK(Accepted("id:123456789").persistent_id == "123456789");
  CHECK(Accepted("id:2697367084").persistent_id == "2697367084");
  CHECK(Accepted("id:4294967296").persistent_id == "4294967296");
  CHECK(Accepted("id:-5").persistent_id == "-5");
  CHECK(Accepted("id:9223372036854775807").persistent_id == "9223372036854775807");
  CHECK(Accepted("id:-9223372036854775808").persistent_id == "-9223372036854775808");
  CHECK(Accepted("id:00123").persistent_id == "123"); // canonical form for matching
  CHECK(Accepted("id:2697367084").index == -1);

  // Rejections: nothing is guessed or silently reinterpreted.
  Rejected("");
  Rejected("0");          // bare number: ambiguous, never treated as an index
  Rejected("1234567890"); // same, even when a persistent id would match it
  Rejected("index:");
  Rejected("id:");
  Rejected(":1");
  Rejected("index:abc");
  Rejected("index:-1");
  Rejected("index:+1"); // no '+' sign in the strict grammar
  Rejected("index:1x");
  Rejected("index:1.0");
  Rejected("index: 1");
  Rejected("index:2147483648"); // one past INT_MAX
  Rejected("index:4294967296");
  Rejected("index:99999999999999999999");
  Rejected("index:1:2");
  Rejected("id:abc");
  Rejected("id:1.5");
  Rejected("id: 123");
  Rejected("id:9223372036854775808");  // one past INT64_MAX
  Rejected("id:-9223372036854775809"); // one below INT64_MIN
  Rejected("foo:1");
  Rejected("device:0");

  std::printf("decklink device selector test: OK\n");
  return 0;
}
