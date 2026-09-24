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

  std::printf("decklink monitor mode test: OK\n");
  return 0;
}
