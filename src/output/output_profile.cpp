#include "output/output_profile.hpp"

#include <cmath>

namespace EYEQ {

namespace {

bool Fail(std::string *error, const char *message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

} // namespace

bool ValidateRequest(const OutputRequest &request, std::string *error) {
  const MonitorMode &mode = request.mode;
  if (mode.width <= 0 || mode.height <= 0) {
    return Fail(error, "mode dimensions must be positive");
  }
  if (mode.frame_duration <= 0 || mode.time_scale <= 0) {
    return Fail(error, "mode frame_duration/time_scale must be positive");
  }
  if (!mode.is_progressive) {
    return Fail(error, "first version supports progressive modes only");
  }

  const ColorEncoding &encoding = request.color.encoding;
  const bool is_sdr = encoding.primaries == ColorPrimaries::BT709 && encoding.transfer == ColorTransfer::SDR_BT1886;
  const bool is_hdr = encoding.primaries == ColorPrimaries::BT2020 && encoding.transfer == ColorTransfer::PQ;
  if (!is_sdr && !is_hdr) {
    return Fail(error, "unsupported color encoding: first version allows BT709/BT1886 or BT2020/PQ");
  }
  if (encoding.range != ColorRange::Video) {
    return Fail(error, "first version requires ColorRange::Video");
  }
  if (encoding.chroma_loc != ChromaLocation::Left) {
    return Fail(error, "first version requires ChromaLocation::Left");
  }
  if (!std::isfinite(request.color.target_nits) || request.color.target_nits <= 0.0) {
    return Fail(error, "target_nits must be finite and positive");
  }

  // Signal metadata must match the pixel encoding exactly. PQ only requires the EOTF
  // and Rec.2020 signaling; unknown mastering/CLL are allowed.
  const StaticHdrMetadata &hdr = request.hdr;
  if (is_hdr) {
    if (!hdr.hdr_signaling) {
      return Fail(error, "PQ encoding requires hdr_signaling");
    }
    if (hdr.transfer != ColorTransfer::PQ || hdr.primaries != ColorPrimaries::BT2020) {
      return Fail(error, "PQ encoding requires PQ/BT2020 HDR signaling");
    }
  } else {
    if (hdr.hdr_signaling) {
      return Fail(error, "SDR encoding must not enable HDR signaling");
    }
    if (hdr.transfer != ColorTransfer::SDR_BT1886 || hdr.primaries != ColorPrimaries::BT709) {
      return Fail(error, "SDR encoding requires SDR/BT709 signaling");
    }
  }

  if (hdr.mastering.has_primaries) {
    const double values[8] = {hdr.mastering.red_x,  hdr.mastering.red_y,  hdr.mastering.green_x, hdr.mastering.green_y,
                              hdr.mastering.blue_x, hdr.mastering.blue_y, hdr.mastering.white_x, hdr.mastering.white_y};
    for (double value : values) {
      if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        return Fail(error, "mastering primaries must be finite in [0, 1]");
      }
    }
  }
  if (hdr.mastering.has_luminance) {
    if (!std::isfinite(hdr.mastering.min_nits) || !std::isfinite(hdr.mastering.max_nits)) {
      return Fail(error, "mastering luminance must be finite");
    }
    if (hdr.mastering.min_nits < 0.0001 || hdr.mastering.min_nits > 6.5535) {
      return Fail(error, "mastering min_nits out of SDK range [0.0001, 6.5535]");
    }
    if (hdr.mastering.max_nits < 1.0 || hdr.mastering.max_nits > 65535.0) {
      return Fail(error, "mastering max_nits out of SDK range [1, 65535]");
    }
    if (hdr.mastering.min_nits >= hdr.mastering.max_nits) {
      return Fail(error, "mastering luminance requires min_nits < max_nits");
    }
  }
  if (hdr.cll.has_values) {
    if (!std::isfinite(hdr.cll.max_cll) || !std::isfinite(hdr.cll.max_fall)) {
      return Fail(error, "CLL/FALL must be finite");
    }
    if (hdr.cll.max_cll < 0.0 || hdr.cll.max_cll > 65535.0 || hdr.cll.max_fall < 0.0 || hdr.cll.max_fall > 65535.0) {
      return Fail(error, "CLL/FALL out of SDK range [0, 65535]");
    }
    if (hdr.cll.max_fall > hdr.cll.max_cll) {
      return Fail(error, "CLL requires max_fall <= max_cll");
    }
  }

  return true;
}

PackConfig ToPackConfig(const ResolvedOutput &resolved) {
  PackConfig config;
  config.layout = resolved.layout;
  config.output = resolved.color.encoding;
  return config;
}

} // namespace EYEQ
