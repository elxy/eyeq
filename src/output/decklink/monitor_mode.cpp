#include "output/decklink/monitor_mode.hpp"

namespace EYEQ {

namespace {

int64_t Gcd(int64_t a, int64_t b) {
  while (b != 0) {
    const int64_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a < 0 ? -a : a;
}

} // namespace

std::string FormatMonitorMode(const MonitorMode &mode) {
  std::string rate = "?";
  if (mode.frame_duration > 0 && mode.time_scale > 0) {
    int64_t numerator = mode.time_scale;
    int64_t denominator = mode.frame_duration;
    const int64_t divisor = Gcd(numerator, denominator);
    if (divisor > 1) {
      numerator /= divisor;
      denominator /= divisor;
    }
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
  return mode.time_scale * fps_denominator == mode.frame_duration * fps_numerator;
}

} // namespace EYEQ
