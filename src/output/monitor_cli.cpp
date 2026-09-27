#include "output/monitor_cli.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <system_error>

#include "output/decklink/monitor_mode.hpp"

namespace EYEQ {

namespace {

bool Fail(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

// Parses a non-negative decimal into an int. Rejects trailing characters,
// overflow and a leading sign ("+1", "-1"), so nothing is silently reinterpreted.
bool ParsePositiveInt(const std::string &text, const char *what, int *out, std::string *error) {
  if (text.empty()) {
    return Fail(error, std::string(what) + " is empty");
  }
  int64_t value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end || value <= 0 ||
      value > static_cast<int64_t>(std::numeric_limits<int>::max())) {
    return Fail(error, std::string(what) + " '" + text + "' must be a positive integer");
  }
  *out = static_cast<int>(value);
  return true;
}

bool ParsePositiveInt64(const std::string &text, const char *what, int64_t *out, std::string *error) {
  if (text.empty()) {
    return Fail(error, std::string(what) + " is empty");
  }
  int64_t value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end || value <= 0) {
    return Fail(error, std::string(what) + " '" + text + "' must be a positive integer");
  }
  *out = value;
  return true;
}

} // namespace

bool ParseDeviceSelector(const std::string &text, DeviceId *selector, std::string *error) {
  selector->persistent_id.clear();
  selector->index = -1;

  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return Fail(error, "device selector '" + text + "' must be 'index:<n>' or 'id:<value>'");
  }

  const std::string kind = text.substr(0, colon);
  const std::string value = text.substr(colon + 1);
  if (value.empty()) {
    return Fail(error, "device selector '" + kind + ":' is missing a value");
  }

  if (kind == "index") {
    int64_t index = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, index);
    if (result.ec != std::errc() || result.ptr != end || index < 0 ||
        index > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      return Fail(error, "invalid device index '" + value + "': expected a non-negative integer that fits in int");
    }
    selector->index = static_cast<int>(index);
    return true;
  }

  if (kind == "id") {
    int64_t id = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, id);
    if (result.ec != std::errc() || result.ptr != end) {
      return Fail(error, "invalid persistent id '" + value + "': expected a decimal int64");
    }
    selector->persistent_id = std::to_string(id);
    return true;
  }

  return Fail(error, "unknown device selector kind '" + kind + "': use 'index:' or 'id:'");
}

bool ParseMonitorConnectionChoice(const std::string &text, MonitorConnectionChoice *out, std::string *error) {
  if (text == "auto") {
    *out = MonitorConnectionChoice::Auto;
    return true;
  }
  if (text == "sdi") {
    *out = MonitorConnectionChoice::Sdi;
    return true;
  }
  if (text == "hdmi") {
    *out = MonitorConnectionChoice::Hdmi;
    return true;
  }
  return Fail(error, "monitor connection '" + text + "' must be 'auto', 'sdi' or 'hdmi'");
}

const char *MonitorConnectionChoiceLabel(MonitorConnectionChoice choice) {
  switch (choice) {
  case MonitorConnectionChoice::Auto:
    return "auto";
  case MonitorConnectionChoice::Sdi:
    return "sdi";
  case MonitorConnectionChoice::Hdmi:
    return "hdmi";
  }
  return "?";
}

bool ResolveMonitorConnection(MonitorConnectionChoice requested, const MonitorConnections &connections,
                              VideoConnection *out, std::string *reason, std::string *error) {
  switch (requested) {
  case MonitorConnectionChoice::Sdi:
    *out = VideoConnection::SDI;
    if (reason != nullptr) {
      *reason = "explicit sdi";
    }
    return true;
  case MonitorConnectionChoice::Hdmi:
    *out = VideoConnection::HDMI;
    if (reason != nullptr) {
      *reason = "explicit hdmi";
    }
    return true;
  case MonitorConnectionChoice::Auto:
    break;
  }

  if (!connections.known) {
    return Fail(error, "cannot auto-select the monitor connection: the device did not report its output "
                       "connections; pass --monitor-connection sdi|hdmi explicitly");
  }
  if (connections.sdi) {
    *out = VideoConnection::SDI;
    if (reason != nullptr) {
      *reason = "auto: device reports SDI";
    }
    return true;
  }
  if (connections.hdmi) {
    *out = VideoConnection::HDMI;
    if (reason != nullptr) {
      *reason = "auto: device reports only HDMI";
    }
    return true;
  }
  return Fail(error, "cannot auto-select the monitor connection: the device reports neither SDI nor HDMI output");
}

bool ParseMonitorPixelFormat(const std::string &text, PixelFormat *out, std::string *error) {
  if (text == "v210") {
    *out = PixelFormat::V210;
    return true;
  }
  if (text == "r210") {
    *out = PixelFormat::R210;
    return true;
  }
  return Fail(error, "monitor pixel format '" + text + "' must be 'v210' or 'r210'");
}

