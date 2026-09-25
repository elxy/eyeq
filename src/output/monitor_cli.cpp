#include "output/monitor_cli.hpp"

#include <charconv>
#include <limits>
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

bool ParseMonitorConnection(const std::string &text, VideoConnection *out, std::string *error) {
  if (text == "sdi") {
    *out = VideoConnection::SDI;
    return true;
  }
  if (text == "hdmi") {
    *out = VideoConnection::HDMI;
    return true;
  }
  return Fail(error, "monitor connection '" + text + "' must be 'sdi' or 'hdmi'");
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
  if (text == "sdr709") {
    *out = MonitorColorTarget::SdrBt709;
    return true;
  }
  if (text == "pq2020") {
    *out = MonitorColorTarget::PqBt2020;
    return true;
  }
  return Fail(error, "monitor color target '" + text + "' must be 'sdr709' (BT.709/BT.1886) or 'pq2020' (BT.2020/PQ)");
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
  return target == MonitorColorTarget::PqBt2020 ? "pq2020" : "sdr709";
}

} // namespace EYEQ
