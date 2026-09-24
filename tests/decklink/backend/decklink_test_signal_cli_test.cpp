// CLI regression for decklink-test-signal: argument handling, arithmetic rejection
// and shutdown behavior without a device. main.cpp is renamed and invoked in-process,
// and the fake SDK supplies a deterministic device/mode list. No output dispatch is
// linked.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "DeckLinkAPI.h"
#include "fake_decklink_output.hpp"

#define main test_signal_cli_main
#include "../../../tools/decklink-test-signal/main.cpp"
#undef main

using namespace eyeq_decklink_test;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

int RunCli(const std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  argv.push_back(const_cast<char *>("decklink-test-signal"));
  for (const std::string &arg : args) {
    argv.push_back(const_cast<char *>(arg.c_str()));
  }
  return test_signal_cli_main(static_cast<int>(argv.size()), argv.data());
}

// Runs the CLI with stdout captured so the printed summary can be asserted.
int RunCliCapture(const std::vector<std::string> &args, std::string *captured) {
  std::FILE *tmp = std::tmpfile();
  if (tmp == nullptr) {
    std::abort();
  }
  std::fflush(stdout);
  std::FILE *saved = stdout;
  stdout = tmp;
  const int code = RunCli(args);
  std::fflush(stdout);
  stdout = saved;
  std::rewind(tmp);
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), tmp) != nullptr) {
    *captured += buffer;
  }
  std::fclose(tmp);
  return code;
}

void RaiseSigint(void *) { std::raise(SIGINT); }

void Prepare() {
  ResetFakeOutput();
  EYEQ::test_signal_stop_requested = 0;
}

} // namespace

int main() {
  Prepare();

  // --help succeeds and prints usage.
  CHECK(RunCli({"--help"}) == 0);
  // A missing --device is a usage error.
  CHECK(RunCli({}) == 1);
  // A malformed selector is rejected instead of guessed.
  CHECK(RunCli({"--device", "bogus"}) == 1);
  CHECK(RunCli({"--device", "0"}) == 1);
  // The fake device only advertises 3840x2160 50p, so an explicit unsupported mode
  // must be rejected before any output is opened.
  CHECK(RunCli({"--device", "index:0", "--mode", "1280x720@60"}) == 1);
  // --frames and --seconds are mutually exclusive.
  CHECK(RunCli({"--device", "index:0", "--frames", "10", "--seconds", "2"}) == 1);

  // Arithmetic that would otherwise silently narrow or wrap must be rejected before
  // Enumerate/Open, so the device is never touched.
  Prepare();
  CHECK(RunCli({"--device", "index:0", "--mode", "4294971136x2160@50", "--frames", "1"}) == 1);
  CHECK(OutputStats().enable_calls == 0);
  Prepare();
  CHECK(RunCli({"--device", "index:0", "--drain-timeout-ms", "4294967296", "--frames", "1"}) == 1);
  CHECK(OutputStats().enable_calls == 0);
  Prepare();
  CHECK(RunCli({"--device", "index:0", "--seconds", "922337203685477581"}) == 1);
  CHECK(OutputStats().enable_calls == 0);
  Prepare();
  CHECK(RunCli({"--device", "index:0", "--frames", "9223372036854775807"}) == 1);
  CHECK(OutputStats().enable_calls == 0);

  // Positive SDI/HDMI still paths reach Enable and record the connection passed to
  // the native support query.
  Prepare();
  CHECK(RunCli({"--device", "index:0", "--connection", "sdi", "--still"}) == 0);
  CHECK(OutputStats().support_connection == static_cast<int>(bmdVideoConnectionSDI));
  CHECK(OutputStats().enable_calls == 1);
  CHECK(OutputStats().disable_calls == 1);
  Prepare();
  CHECK(RunCli({"--device", "index:0", "--connection", "hdmi", "--still"}) == 0);
  CHECK(OutputStats().support_connection == static_cast<int>(bmdVideoConnectionHDMI));
  CHECK(OutputStats().enable_calls == 1);
  CHECK(OutputStats().disable_calls == 1);

  // Real SIGINT: the interrupt stops new submissions, the retained completion is
  // still counted before the process exits cleanly.
  Prepare();
  SetScheduleReentrant(true);
  SetCompletionGate(RaiseSigint, nullptr);
  std::string captured;
  CHECK(RunCliCapture({"--device", "index:0", "--frames", "20", "--preroll", "1"}, &captured) == 0);
  CHECK(captured.find("completed=1") != std::string::npos);
  CHECK(OutputStats().disable_calls == 1);
  SetCompletionGate(nullptr, nullptr);

  // A failed native stop must not be hidden by a successful Close.
  Prepare();
  SetScheduleReentrant(true);
  SetOutputFault(OutputFault::StopFails);
  CHECK(RunCli({"--device", "index:0", "--frames", "1"}) == 1);
  SetOutputFault(OutputFault::None);

  // SIGINT coinciding with a real device error must not be reclassified as success.
  Prepare();
  SetScheduleReentrant(true);
  SetCompletionGate(RaiseSigint, nullptr);
  SetOutputFault(OutputFault::ScheduleFails);
  CHECK(RunCli({"--device", "index:0", "--frames", "1"}) == 1);
  SetCompletionGate(nullptr, nullptr);
  SetOutputFault(OutputFault::None);
  EYEQ::test_signal_stop_requested = 0;

  std::printf("decklink_test_signal_cli: OK\n");
  return 0;
}
