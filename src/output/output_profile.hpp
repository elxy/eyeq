#pragma once

#include <cstdint>
#include <string>

#include "output/pixel_format.hpp"

namespace EYEQ {

struct DeviceId {
  std::string persistent_id; // preferred; empty when only an index is available
  int index = -1;            // fallback only
};

// Integer frame rate. duration is seconds-per-frame over time_scale; fps is
// time_scale / frame_duration. Both values come from a resolved display mode.
struct MonitorMode {
  int width = 0;
  int height = 0;
  int64_t frame_duration = 0; // BMDTimeValue
  int64_t time_scale = 0;     // BMDTimeScale
  bool is_progressive = true;
};

enum class VideoConnection { SDI, HDMI };

struct MasteringDisplay {
  bool has_primaries = false;
  double red_x = 0, red_y = 0, green_x = 0, green_y = 0, blue_x = 0, blue_y = 0, white_x = 0, white_y = 0;
  bool has_luminance = false;
  double min_nits = 0.0;
  double max_nits = 0.0;
};

struct ContentLightLevel {
  bool has_values = false;
  double max_cll = 0.0;
  double max_fall = 0.0;
};

// EyeQ-owned HDR signaling. Defaults are SDR-consistent. SDK FourCC / CEA codes are
// only produced inside the backend implementation.
struct StaticHdrMetadata {
  bool hdr_signaling = false;
  ColorPrimaries primaries = ColorPrimaries::BT709;
  ColorTransfer transfer = ColorTransfer::SDR_BT1886;
  MasteringDisplay mastering; // has_* == false -> not transmitted
  ContentLightLevel cll;      // never fabricated
};

// Requested output configuration. The default-constructed request is a valid SDR
// request; a PQ request must fill color.encoding and hdr coherently.
struct OutputRequest {
  DeviceId device;
  MonitorMode mode;
  VideoConnection connection = VideoConnection::SDI;
  PixelFormat pixel_format = PixelFormat::V210;
  ColorProfile color;
  StaticHdrMetadata hdr;
};

// Immutable after a successful Open; single authority for layout/color/HDR.
struct ResolvedOutput {
  DeviceId device;
  MonitorMode mode;
  VideoConnection connection = VideoConnection::SDI;
  PixelFormat pixel_format = PixelFormat::V210;
  BufferLayout layout;
  ColorProfile color;
  StaticHdrMetadata hdr;
  bool needs_vertical_flip = false;
};

// Strictly validates an OutputRequest before any device output is enabled.
// First version allows exactly BT709/BT1886/SDR and BT2020/PQ/HDR, Video range,
// Left chroma location, and progressive modes. Unknown mastering/CLL are allowed
// for a valid PQ request. Never silently converts to SDR. Returns false and sets
// *error on an unsupported or inconsistent combination.
bool ValidateRequest(const OutputRequest &request, std::string *error);

// Derives the packer's minimal input. No target_nits crosses into PackConfig.
PackConfig ToPackConfig(const ResolvedOutput &resolved);

} // namespace EYEQ
