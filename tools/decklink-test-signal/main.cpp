// Standalone DeckLink test-signal tool.
//
// It renders a deterministic CPU test pattern and sends it to a DeckLink output
// through the existing FrameSink backend, without the EyeQ player and without the
// GPU packer. It is meant for on-hardware verification of SDI/HDMI links, mode
// timing, range/quantization and start/stop behavior.
//
// The executable enables real hardware output. Current automated validation only
// exercises it against a fake SDK; that is a verification status, not a runtime
// behavior. When it runs it takes exclusive control of the selected output; if the
// device is busy it reports the failure and exits, and it never terminates or
// preempts another application.
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "device_selector.hpp"
#include "output/decklink/monitor_mode.hpp"
#include "output/decklink_backend.hpp"
#include "test_pattern.hpp"
#include "test_signal_session.hpp"

namespace {

struct ParsedMode {
  bool is_auto = true;
  int width = 0;
  int height = 0;
  int64_t numerator = 0;
  int64_t denominator = 1;
};

void PrintUsage(const char *program) {
  std::printf("Usage: %s [options]\n"
              "\n"
              "Deterministic DeckLink test-signal generator. Renders a CPU pattern and\n"
              "outputs it through the DeckLink backend. Requires exclusive use of the\n"
              "device while running; if the device is busy it only reports failure.\n"
              "\n"
              "Options:\n"
              "  --device <selector>            Required. index:<n> or id:<persistent-id>\n"
              "  --connection <sdi|hdmi>        Output connection (default: sdi)\n"
              "  --pixel-format <r210|v210>     Pixel format (default: v210)\n"
              "  --mode <WxH@fps|auto>          Output mode (default: auto). auto prefers\n"
              "                                 3840x2160 50p progressive; a mode must be in\n"
              "                                 the device's native ListModes result\n"
              "  --hdr                          Request BT2020/PQ + HDR signaling\n"
              "                                 (default: SDR BT709/BT1886)\n"
              "  --frames <n>                   Number of frames (mutually exclusive with\n"
              "                                 --seconds)\n"
              "  --seconds <s>                  Number of seconds (default: 5); converted to\n"
              "                                 frames with the mode's rational time base\n"
              "  --preroll <n>                  Frames queued before StartScheduled\n"
              "                                 (default: 4, capped to the frame count; if\n"
              "                                 the device pool fills first, playback starts\n"
              "                                 early rather than waiting for completions)\n"
              "  --drain-timeout-ms <n>         Drain/retry timeout in milliseconds\n"
              "                                 (default: 5000)\n"
              "  --still                        Show one still frame and exit; no start and no\n"
              "                                 scheduling (ignores --frames/--seconds)\n"
              "  -h, --help                     Show this help\n"
              "\n"
              "Exit codes: 0 success, 1 usage/request/run error, 2 DeckLink driver unavailable.\n",
              program);
}

bool ParseInt64(const std::string &text, int64_t *out) {
  if (text.empty()) {
    return false;
  }
  int64_t value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *out = value;
  return true;
}

bool ParseUint64(const std::string &text, uint64_t *out) {
  int64_t value = 0;
  if (!ParseInt64(text, &value) || value < 0) {
    return false;
  }
  *out = static_cast<uint64_t>(value);
  return true;
}

bool ParseConnection(const std::string &text, EYEQ::VideoConnection *out) {
  if (text == "sdi") {
    *out = EYEQ::VideoConnection::SDI;
    return true;
  }
  if (text == "hdmi") {
    *out = EYEQ::VideoConnection::HDMI;
    return true;
  }
  return false;
}

bool ParsePixelFormat(const std::string &text, EYEQ::PixelFormat *out) {
  if (text == "r210") {
    *out = EYEQ::PixelFormat::R210;
    return true;
  }
  if (text == "v210") {
    *out = EYEQ::PixelFormat::V210;
    return true;
  }
  return false;
}

// Parses "WxH@num" or "WxH@num/den" (for example 3840x2160@50 or 1920x1080@30000/1001).
bool ParseMode(const std::string &text, ParsedMode *out) {
  if (text == "auto") {
    *out = ParsedMode();
    return true;
  }
  const std::size_t x = text.find('x');
  const std::size_t at = text.find('@');
  if (x == std::string::npos || at == std::string::npos || x == 0 || at <= x + 1 || at + 1 >= text.size()) {
    return false;
  }
  int64_t width = 0;
  int64_t height = 0;
  if (!ParseInt64(text.substr(0, x), &width) || !ParseInt64(text.substr(x + 1, at - x - 1), &height)) {
    return false;
  }
  const std::string rate = text.substr(at + 1);
  int64_t numerator = 0;
  int64_t denominator = 1;
  const std::size_t slash = rate.find('/');
  if (slash == std::string::npos) {
    if (!ParseInt64(rate, &numerator)) {
      return false;
    }
  } else {
    if (!ParseInt64(rate.substr(0, slash), &numerator) || !ParseInt64(rate.substr(slash + 1), &denominator)) {
      return false;
    }
  }
  if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
      height > std::numeric_limits<int>::max() || numerator <= 0 || denominator <= 0) {
    return false;
  }
  out->is_auto = false;
  out->width = static_cast<int>(width);
  out->height = static_cast<int>(height);
  out->numerator = numerator;
  out->denominator = denominator;
  return true;
}