bool ParseMonitorColorTarget(const std::string &text, MonitorColorTarget *out, std::string *error) {
  if (text == "auto") {
    *out = MonitorColorTarget::Auto;
    return true;
  }
  if (text == "sdr709") {
    *out = MonitorColorTarget::SdrBt709;
    return true;
  }
  if (text == "pq2020") {
    *out = MonitorColorTarget::PqBt2020;
    return true;
  }
  return Fail(error,
              "monitor color target '" + text + "' must be 'auto', 'sdr709' (BT.709/BT.1886) or 'pq2020' (BT.2020/PQ)");
}

bool ResolveMonitorColorTarget(MonitorColorTarget requested, bool main_is_hdr, MonitorColorTarget *out,
                               std::string *reason) {
  if (requested != MonitorColorTarget::Auto) {
    *out = requested;
    if (reason != nullptr) {
      *reason = "explicit";
    }
    return true;
  }
  // The choice is driven by the transfer function, never by primaries: BT.2020 is
  // a superset of wide-gamut SDR (P3) and is only paired with a PQ wire.
  *out = main_is_hdr ? MonitorColorTarget::PqBt2020 : MonitorColorTarget::SdrBt709;
  if (reason != nullptr) {
    *reason = main_is_hdr ? "auto: main video is HDR (PQ/HLG)" : "auto: main video is SDR";
  }
  return true;
}

bool ParseMonitorMode(const std::string &text, MonitorMode *out, std::string *error) {
  const std::size_t at = text.find('@');
  if (at == std::string::npos) {
    return Fail(error, "monitor mode '" + text + "' must be 'WxH@N' or 'WxH@N/D' (exact integer rate)");
  }
  const std::string size = text.substr(0, at);
  const std::string rate = text.substr(at + 1);

  const std::size_t x = size.find('x');
  if (x == std::string::npos) {
    return Fail(error, "monitor mode '" + text + "' size must be 'WxH'");
  }

  MonitorMode mode;
  if (!ParsePositiveInt(size.substr(0, x), "monitor mode width", &mode.width, error) ||
      !ParsePositiveInt(size.substr(x + 1), "monitor mode height", &mode.height, error)) {
    return false;
  }

  const std::size_t slash = rate.find('/');
  int64_t numerator = 0;
  int64_t denominator = 0;
  if (slash == std::string::npos) {
    if (!ParsePositiveInt64(rate, "monitor mode rate", &numerator, error)) {
      return false;
    }
    denominator = 1;
  } else {
    if (!ParsePositiveInt64(rate.substr(0, slash), "monitor mode rate numerator", &numerator, error) ||
        !ParsePositiveInt64(rate.substr(slash + 1), "monitor mode rate denominator", &denominator, error)) {
      return false;
    }
  }

  // fps = time_scale / frame_duration, so the parsed N/D maps to time_scale=N,
  // frame_duration=D. Only progressive modes are supported.
  mode.time_scale = numerator;
  mode.frame_duration = denominator;
  mode.is_progressive = true;
  *out = mode;
  return true;
}

bool ParseMonitorModeChoice(const std::string &text, bool *is_auto, MonitorMode *explicit_mode, std::string *error) {
  if (text == "auto") {
    *is_auto = true;
    *explicit_mode = MonitorMode{};
    return true;
  }
  *is_auto = false;
  return ParseMonitorMode(text, explicit_mode, error);
}

bool IsMonitorModeWhitelisted(const MonitorMode &mode) {
  return (mode.width == 1920 && mode.height == 1080) || (mode.width == 3840 && mode.height == 2160);
}

namespace {

double ModeFps(const MonitorMode &mode) {
  return mode.frame_duration > 0 ? static_cast<double>(mode.time_scale) / static_cast<double>(mode.frame_duration)
                                 : 0.0;
}

// Smallest-area mode that covers the source; when none covers, the largest area.
bool PreferResolution(const MonitorMode &candidate, const MonitorMode &current, int source_width, int source_height) {
  const bool candidate_covers = candidate.width >= source_width && candidate.height >= source_height;
  const bool current_covers = current.width >= source_width && current.height >= source_height;
  if (candidate_covers != current_covers) {
    return candidate_covers; // covering beats not covering
  }
  const int64_t candidate_area = static_cast<int64_t>(candidate.width) * candidate.height;
  const int64_t current_area = static_cast<int64_t>(current.width) * current.height;
  if (candidate_covers) {
    return candidate_area < current_area; // least upscale among covering modes
  }
  return candidate_area > current_area; // least downscale among non-covering modes
}

} // namespace

