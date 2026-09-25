#pragma once

namespace EYEQ {

/**
 * @brief Raw HDR Vivid 1.0 dynamic metadata code values for one frame
 *
 * Field names follow the CUVA / T-UWA 005.1 (GB/T 41893.1) syntax elements. Values are the
 * unscaled bitstream codes, which is what the curve solver expects.
 */
struct HDRVividMetadata {
  unsigned int system_start_code;

  unsigned int minimum_maxrgb;
  unsigned int average_maxrgb;
  unsigned int variance_maxrgb;
  unsigned int maximum_maxrgb;

  unsigned int tone_mapping_mode;
  unsigned int tone_mapping_param_num;

  // Up to two parameter sets, indexed by target display luminance
  unsigned int targeted_system_display_maximum_luminance[2];

  unsigned int base_flag[2];
  unsigned int base_param_m_p[2];
  unsigned int base_param_m_m[2];
  unsigned int base_param_m_a[2];
  unsigned int base_param_m_b[2];
  unsigned int base_param_m_n[2];
  unsigned int base_param_k1[2];
  unsigned int base_param_k2[2];
  unsigned int base_param_k3[2];
  unsigned int base_param_delta_mode[2];
  unsigned int base_param_delta[2];

  unsigned int spline_flag[2];
  unsigned int spline_num[2];
  unsigned int spline_th_mode[2][4];
  unsigned int spline_th_mb[2][4];
  unsigned int spline_th[2][4][3];
  unsigned int spline_strength[2][4];

  unsigned int color_saturation_mapping_flag;
  unsigned int color_saturation_num;
  unsigned int color_saturation_gain[16];
};

/**
 * @brief Tone mapping curve solved from one frame of dynamic metadata
 *
 * The curve is piecewise: an optional low-luminance spline, an optional high-luminance
 * spline, and a parametric base curve everywhere else. All thresholds and the input to the
 * curve are PQ code values.
 */
struct HDRVividCurve {
  // Base curve
  float m_p, m_m, m_a, m_b, m_n;
  float k1, k2, k3;

  // Low-luminance spline
  int low_valid;
  float th1, th2, th3;
  float d1, c1, b1, a1;
  float d2, c2, b2, a2;
  float dark_slope, dark_offset;

  // High-luminance spline
  int high_valid;
  float th1_high, th2_high, th3_high;
  float d1_high, c1_high, b1_high, a1_high;
  float d2_high, c2_high, b2_high, a2_high;
  int high_clamped; // 1: hold the last segment instead of extending it linearly

  float tml; // target display luminance, PQ code value
  float rml; // mastering display luminance, PQ code value, never below tml
};

/**
 * @brief Solve the tone mapping curve for one frame
 *
 * Pure parameter math, no pixels are touched.
 *
 * @param meta Dynamic metadata of the frame
 * @param target_nits Target display peak luminance
 * @param mastering_nits Mastering display peak luminance
 */
void SolveHDRVividCurve(const HDRVividMetadata &meta, double target_nits, double mastering_nits, HDRVividCurve *out);

/**
 * @brief Evaluate the solved curve, mapping a PQ code value to a PQ code value
 *
 * Kept in sync with the GLSL implementation in hdrvivid_shader.cpp.
 */
double EvalHDRVividCurve(const HDRVividCurve &curve, double value);

/**
 * @brief PQ (SMPTE ST 2084) transfer function, linear values normalized so 1.0 is 10000 nits
 */
double HDRVividPQDecode(double pq);
double HDRVividPQEncode(double linear);

} // namespace EYEQ