const char *ConnectionLabel(EYEQ::VideoConnection connection) {
  return connection == EYEQ::VideoConnection::SDI ? "sdi" : "hdmi";
}

const char *PixelFormatLabel(EYEQ::PixelFormat format) { return format == EYEQ::PixelFormat::R210 ? "r210" : "v210"; }

// Async-signal-safe: set the stop flag and nothing else (no SDK, logging, locks or
// allocation). The consumer thread observes the flag and closes the output normally.
void HandleStopSignal(int) { EYEQ::test_signal_stop_requested = 1; }

void PrintModes(const std::vector<EYEQ::MonitorMode> &modes) {
  std::fprintf(stderr, "Available modes:\n");
  for (const EYEQ::MonitorMode &mode : modes) {
    std::fprintf(stderr, "  %s\n", EYEQ::FormatMonitorMode(mode).c_str());
  }
}

// Selects a mode from the native list. An explicit mode must match exactly; no mode
// is ever constructed that ListModes did not return.
bool SelectMode(const std::vector<EYEQ::MonitorMode> &modes, const ParsedMode &requested, EYEQ::MonitorMode *out,
                std::string *error) {
  if (requested.is_auto) {
    for (const EYEQ::MonitorMode &mode : modes) {
      if (mode.is_progressive && EYEQ::MonitorModeIs(mode, 3840, 2160, 50, 1)) {
        *out = mode;
        return true;
      }
    }
    *error = "no 3840x2160 50p progressive mode is natively supported; use --mode <WxH@fps>";
    return false;
  }
  for (const EYEQ::MonitorMode &mode : modes) {
    if (mode.is_progressive &&
        EYEQ::MonitorModeIs(mode, requested.width, requested.height, requested.numerator, requested.denominator)) {
      *out = mode;
      return true;
    }
  }
  *error = "requested mode is not natively supported by this device";
  return false;
}

// Common tool finalization. CloseStatus distinguishes a clean shutdown from a
// device that still could not be closed; errors keeps the real event identity.
struct FinalizeReport {
  EYEQ::CloseStatus close = EYEQ::CloseStatus::Closed;
  std::vector<std::string> errors;
};

const char *StageName(EYEQ::OutputEvent::Stage stage) {
  using Stage = EYEQ::OutputEvent::Stage;
  switch (stage) {
  case Stage::None:
    return "None";
  case Stage::Negotiate:
    return "Negotiate";
  case Stage::Enable:
    return "Enable";
  case Stage::Access:
    return "Access";
  case Stage::Metadata:
    return "Metadata";
  case Stage::Schedule:
    return "Schedule";
  case Stage::DisplayStill:
    return "DisplayStill";
  case Stage::Start:
    return "Start";
  case Stage::Stop:
    return "Stop";
  case Stage::Close:
    return "Close";
  }
  return "Unknown";
}

