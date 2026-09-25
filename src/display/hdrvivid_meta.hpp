#pragma once

extern "C" {
#include <libavutil/frame.h>
}

#include "hdrvivid_curve.hpp"

namespace EYEQ {

/**
 * @brief Extract HDR Vivid 1.0 dynamic metadata from a decoded frame
 *
 * Reads AV_FRAME_DATA_DYNAMIC_HDR_VIVID (already parsed by FFmpeg) and converts the
 * AVRational-based representation back to the raw CUVA code values expected by the curve
 * solver.
 *
 * @return false if the frame carries no usable HDR Vivid metadata
 */
bool ParseHDRVividMetadata(const AVFrame *frame, HDRVividMetadata *out);

/**
 * @brief Mastering display peak luminance in nits, defaults to 1000 when absent
 */
double MasteringDisplayMaxLuminance(const AVFrame *frame);

} // namespace EYEQ
