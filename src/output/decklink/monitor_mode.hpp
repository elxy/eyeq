#pragma once

#include <cstdint>
#include <string>

#include "output/output_profile.hpp"

namespace EYEQ {

// Formats the exact integer rational rate of a mode using no floating point, e.g.
// "3840x2160@50p" or "1920x1080@30000/1001p". Rate = time_scale / frame_duration,
// reduced by their GCD. Progressive modes end in "p", everything else in "i".
std::string FormatMonitorMode(const MonitorMode &mode);

// Exact integer-rational match: width and height equal, and
// time_scale * fps_denominator == frame_duration * fps_numerator.
// Never compares floating-point frame rates.
bool MonitorModeIs(const MonitorMode &mode, int width, int height, int64_t fps_numerator, int64_t fps_denominator);

} // namespace EYEQ