// Calls Close and then consumes every event the backend retained. PollEvent returns
// each event exactly once, so completion counters can be added without double
// counting and the real stage/HRESULT/identity survives into diagnostics.
void Finalize(EYEQ::DeckLinkOutput &output, int drain_timeout_ms, EYEQ::TestSignalResult *result,
              FinalizeReport *report) {
  report->close = output.Close(drain_timeout_ms);
  EYEQ::OutputEvent event;
  while (output.PollEvent(&event)) {
    switch (event.kind) {
    case EYEQ::OutputEvent::Kind::FrameCompleted:
      switch (event.result) {
      case EYEQ::CompletionResult::Completed:
        ++result->completed;
        break;
      case EYEQ::CompletionResult::DisplayedLate:
        ++result->late;
        break;
      case EYEQ::CompletionResult::Dropped:
        ++result->dropped;
        break;
      case EYEQ::CompletionResult::Flushed:
        ++result->flushed;
        break;
      }
      break;
    case EYEQ::OutputEvent::Kind::Error:
    case EYEQ::OutputEvent::Kind::DeviceLost: {
      char buffer[512];
      std::snprintf(buffer, sizeof(buffer),
                    "%s stage=%s status=0x%08X submit_id=%llu batch_seq=%llu timeline_epoch=%llu output_index=%llu: %s",
                    event.kind == EYEQ::OutputEvent::Kind::DeviceLost ? "DeviceLost" : "Error", StageName(event.stage),
                    static_cast<unsigned>(event.os_status), static_cast<unsigned long long>(event.submit_id),
                    static_cast<unsigned long long>(event.batch_seq),
                    static_cast<unsigned long long>(event.timeline_epoch),
                    static_cast<unsigned long long>(event.output_index), event.message);
      report->errors.emplace_back(buffer);
      break;
    }
    case EYEQ::OutputEvent::Kind::PlaybackStopped:
      break;
    }
  }
}

// Best-effort finalization after a failed Open. The backend may have left a
// retryable closing session, so still attempt Close and print the original failure
// and any shutdown diagnostics before the separate fatal destructor policy applies.
void FinalizeAfterOpenFailure(EYEQ::DeckLinkOutput &output, const std::string &open_error, int drain_timeout_ms) {
  std::fprintf(stderr, "open failed: %s\n", open_error.c_str());
  EYEQ::TestSignalResult empty;
  FinalizeReport report;
  Finalize(output, drain_timeout_ms, &empty, &report);
  for (const std::string &message : report.errors) {
    std::fprintf(stderr, "shutdown diagnostic: %s\n", message.c_str());
  }
  if (report.close != EYEQ::CloseStatus::Closed) {
    std::fprintf(stderr, "close after failed open returned DeviceError\n");
  }
}

void PrintSummary(const EYEQ::DeviceInfo &device, const EYEQ::MonitorMode &mode, EYEQ::VideoConnection connection,
                  EYEQ::PixelFormat format, bool hdr, const EYEQ::TestSignalResult &result,
                  const FinalizeReport &report) {
  std::printf("Device: index %d model=\"%s\"\n", device.index, device.model_name.c_str());
  std::printf("Mode: %s frame_duration=%lld time_scale=%lld\n", EYEQ::FormatMonitorMode(mode).c_str(),
              static_cast<long long>(mode.frame_duration), static_cast<long long>(mode.time_scale));
  std::printf("Connection: %s  Pixel format: %s  HDR: %s\n", ConnectionLabel(connection), PixelFormatLabel(format),
              hdr ? "on" : "off");
  std::printf("Frames: submitted=%llu completed=%llu late=%llu dropped=%llu flushed=%llu started=%s\n",
              static_cast<unsigned long long>(result.submitted), static_cast<unsigned long long>(result.completed),
              static_cast<unsigned long long>(result.late), static_cast<unsigned long long>(result.dropped),
              static_cast<unsigned long long>(result.flushed), result.started ? "yes" : "no");
  std::printf("Close: %s\n", report.close == EYEQ::CloseStatus::Closed ? "closed" : "failed");
  std::printf("Diagnostics: %zu error(s)\n", report.errors.size());
  for (const std::string &message : report.errors) {
    std::printf("  %s\n", message.c_str());
  }
}

} // namespace

