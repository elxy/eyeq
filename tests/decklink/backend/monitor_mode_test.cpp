// CPU tests for integer-rational monitor mode formatting and matching. No SDK,
// no GPU, no device.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "output/decklink/monitor_mode.hpp"

using namespace EYEQ;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

int main() {
  const MonitorMode p50{3840, 2160, 1000, 50000, true};
  CHECK(FormatMonitorMode(p50) == "3840x2160@50p");

  const MonitorMode p2997{1920, 1080, 1001, 30000, true};
  CHECK(FormatMonitorMode(p2997) == "1920x1080@30000/1001p");

  const MonitorMode i50{1920, 1080, 1000, 50000, false};
  CHECK(FormatMonitorMode(i50) == "1920x1080@50i");

  const MonitorMode unknown{1280, 720, 0, 0, true};
  CHECK(FormatMonitorMode(unknown) == "1280x720@?p");

  CHECK(MonitorModeIs(p50, 3840, 2160, 50, 1));
  CHECK(MonitorModeIs(p50, 3840, 2160, 100, 2)); // same rate, unreduced fraction
  CHECK(!MonitorModeIs(p50, 3840, 2160, 30000, 1001));
  CHECK(!MonitorModeIs(p50, 1920, 2160, 50, 1));
  CHECK(MonitorModeIs(p2997, 1920, 1080, 30000, 1001));
  CHECK(!MonitorModeIs(p2997, 1920, 1080, 30, 1));
  CHECK(!MonitorModeIs(p50, 3840, 2160, 0, 1)); // invalid rational never matches

  // F4: the comparison must not overflow signed int64 for large, legal rationals.
  // duration=2^32, scale=2^33 is 2 fps; 2^33/2^33 is 1 fps. Cross-multiplication
  // overflows both sides and used to report a false match.
  const MonitorMode big{3840, 2160, int64_t{1} << 32, int64_t{1} << 33, true};
  CHECK(!MonitorModeIs(big, 3840, 2160, int64_t{1} << 33, int64_t{1} << 33));
  CHECK(MonitorModeIs(big, 3840, 2160, 2, 1));
  CHECK(MonitorModeIs(big, 3840, 2160, int64_t{1} << 30, int64_t{1} << 29));
  CHECK(!MonitorModeIs(big, 3840, 2160, 3, 1));

  // INT64_MAX bounds: unreduced huge rationals are still compared exactly.
  const MonitorMode max_rate{3840, 2160, INT64_MAX, INT64_MAX, true};
  CHECK(MonitorModeIs(max_rate, 3840, 2160, 1, 1));
  CHECK(MonitorModeIs(max_rate, 3840, 2160, INT64_MAX, INT64_MAX));
  CHECK(!MonitorModeIs(max_rate, 3840, 2160, INT64_MAX, 1));
  CHECK(!MonitorModeIs(max_rate, 3840, 2160, 2, 1));
  const MonitorMode max_scale{3840, 2160, 1, INT64_MAX, true};
  CHECK(MonitorModeIs(max_scale, 3840, 2160, INT64_MAX, 1));
  CHECK(!MonitorModeIs(max_scale, 3840, 2160, 1, 1));

  // Zero and negative rationals never match, and a broken mode never matches.
  CHECK(!MonitorModeIs(p50, 3840, 2160, -50, 1));
  CHECK(!MonitorModeIs(p50, 3840, 2160, 50, -1));
  CHECK(!MonitorModeIs(p50, 3840, 2160, 50, 0));
  CHECK(!MonitorModeIs(MonitorMode{3840, 2160, -1000, 50000, true}, 3840, 2160, 50, 1));
  CHECK(!MonitorModeIs(MonitorMode{3840, 2160, 1000, -50000, true}, 3840, 2160, 50, 1));

  std::printf("decklink monitor mode test: OK\n");
  return 0;
}
