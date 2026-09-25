#include "hdrvivid_meta.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <spdlog/fmt/fmt.h>

extern "C" {
#include <libavutil/hdr_dynamic_vivid_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/rational.h>
}

namespace EYEQ {
namespace {

/**
 * @brief Convert an AVRational back to the raw bitstream code value
 *
 * FFmpeg stores each CUVA field as code/denominator. The mapping implementation expects
 * the raw code, so multiply by the same denominator FFmpeg used.
 */
unsigned int to_code(const AVRational &value, int denominator, int max_code, const char *name) {
  if (value.den == 0) {
    throw std::runtime_error(fmt::format("HDR Vivid metadata has invalid rational for {}", name));
  }

  const double scaled = av_q2d(value) * denominator;
  if (!std::isfinite(scaled)) {
    throw std::runtime_error(fmt::format("HDR Vivid metadata has non-finite value for {}", name));
  }

  const long rounded = std::lround(scaled);
  return static_cast<unsigned int>(std::clamp<long>(rounded, 0, max_code));
}

unsigned int to_code(const AVRational &value, int denominator_and_max, const char *name) {
  return to_code(value, denominator_and_max, denominator_and_max, name);
}

} // namespace

bool ParseHDRVividMetadata(const AVFrame *frame, HDRVividMetadata *out) {
  AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_VIVID);
  if (!sd || sd->size < static_cast<int>(sizeof(AVDynamicHDRVivid))) {
    return false;
  }

  const auto *vivid = reinterpret_cast<const AVDynamicHDRVivid *>(sd->data);
  if (vivid->num_windows < 1 || vivid->system_start_code < 0x01 || vivid->system_start_code > 0x07) {
    return false;
  }

  const AVHDRVividColorTransformParams &p = vivid->params[0];
  std::memset(out, 0, sizeof(*out));
  out->system_start_code = vivid->system_start_code;
  out->minimum_maxrgb = to_code(p.minimum_maxrgb, 4095, "minimum_maxrgb");
  out->average_maxrgb = to_code(p.average_maxrgb, 4095, "average_maxrgb");
  out->variance_maxrgb = to_code(p.variance_maxrgb, 4095, "variance_maxrgb");
  out->maximum_maxrgb = to_code(p.maximum_maxrgb, 4095, "maximum_maxrgb");

  out->tone_mapping_mode = p.tone_mapping_mode_flag ? 1 : 0;
  if (out->tone_mapping_mode) {
    out->tone_mapping_param_num = static_cast<unsigned int>(std::clamp(p.tone_mapping_param_num, 1, 2));
    for (unsigned int i = 0; i < out->tone_mapping_param_num; ++i) {
      const AVHDRVividColorToneMappingParams &tm = p.tm_params[i];
      out->targeted_system_display_maximum_luminance[i] =
          to_code(tm.targeted_system_display_maximum_luminance, 4095, "targeted_system_display_maximum_luminance");
      out->base_flag[i] = tm.base_enable_flag ? 1 : 0;
      if (out->base_flag[i]) {
        out->base_param_m_p[i] = to_code(tm.base_param_m_p, 16383, "base_param_m_p");
        out->base_param_m_m[i] = to_code(tm.base_param_m_m, 10, 63, "base_param_m_m");
        out->base_param_m_a[i] = to_code(tm.base_param_m_a, 1023, "base_param_m_a");
        out->base_param_m_b[i] = to_code(tm.base_param_m_b, 1023, "base_param_m_b");
        out->base_param_m_n[i] = to_code(tm.base_param_m_n, 10, 63, "base_param_m_n");
        out->base_param_k1[i] = static_cast<unsigned int>(std::clamp(tm.base_param_k1, 0, 3));
        out->base_param_k2[i] = static_cast<unsigned int>(std::clamp(tm.base_param_k2, 0, 3));
        out->base_param_k3[i] = static_cast<unsigned int>(std::clamp(tm.base_param_k3, 0, 15));
        out->base_param_delta_mode[i] = static_cast<unsigned int>(std::clamp(tm.base_param_Delta_enable_mode, 0, 7));
        out->base_param_delta[i] = to_code(tm.base_param_Delta, 127, "base_param_Delta");
      }

      out->spline_flag[i] = tm.three_Spline_enable_flag ? 1 : 0;
      if (out->spline_flag[i]) {
        out->spline_num[i] = static_cast<unsigned int>(std::clamp(tm.three_Spline_num, 1, 2));
        for (unsigned int j = 0; j < out->spline_num[i]; ++j) {
          const AVHDRVivid3SplineParams &s = tm.three_spline[j];
          out->spline_th_mode[i][j] = static_cast<unsigned int>(std::clamp(s.th_mode, 0, 3));
          if (s.th_mode == 0 || s.th_mode == 2) {
            out->spline_th_mb[i][j] = to_code(s.th_enable_mb, 255, "three_spline.th_enable_mb");
          }
          out->spline_th[i][j][0] = to_code(s.th_enable, 4095, "three_spline.th_enable");
          out->spline_th[i][j][1] = to_code(s.th_delta1, 1023, "three_spline.th_delta1");
          out->spline_th[i][j][2] = to_code(s.th_delta2, 1023, "three_spline.th_delta2");
          out->spline_strength[i][j] = to_code(s.enable_strength, 255, "three_spline.enable_strength");
        }
      }
    }
  }

  out->color_saturation_mapping_flag = p.color_saturation_mapping_flag ? 1 : 0;
  if (out->color_saturation_mapping_flag) {
    out->color_saturation_num = static_cast<unsigned int>(std::clamp(p.color_saturation_num, 0, 7));
    for (unsigned int i = 0; i < out->color_saturation_num; ++i) {
      out->color_saturation_gain[i] = to_code(p.color_saturation_gain[i], 128, 255, "color_saturation_gain");
    }
  }

  return true;
}

double MasteringDisplayMaxLuminance(const AVFrame *frame) {
  AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
  if (!sd || sd->size < static_cast<int>(sizeof(AVMasteringDisplayMetadata))) {
    return 1000.0;
  }

  const auto *mastering = reinterpret_cast<const AVMasteringDisplayMetadata *>(sd->data);
  if (!mastering->has_luminance || mastering->max_luminance.den == 0) {
    return 1000.0;
  }

  const double luma = av_q2d(mastering->max_luminance);
  return std::isfinite(luma) && luma > 0.0 ? luma : 1000.0;
}

} // namespace EYEQ