int main(int argc, char **argv) {
  EYEQ::test_signal_stop_requested = 0;
  EYEQ::DeviceId selector;
  bool has_selector = false;
  EYEQ::VideoConnection connection = EYEQ::VideoConnection::SDI;
  EYEQ::PixelFormat format = EYEQ::PixelFormat::V210;
  ParsedMode requested;
  bool hdr = false;
  bool still = false;
  bool frames_set = false;
  bool seconds_set = false;
  uint64_t frames = 0;
  int64_t seconds = 5;
  uint64_t preroll = 4;
  int64_t drain_timeout_ms = 5000;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (arg == "--device") {
      if (++i >= argc) {
        std::fprintf(stderr, "--device requires a value\n");
        return 1;
      }
      if (has_selector) {
        std::fprintf(stderr, "--device may be given only once\n");
        return 1;
      }
      std::string selector_error;
      if (!EYEQ::ParseDeviceSelector(argv[i], &selector, &selector_error)) {
        std::fprintf(stderr, "--device: %s\n", selector_error.c_str());
        return 1;
      }
      has_selector = true;
      continue;
    }
    if (arg == "--connection") {
      if (++i >= argc || !ParseConnection(argv[i], &connection)) {
        std::fprintf(stderr, "--connection must be sdi or hdmi\n");
        return 1;
      }
      continue;
    }
    if (arg == "--pixel-format") {
      if (++i >= argc || !ParsePixelFormat(argv[i], &format)) {
        std::fprintf(stderr, "--pixel-format must be r210 or v210\n");
        return 1;
      }
      continue;
    }
    if (arg == "--mode") {
      if (++i >= argc || !ParseMode(argv[i], &requested)) {
        std::fprintf(stderr, "--mode must be WxH@fps (for example 3840x2160@50) or auto\n");
        return 1;
      }
      continue;
    }
    if (arg == "--hdr") {
      hdr = true;
      continue;
    }
    if (arg == "--still") {
      still = true;
      continue;
    }
    if (arg == "--frames") {
      if (++i >= argc || !ParseUint64(argv[i], &frames) || frames == 0) {
        std::fprintf(stderr, "--frames requires a positive integer\n");
        return 1;
      }
      frames_set = true;
      continue;
    }
    if (arg == "--seconds") {
      if (++i >= argc || !ParseInt64(argv[i], &seconds) || seconds <= 0) {
        std::fprintf(stderr, "--seconds requires a positive integer\n");
        return 1;
      }
      seconds_set = true;
      continue;
    }
    if (arg == "--preroll") {
      if (++i >= argc || !ParseUint64(argv[i], &preroll)) {
        std::fprintf(stderr, "--preroll requires a non-negative integer\n");
        return 1;
      }
      continue;
    }
    if (arg == "--drain-timeout-ms") {
      if (++i >= argc || !ParseInt64(argv[i], &drain_timeout_ms) || drain_timeout_ms < 0) {
        std::fprintf(stderr, "--drain-timeout-ms requires a non-negative integer\n");
        return 1;
      }
      continue;
    }
    std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
    PrintUsage(argv[0]);
    return 1;
  }

  if (!has_selector) {
    std::fprintf(stderr, "--device is required\n");
    PrintUsage(argv[0]);
    return 1;
  }
  if (frames_set && seconds_set) {
    std::fprintf(stderr, "--frames and --seconds are mutually exclusive\n");
    return 1;
  }
  if (drain_timeout_ms > std::numeric_limits<int>::max()) {
    std::fprintf(stderr, "--drain-timeout-ms is too large\n");
    return 1;
  }

  const EYEQ::DeviceCatalog catalog = EYEQ::DeckLinkOutput::Enumerate();
  if (catalog.status != EYEQ::DeviceCatalog::Status::Ok) {
    std::fprintf(stderr, "DeckLink enumeration failed: %s\n", catalog.error.c_str());
    return 2;
  }

  const EYEQ::DeviceInfo *device = nullptr;
  for (const EYEQ::DeviceInfo &candidate : catalog.devices) {
    if (!selector.persistent_id.empty()) {
      if (candidate.has_persistent_id && candidate.id.persistent_id == selector.persistent_id) {
        device = &candidate;
        break;
      }
    } else if (candidate.index == selector.index) {
      device = &candidate;
      break;
    }
  }
  if (device == nullptr) {
    std::fprintf(stderr, "device not found\n");
    return 1;
  }

  std::vector<EYEQ::MonitorMode> modes;
  std::string error;
  if (!EYEQ::DeckLinkOutput::ListModes(device->id, connection, format, &modes, &error)) {
    std::fprintf(stderr, "mode query failed: %s\n", error.c_str());
    return 1;
  }

  EYEQ::MonitorMode mode;
  if (!SelectMode(modes, requested, &mode, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    PrintModes(modes);
    return 1;
  }

  uint64_t frame_count = 0;
  if (!still) {
    if (mode.frame_duration <= 0 || mode.time_scale <= 0) {
      std::fprintf(stderr, "the selected mode has an invalid time base\n");
      return 1;
    }
    if (frames_set) {
      frame_count = frames;
    } else {
      // Exact rational seconds-to-frames conversion with a wide intermediate so an
      // oversized --seconds is rejected instead of wrapping into a different request.
      const __int128 scaled = static_cast<__int128>(seconds) * mode.time_scale;
      const __int128 frames128 = scaled / mode.frame_duration;
      if (frames128 <= 0 || frames128 > static_cast<__int128>(std::numeric_limits<uint64_t>::max())) {
        std::fprintf(stderr, "--seconds is out of range for the selected mode\n");
        return 1;
      }
      frame_count = static_cast<uint64_t>(frames128);
    }
    if (frame_count == 0) {
      std::fprintf(stderr, "the requested duration is shorter than one frame at this mode\n");
      return 1;
    }
    // The last frame's end time must be representable: index * frame_duration is a
    // signed multiply in the session and would otherwise overflow silently.
    if (frame_count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / mode.frame_duration)) {
      std::fprintf(stderr, "the requested frame count exceeds the representable timeline\n");
      return 1;
    }
  }
  if (preroll > frame_count) {
    preroll = frame_count;
  }

  EYEQ::OutputRequest request;
  request.device = device->id;
  request.mode = mode;
  request.connection = connection;
  request.pixel_format = format;
  request.color.encoding.range = EYEQ::ColorRange::Video;
  request.color.encoding.chroma_loc = EYEQ::ChromaLocation::Left;
  if (hdr) {
    request.color.encoding.primaries = EYEQ::ColorPrimaries::BT2020;
    request.color.encoding.transfer = EYEQ::ColorTransfer::PQ;
    request.hdr.hdr_signaling = true;
    request.hdr.primaries = EYEQ::ColorPrimaries::BT2020;
    request.hdr.transfer = EYEQ::ColorTransfer::PQ;
  }

  std::fprintf(stderr,
               "decklink-test-signal: this tool takes exclusive control of the selected DeckLink output while it\n"
               "runs. If the device is already in use it reports the failure and exits; it never terminates or\n"
               "preempts another application (for example DaVinci Resolve). Press Ctrl-C to stop and release the\n"
               "device cleanly.\n");

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);

  EYEQ::DeckLinkOutput output;
  EYEQ::ResolvedOutput resolved;
  if (!output.Open(request, &resolved, &error)) {
    FinalizeAfterOpenFailure(output, error, static_cast<int>(drain_timeout_ms));
    return 1;
  }

  EYEQ::TestSignalResult result;
  FinalizeReport report;
  if (still) {
    std::vector<uint8_t> staging(resolved.layout.image_bytes, 0);
    EYEQ::RenderTestPattern(0, resolved.layout, resolved.color.encoding, staging.data());
    EYEQ::SubmissionInfo info;
    info.submit_id = 0;
    const EYEQ::StillOutcome outcome = output.DisplayStill({staging.data(), staging.size()}, info);
    if (outcome != EYEQ::StillOutcome::Displayed) {
      std::fprintf(stderr, "still frame was not displayed\n");
    } else {
      result.submitted = 1;
      result.completed = 1;
    }
    Finalize(output, static_cast<int>(drain_timeout_ms), &result, &report);
    PrintSummary(*device, mode, connection, format, hdr, result, report);
    if (outcome != EYEQ::StillOutcome::Displayed || report.close != EYEQ::CloseStatus::Closed ||
        !report.errors.empty()) {
      return 1;
    }
    return 0;
  }

  EYEQ::TestSignalOptions options;
  options.frame_count = frame_count;
  options.preroll_frames = preroll;
  options.drain_timeout_ms = static_cast<int>(drain_timeout_ms);
  const bool run_ok = EYEQ::RunTestSignal(output, resolved, options, &result, &error);
  // The error text, not the stop flag alone, decides whether this was an intentional
  // interruption; a real device error must never be reclassified as a clean stop.
  const bool interrupted = !run_ok && error.rfind("interrupted", 0) == 0;
  if (!run_ok) {
    std::fprintf(stderr, "run %s: %s\n", interrupted ? "interrupted" : "failed", error.c_str());
  }
  Finalize(output, static_cast<int>(drain_timeout_ms), &result, &report);
  PrintSummary(*device, mode, connection, format, hdr, result, report);
  if (report.close != EYEQ::CloseStatus::Closed || !report.errors.empty()) {
    return 1;
  }
  if (!run_ok) {
    return interrupted ? 0 : 1;
  }
  return 0;
}
