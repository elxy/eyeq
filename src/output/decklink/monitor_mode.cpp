#include "output/decklink/monitor_mode.hpp"

#include <numeric>

namespace EYEQ {

std::string FormatMonitorMode(const MonitorMode &mode) {
  std::string rate = "?";
  if (mode.frame_duration > 0 && mode.time_scale > 0) {
    const int64_t divisor = std::gcd(mode.time_scale, mode.frame_duration);
    const int64_t numerator = mode.time_scale / divisor;
    const int64_t denominator = mode.frame_duration / divisor;
    rate = std::to_string(numerator);
    if (denominator != 1) {
      rate += "/" + std::to_string(denominator);
    }
  }
  return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "@" + rate +
         (mode.is_progressive ? "p" : "i");
}

bool MonitorModeIs(const MonitorMode &mode, int width, int height, int64_t fps_numerator, int64_t fps_denominator) {
  if (mode.width != width || mode.height != height) {
    return false;
  }
  if (mode.frame_duration <= 0 || mode.time_scale <= 0 || fps_numerator <= 0 || fps_denominator <= 0) {
    return false;
  }
  // Compare the two rationals in lowest terms. This avoids the cross-multiplication
  // time_scale * fps_denominator == frame_duration * fps_numerator, which overflows
  // signed int64 for legal large inputs and can then report a false match.
  const int64_t mode_divisor = std::gcd(mode.time_scale, mode.frame_duration);
  const int64_t requested_divisor = std::gcd(fps_numerator, fps_denominator);
  return mode.time_scale / mode_divisor == fps_numerator / requested_divisor &&
         mode.frame_duration / mode_divisor == fps_denominator / requested_divisor;
}

} // namespace EYEQ