bool SelectMonitorModeForSource(const std::vector<MonitorMode> &modes, int source_width, int source_height,
                                double source_fps, MonitorMode *out, std::string *reason, std::string *error) {
  if (source_width <= 0 || source_height <= 0) {
    return Fail(error, "cannot auto-select a monitor mode: the main video size is unknown; pass --monitor-mode");
  }
  if (!std::isfinite(source_fps) || source_fps <= 0.0) {
    return Fail(error, "cannot auto-select a monitor mode: the main video frame rate is unknown; pass "
                       "--monitor-mode");
  }

  // Prefer whitelisted modes whenever the device has any; a mode outside the
  // whitelist would be rejected by Open, so auto should not choose one needlessly.
  std::vector<const MonitorMode *> progressive;
  std::vector<const MonitorMode *> whitelisted;
  for (const MonitorMode &mode : modes) {
    if (!mode.is_progressive) {
      continue;
    }
    progressive.push_back(&mode);
    if (IsMonitorModeWhitelisted(mode)) {
      whitelisted.push_back(&mode);
    }
  }
  const std::vector<const MonitorMode *> &pool = whitelisted.empty() ? progressive : whitelisted;
  if (pool.empty()) {
    return Fail(error, "cannot auto-select a monitor mode: the device lists no progressive mode for this "
                       "connection and pixel format");
  }

  constexpr double kRateEpsilon = 5e-3; // separates 30000/1001 from 30000/1000 and nothing finer
  const MonitorMode *rate_choice = nullptr;
  for (const MonitorMode *mode : pool) {
    if (std::fabs(ModeFps(*mode) - source_fps) <= kRateEpsilon) {
      rate_choice = mode;
      break;
    }
  }
  bool exact = rate_choice != nullptr;
  if (rate_choice == nullptr) {
    for (const MonitorMode *mode : pool) {
      const double fps = ModeFps(*mode);
      if (fps >= source_fps - kRateEpsilon && (rate_choice == nullptr || fps < ModeFps(*rate_choice))) {
        rate_choice = mode; // lowest rate not below the source (no dropped frames)
      }
    }
  }
  if (rate_choice == nullptr) {
    for (const MonitorMode *mode : pool) {
      if (rate_choice == nullptr || ModeFps(*mode) > ModeFps(*rate_choice)) {
        rate_choice = mode; // highest rate below the source
      }
    }
  }

  const MonitorMode *best = nullptr;
  for (const MonitorMode *mode : pool) {
    if (std::fabs(ModeFps(*mode) - ModeFps(*rate_choice)) > kRateEpsilon) {
      continue;
    }
    if (best == nullptr || PreferResolution(*mode, *best, source_width, source_height)) {
      best = mode;
    }
  }

  *out = *best;
  if (reason != nullptr) {
    std::ostringstream text;
    text << "auto: source " << source_width << "x" << source_height << " @ " << source_fps << " fps -> "
         << FormatMonitorMode(*best) << " (" << (exact ? "exact rate" : "nearest rate") << ")";
    *reason = text.str();
  }
  return true;
}

bool SelectMonitorMode(const std::vector<MonitorMode> &modes, const MonitorMode &requested, MonitorMode *out,
                       std::string *error) {
  for (const MonitorMode &mode : modes) {
    if (mode.is_progressive &&
        MonitorModeIs(mode, requested.width, requested.height, requested.time_scale, requested.frame_duration)) {
      *out = mode;
      return true;
    }
  }
  std::string available = "requested monitor mode is not natively supported; available:";
  for (const MonitorMode &mode : modes) {
    available += " " + FormatMonitorMode(mode);
  }
  return Fail(error, available);
}

bool BuildMonitorOutputRequest(const DeviceId &device, const MonitorMode &mode, VideoConnection connection,
                               PixelFormat pixel_format, MonitorColorTarget color_target, double target_nits,
                               OutputRequest *out, std::string *error) {
  OutputRequest request;
  request.device = device;
  request.mode = mode;
  request.connection = connection;
  request.pixel_format = pixel_format;
  request.color.encoding.range = ColorRange::Video;
  request.color.encoding.chroma_loc = ChromaLocation::Left;
  request.color.target_nits = target_nits;
  if (color_target == MonitorColorTarget::Auto) {
    return Fail(error, "internal error: monitor color target was not resolved before building the request");
  }
  if (color_target == MonitorColorTarget::PqBt2020) {
    request.color.encoding.primaries = ColorPrimaries::BT2020;
    request.color.encoding.transfer = ColorTransfer::PQ;
    // Static HDR signaling: exactly PQ/BT2020, no fabricated mastering/CLL values.
    request.hdr.hdr_signaling = true;
    request.hdr.primaries = ColorPrimaries::BT2020;
    request.hdr.transfer = ColorTransfer::PQ;
  } else {
    request.color.encoding.primaries = ColorPrimaries::BT709;
    request.color.encoding.transfer = ColorTransfer::SDR_BT1886;
    request.hdr.hdr_signaling = false;
    request.hdr.primaries = ColorPrimaries::BT709;
    request.hdr.transfer = ColorTransfer::SDR_BT1886;
  }
  *out = request;
  return ValidateRequest(*out, error);
}

const char *MonitorColorTargetLabel(MonitorColorTarget target) {
  switch (target) {
  case MonitorColorTarget::Auto:
    return "auto";
  case MonitorColorTarget::SdrBt709:
    return "sdr709";
  case MonitorColorTarget::PqBt2020:
    return "pq2020";
  }
  return "?";
}

} // namespace EYEQ
