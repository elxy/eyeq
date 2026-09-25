#include "hdrvivid_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// HDR Vivid 1.0 tone mapping curve, as specified by T/UWA 005.1 (GB/T 41893.1).
//
// Only the curve parameters are solved here; the per-pixel mapping runs in the GLSL shader in
// display/hdrvivid_shader.cpp. The algorithm follows the reference implementation published
// with the standard, restricted to the HDR Vivid to HDR10/PQ case.

namespace EYEQ {
namespace {

// Bit widths of the metadata syntax elements, expressed as the maximum code value
constexpr double kMaxRGBMax = 4095.0;
constexpr double kTargetedLuminanceMax = 4095.0;
constexpr float kTargetedLuminanceMaxF = 4095.0f;
constexpr double kBaseParamMpMax = 16383.0;
constexpr double kBaseParamMaMax = 1023.0;
constexpr double kBaseParamMbMax = 1023.0;
constexpr double kBaseParamDeltaMax = 127.0;
constexpr double kSplineTh1Max = 4095.0;
constexpr double kSplineTh2Max = 1023.0;
constexpr double kSplineTh3Max = 1023.0;
constexpr float kSplineThMbMaxF = 255.0f;
constexpr double kSplineStrengthMax = 255.0;
constexpr int kSplineThOffsetBits = 2;

// Metadata code of the luminance that separates the HDR and SDR parameter sets
constexpr int kHdrSdrSplitCode = 2080;

/**
 * @brief Curve state while solving, in double precision
 */
struct WorkCurve {
  double m_p = 0, m_m = 0, m_a = 0, m_b = 0, m_n = 0;
  double k1 = 0, k2 = 0, k3 = 0;

  int low_valid = 0;
  double th1 = 0, th2 = 0, th3 = 0;
  double d1 = 0, c1 = 0, b1 = 0, a1 = 0;
  double d2 = 0, c2 = 0, b2 = 0, a2 = 0;
  double dark_slope = 0, dark_offset = 0;

  int high_valid = 0;
  double th1_high = 0, th2_high = 0, th3_high = 0;
  double d1_high = 0, c1_high = 0, b1_high = 0, a1_high = 0;
  double d2_high = 0, c2_high = 0, b2_high = 0, a2_high = 0;
  int high_clamped = 0;

  // Set when the base curve gain had to be capped, which also shifts the dark spline
  int gain_capped = 0;
  double m_a_cap = 0;
};

/**
 * @brief Parametric base curve, mapping a PQ code value to a PQ code value
 */
double BaseCurve(const WorkCurve &c, double x) {
  const double v = std::pow(x, c.m_n);
  double y = c.m_p * v / ((c.k1 * c.m_p - c.k2) * v + c.k3);
  y = std::pow(y, c.m_m);
  return c.m_a * y + c.m_b;
}

/**
 * @brief Derivative of the base curve, used to make the splines join smoothly
 */
double BaseCurveSlope(const WorkCurve &c, double x) {
  const double v = std::pow(x, c.m_n);
  const double v1 = std::pow(x, c.m_n - 1);
  double s = c.m_p * v / ((c.k1 * c.m_p - c.k2) * v + c.k3);
  s = std::pow(s, c.m_m + 1);
  return c.m_a * c.m_m * c.m_p * c.k3 * c.m_n * v1 * s * (1.0 / std::pow(v * c.m_p, 2));
}

/**
 * @brief Cap the base curve gain so dark areas are not lifted too aggressively
 *
 * The cap is a function of m_p, linearly interpolated between the tabulated points.
 */
void CapBaseGain(WorkCurve *c) {
  constexpr double kGainCap[4][2] = {{2.5, 0.99}, {3.5, 0.879}, {4.5, 0.777}, {7.5, 0.54}};
  constexpr int kNum = 4;

  int index = 0;
  double cap = kGainCap[0][1];
  for (int i = 0; i < kNum; i++) {
    if (c->m_p <= kGainCap[i][0]) {
      index = i;
      break;
    }
  }

  if (index == 0 && c->m_p <= kGainCap[0][0]) {
    cap = kGainCap[0][1];
  } else if (index == 0 && c->m_p > kGainCap[kNum - 1][0]) {
    cap = kGainCap[kNum - 1][1];
  } else {
    const double w1 = c->m_p - kGainCap[index - 1][0];
    const double w2 = kGainCap[index][0] - c->m_p;
    cap = (kGainCap[index][1] * w1 + kGainCap[index - 1][1] * w2) / (w1 + w2);
  }

  c->gain_capped = 0;
  if (c->m_a > cap) {
    c->m_a = c->m_a_cap = cap;
    c->m_b = 0;
    c->gain_capped = 1;
  }
}

/**
 * @brief Compensate the dark spline for a capped base curve gain
 *
 * Only applies to the standard base curve shape; a custom m_m or m_n means the content
 * author already tuned the curve.
 */
void CompensateCappedGain(double max_e, double input_max_e, WorkCurve *c) {
  if (!c->gain_capped) {
    return;
  }
  if (c->m_m < 2.35 || c->m_m > 2.45 || c->m_n < 0.95 || c->m_n > 1.05) {
    return;
  }

  if (input_max_e < max_e) {
    input_max_e = max_e;
  }

  const double target_ratio = max_e / input_max_e;

  double mapped = c->m_p * std::pow(input_max_e, c->m_n);
  const double den = (c->m_p * c->k1 - c->k2) * std::pow(input_max_e, c->m_n) + c->k3;
  if (den) {
    mapped /= den;
  }
  mapped = c->m_a_cap * std::pow(mapped, c->m_m) + c->m_b;
  const double capped_ratio = mapped / input_max_e;

  double weight = target_ratio - capped_ratio;
  if (weight < 0) {
    weight = 0;
  }
  weight /= (1 - capped_ratio);

  if (c->low_valid) {
    c->dark_slope += (1 - c->dark_slope) * weight;

    const double th1_old = c->th1;
    const double th2_old = c->th2;
    const double th3_old = c->th3;
    c->th1 += (input_max_e - c->th1) * weight;
    c->th2 = c->th1 + th2_old - th1_old;
    c->th3 = c->th2 + th3_old - th2_old;
    c->m_b *= (1 - weight);
  }
}

/**
 * @brief Solve the cubic spline coefficients for one segment pair
 *
 * Both spline variants join (th1, a1) to (th3, y2) through (th2, a2) with prescribed end
 * slopes, so the coefficient math is shared.
 */
void SolveSplineCoefficients(double h1, double h2, double a1, double b1, double a2, double y1, double y2, double s2,
                             double *d1, double *c1, double *b2_out, double *d2, double *c2) {
  const double b2 = -(3.0 * a1 * h2 * h2 + 3.0 * a2 * h1 * h1 - 3.0 * h1 * h1 * y2 - 3.0 * h2 * h2 * y1 +
                      h1 * h1 * h2 * s2 + b1 * h1 * h2 * h2) /
                    (2.0 * h2 * (h1 * h1 + h2 * h1));
  const double c1_v = (3.0 * y1 - 2.0 * b1 * h1 - 3.0 * a1 - b2 * h1) / (h1 * h1);
  const double d1_v = (h1 * b1 + h1 * b2 + 2 * a1 - 2.0 * y1) / (h1 * h1 * h1);

  *b2_out = b2;
  *c1 = c1_v;
  *d1 = d1_v;
  *c2 = c1_v + 3.0 * d1_v * h1;
  *d2 = -(y2 - a2 - h2 * s2 + c1_v * h2 * h2 + 3 * d1_v * h1 * h2 * h2) / (2 * h2 * h2 * h2);
}

/**
 * @brief Derive the base curve from the frame statistics and the metadata parameter set
 *
 * @param master_pq Mastering display luminance as a PQ code value
 * @param max_display Target display luminance as a PQ code value
 * @param min_display Target display black level as a PQ code value
 */
void SolveBaseCurve(const HDRVividMetadata &meta, double master_pq, double max_display, double min_display,
                    WorkCurve *c, double *max_e_out, double *input_max_e_out) {
  const double maximum_maxrgb = static_cast<double>(meta.maximum_maxrgb) / kMaxRGBMax;
  const double average_maxrgb = static_cast<double>(meta.average_maxrgb) / kMaxRGBMax;
  const double variance_maxrgb = static_cast<double>(meta.variance_maxrgb) / kMaxRGBMax;
  const double mean_var = average_maxrgb + variance_maxrgb / 2;

  c->m_m = 2.4;
  c->m_n = 1.0;
  c->m_b = 0.0;
  c->k1 = 1.0;
  c->k2 = 1.0;
  c->k3 = 1.0;

  // Brighter scenes get a gentler knee
  if (average_maxrgb > 0.6) {
    c->m_p = 3.5;
  } else if (average_maxrgb > 0.3) {
    c->m_p = 4.0 + (average_maxrgb - 0.3) / (0.6 - 0.3) * (3.5 - 4.0);
  } else {
    c->m_p = 4.0;
  }

  const double max_e = max_display;
  const double min_e = min_display;
  const double input_min_e = 0;

  double input_max_e = 0.8 * mean_var + 0.2 * maximum_maxrgb;
  if (input_max_e > master_pq) {
    input_max_e = master_pq;
  } else if (input_max_e < 0.5081) {
    input_max_e = 0.5081;
  }
  if (input_max_e < max_e) {
    input_max_e = max_e;
  }

  if (input_max_e > 0.9) {
    c->m_p += 0.6;
  } else if (input_max_e > 0.75) {
    c->m_p += (input_max_e - 0.75) / (0.9 - 0.75) * 0.6;
  }

  const double tm_min = std::pow(c->m_p * input_min_e / ((c->m_p - 1) * input_min_e + 1.0), c->m_m);
  const double tm_max = std::pow(c->m_p * input_max_e / ((c->m_p - 1) * input_max_e + 1.0), c->m_m);
  c->m_a = (max_e - min_e) / (tm_max - tm_min);
  c->m_b = min_display;

  // Pick the metadata parameter set matching the target display, then blend or offset it
  // towards the target luminance according to the delta mode
  const int max_display_code = static_cast<int>(max_display * kTargetedLuminanceMax);

  for (int i = 0; i < 2; i++) {
    const int target_code = static_cast<int>(meta.targeted_system_display_maximum_luminance[i]);
    // Single precision division and comparison, matching the fixed point behaviour the
    // metadata was authored against
    const double targeted = static_cast<float>(target_code) / kTargetedLuminanceMaxF;

    const bool hdr_sdr_mismatch = (target_code == kHdrSdrSplitCode) != (max_display_code == kHdrSdrSplitCode);
    if (meta.base_flag[i] == 0 || hdr_sdr_mismatch) {
      continue;
    }

    const double targeted_linear = static_cast<float>(10000 * HDRVividPQDecode(targeted));
    const double max_display_linear = static_cast<float>(10000 * HDRVividPQDecode(max_display));
    const double delta_i = std::pow(std::fabs(max_display_linear - targeted_linear) / 100, 0.5);

    const double prev_m_p = c->m_p;
    const double prev_m_m = c->m_m;
    const double prev_m_n = c->m_n;
    const double prev_k1 = c->k1;
    const double prev_k2 = c->k2;
    const double prev_k3 = c->k3;

    c->m_p = 10 * static_cast<double>(meta.base_param_m_p[i]) / kBaseParamMpMax;
    c->m_m = static_cast<double>(meta.base_param_m_m[i]) / 10;
    c->m_a = static_cast<double>(meta.base_param_m_a[i]) / kBaseParamMaMax;
    c->m_b = static_cast<double>(meta.base_param_m_b[i]) / (kBaseParamMbMax * 4);
    c->m_n = static_cast<double>(meta.base_param_m_n[i]) / 10;
    c->k1 = static_cast<double>(meta.base_param_k1[i]);
    c->k2 = static_cast<double>(meta.base_param_k2[i]);
    c->k3 = static_cast<double>(meta.base_param_k3[i]);
    if (meta.base_param_k3[i] == 2) {
      c->k3 = maximum_maxrgb;
    }

    const unsigned int delta_mode = meta.base_param_delta_mode[i];
    if (targeted_linear == max_display_linear || delta_mode == 3) {
      // The parameter set already targets this display
      break;
    }

    if (delta_mode == 0 || delta_mode == 2 || delta_mode == 4 || delta_mode == 6) {
      // Offset the knee, then rescale the gain to the actual target luminance
      double delta = static_cast<double>(meta.base_param_delta[i]) / kBaseParamDeltaMax;
      if (delta_mode == 2 || delta_mode == 6) {
        delta = -delta;
      }

      c->m_p = std::clamp(c->m_p + delta_i * delta, 3.0, 7.5);
      c->m_a *= (max_display - min_display) / targeted;
      c->m_b *= (max_display - min_display) / targeted;
      break;
    }

    if (delta_mode == 1 || delta_mode == 5) {
      // Blend between the metadata parameters and the derived ones
      const double delta = static_cast<double>(meta.base_param_delta[i]) / kBaseParamDeltaMax;
      const double weight = std::min(std::fabs(delta_i * delta), 1.0);
      const double inv = 1 - weight;

      c->m_p = inv * c->m_p + weight * prev_m_p;
      c->m_m = inv * c->m_m + weight * prev_m_m;
      c->m_n = inv * c->m_n + weight * prev_m_n;
      c->k1 = inv * c->k1 + weight * prev_k1;
      c->k2 = inv * c->k2 + weight * prev_k2;
      c->k3 = inv * c->k3 + weight * prev_k3;

      const double blend_min = std::pow(c->m_p * std::pow(input_min_e, c->m_n) /
                                            ((c->k1 * c->m_p - c->k2) * std::pow(input_min_e, c->m_n) + c->k3),
                                        c->m_m);
      const double blend_max = std::pow(c->m_p * std::pow(input_max_e, c->m_n) /
                                            ((c->k1 * c->m_p - c->k2) * std::pow(input_max_e, c->m_n) + c->k3),
                                        c->m_m);
      c->m_a = (max_e - min_e) / (blend_max - blend_min);
      c->m_b = min_e;
      break;
    }
  }

  *max_e_out = max_e;
  *input_max_e_out = input_max_e;
}

/**
 * @brief Default dark spline, used when the metadata does not carry one
 *
 * Thresholds are derived from the average brightness of the frame.
 */
void SolveDefaultLowSpline(WorkCurve *c, double average_maxrgb, double max_e, double input_max_e,
                           unsigned int delta_mode, unsigned int base_flag) {
  double th1 = 0.0;
  double slope = 1.0;
  if (average_maxrgb > 0.6) {
    th1 = 0.1;
    slope = 0.96;
  } else if (average_maxrgb > 0.3) {
    th1 = 0.25 + (average_maxrgb - 0.3) / (0.6 - 0.3) * (0.1 - 0.25);
    slope = 1.0 + (average_maxrgb - 0.3) / (0.6 - 0.3) * (0.96 - 1.0);
  } else {
    th1 = 0.25;
    slope = 1.0;
  }

  if (delta_mode < 3 && base_flag == 1) {
    WorkCurve probe = *c;
    CapBaseGain(&probe);
    probe.th1 = th1;
    probe.th2 = th1 + 0.15;
    probe.th3 = probe.th2 + (probe.th2 - probe.th1) / 2.0;
    probe.dark_slope = slope;
    probe.low_valid = 1;
    CompensateCappedGain(max_e, input_max_e, &probe);
    th1 = probe.th1;
    slope = probe.dark_slope;
  }

  const double th2 = th1 + 0.15;
  const double th3 = th2 + (th2 - th1) / 2.0;

  const double s2 = BaseCurveSlope(*c, th3);
  const double a1 = slope * th1 + c->dark_offset;
  const double b1 = slope;
  const double y2 = BaseCurve(*c, th3);

  double a2 = BaseCurve(*c, th2);
  if (th3 - th1) {
    a2 = th2 * (y2 - a1) / (th3 - th1) + (th3 * a1 - y2 * th1) / (th3 - th1);
  }
  const double y1 = a2;

  SolveSplineCoefficients(th2 - th1, th3 - th2, a1, b1, a2, y1, y2, s2, &c->d1, &c->c1, &c->b2, &c->d2, &c->c2);
  c->th1 = th1;
  c->th2 = th2;
  c->th3 = th3;
  c->b1 = b1;
  c->a1 = a1;
  c->a2 = a2;
  c->dark_slope = slope;
  c->low_valid = 1;
}

/**
 * @brief Dark spline described by the metadata (three_Spline th_mode 0)
 */
void SolveLowSpline(WorkCurve *c, double th1, double th2, double th3, double slope, double strength,
                    unsigned int delta_mode) {
  const double s2 = BaseCurveSlope(*c, th3);
  const double a1 = slope * th1 + c->dark_offset;
  const double b1 = slope;

  // Modes 2, 3 and 6 are authored against a fixed reference and must not be clamped
  const bool clampable = delta_mode != 2 && delta_mode != 3 && delta_mode != 6;

  double y2 = BaseCurve(*c, th3);
  if (y2 > th3 && clampable) {
    c->m_b -= (y2 - th3);
    y2 = th3;
  }

  double a2 = a1 + (y2 - a1) * (th2 - th1) / (th3 - th1) + (y2 - a1) * strength / 2;
  if (a2 > th2 && clampable) {
    a2 = th2;
  }
  const double y1 = a2;

  SolveSplineCoefficients(th2 - th1, th3 - th2, a1, b1, a2, y1, y2, s2, &c->d1, &c->c1, &c->b2, &c->d2, &c->c2);
  c->th1 = th1;
  c->th2 = th2;
  c->th3 = th3;
  c->b1 = b1;
  c->a1 = a1;
  c->a2 = a2;
  c->dark_slope = slope;
  c->low_valid = 1;
}

/**
 * @brief Highlight spline described by the metadata (three_Spline th_mode 1, 2 or 3)
 *
 * @param th_mode Highlight spline mode, decides where the segment ends and with what slope
 * @param reference_display Luminance of the metadata parameter set, as a PQ code value
 */
void SolveHighSpline(WorkCurve *c, int th_mode, double th1, double th2, double th3, double mb, double strength,
                     double max_display, double reference_display, unsigned int delta_mode) {
  const double b1 = BaseCurveSlope(*c, th1);
  const double a1 = BaseCurve(*c, th1);
  const double y1 = a1;

  double y2 = max_display;
  if (delta_mode == 3) {
    y2 = reference_display;
  }
  if (th_mode == 3) {
    y2 = BaseCurve(*c, th3);
  }

  const bool clampable = delta_mode != 2 && delta_mode != 3 && delta_mode != 6;
  const bool clampable_mode = th_mode == 1 || th_mode == 2;

  if (clampable_mode && y2 > th3 && clampable) {
    th3 = y2;
    th2 = th1 + (th3 - th1) / 2.0;
  }

  double a2 = y1 + (y2 - y1) * (th2 - th1) / (th3 - th1) + (y2 - y1) * strength / 2;
  if (clampable_mode && a2 > th2 && clampable) {
    a2 = th2;
  }

  double s2 = 1.0;
  if (th_mode == 2 || th_mode == 3) {
    s2 = BaseCurveSlope(*c, th3);
    if (th_mode == 2) {
      s2 -= mb;
    }
  } else {
    const double up = std::max((y2 - y1) / (th3 - th2), b1);
    const double mid = (y2 - y1) / (th3 - th1);
    const double down = std::max((y2 - y1) * 0.1 / (th3 - th1), b1);

    s2 = strength >= 0 ? (up * strength + mid * (1 - strength)) : (down * (-strength) + mid * (1 + strength));
    s2 = std::min(s2, 1.0);
  }

  if (th3 == y2 && clampable_mode && clampable) {
    s2 = 1.0;
  }

  SolveSplineCoefficients(th2 - th1, th3 - th2, a1, b1, a2, a2, y2, s2, &c->d1_high, &c->c1_high, &c->b2_high,
                          &c->d2_high, &c->c2_high);
  c->th1_high = th1;
  c->th2_high = th2;
  c->th3_high = th3;
  c->b1_high = b1;
  c->a1_high = a1;
  c->a2_high = a2;
  c->high_valid = 1;
}

/**
 * @brief Apply the spline sections of the metadata on top of the base curve
 */
void SolveSplines(const HDRVividMetadata &meta, double max_e, double input_max_e, WorkCurve *c) {
  const double average_maxrgb = static_cast<double>(meta.average_maxrgb) / kMaxRGBMax;
  const int max_display_code = static_cast<int>(max_e * kTargetedLuminanceMax);

  // Locate the parameter set matching the target display, for the splines and for the
  // delta mode respectively
  int spline_set = -1;
  int base_set = -1;
  double reference_display = 0.67658;

  for (int i = 0; i < 2; i++) {
    const int target_code = static_cast<int>(meta.targeted_system_display_maximum_luminance[i]);
    reference_display = static_cast<double>(target_code) / kTargetedLuminanceMax;
    const bool matches = (target_code == kHdrSdrSplitCode) == (max_display_code == kHdrSdrSplitCode);
    if (matches && meta.spline_flag[i]) {
      spline_set = i;
      break;
    }
  }
  for (int i = 0; i < 2; i++) {
    const int target_code = static_cast<int>(meta.targeted_system_display_maximum_luminance[i]);
    reference_display = static_cast<double>(target_code) / kTargetedLuminanceMax;
    if ((target_code == kHdrSdrSplitCode) == (max_display_code == kHdrSdrSplitCode)) {
      base_set = i;
      break;
    }
  }

  bool has_dark_spline = false;
  if (spline_set >= 0) {
    for (unsigned int i = 0; i < meta.spline_num[spline_set]; i++) {
      if (meta.spline_th_mode[spline_set][i] == 0) {
        has_dark_spline = true;
      }
    }
  }

  if (!has_dark_spline) {
    c->dark_offset = 0.0;
    unsigned int delta_mode = 0;
    unsigned int base_flag = 0;
    if (spline_set >= 0) {
      delta_mode = meta.base_param_delta_mode[spline_set];
      base_flag = meta.base_flag[spline_set];
    } else if (base_set >= 0) {
      delta_mode = meta.base_param_delta_mode[base_set];
      base_flag = meta.base_flag[base_set];
    }
    SolveDefaultLowSpline(c, average_maxrgb, max_e, input_max_e, delta_mode, base_flag);
  }

  if (spline_set < 0) {
    return;
  }

  const unsigned int delta_mode = meta.base_param_delta_mode[spline_set];

  for (unsigned int i = 0; i < meta.spline_num[spline_set]; i++) {
    const unsigned int th_mode = meta.spline_th_mode[spline_set][i];
    const unsigned int mb_code = meta.spline_th_mb[spline_set][i];
    const double strength = static_cast<double>(meta.spline_strength[spline_set][i] * 2) / kSplineStrengthMax - 1.0;

    // th_enable_mb doubles as a dark offset in mode 0 and as a slope elsewhere
    const int offset_code = mb_code & ((1 << kSplineThOffsetBits) - 1);
    double mb = static_cast<double>(mb_code >> kSplineThOffsetBits) / ((1 << (8 - kSplineThOffsetBits)) - 1);
    if (th_mode == 0) {
      c->dark_offset = offset_code * 0.1 / ((1 << kSplineThOffsetBits) - 1);
    } else {
      mb = static_cast<float>(mb_code * 1.1) / kSplineThMbMaxF;
    }

    double th1 = static_cast<double>(meta.spline_th[spline_set][i][0]) / kSplineTh1Max;
    double th2 = static_cast<double>(meta.spline_th[spline_set][i][1]) / (kSplineTh2Max * 4) + th1;
    double th3 = static_cast<double>(meta.spline_th[spline_set][i][2]) / (kSplineTh3Max * 4) + th2;

    if (th_mode == 0) {
      WorkCurve probe = *c;
      const bool compensate = delta_mode < 3 && meta.base_flag[spline_set] == 1;
      if (compensate) {
        CapBaseGain(&probe);
      }
      probe.th1 = th1;
      probe.th2 = th2;
      probe.th3 = th3;
      probe.dark_slope = mb;
      probe.low_valid = 1;
      if (compensate) {
        CompensateCappedGain(max_e, input_max_e, &probe);
      }
      th1 = probe.th1;
      th2 = probe.th2;
      th3 = probe.th3;
      c->m_b = probe.m_b;
      mb = probe.dark_slope;

      SolveLowSpline(c, th1, th2, th3, mb, strength, delta_mode);
    } else if (th_mode == 1 || th_mode == 2 || th_mode == 3) {
      // The highlight spline must start above the dark spline
      if (c->low_valid) {
        if (th3 <= c->th3) {
          continue;
        }
        if (th1 < c->th3) {
          th1 = c->th3;
          th2 = (th1 + th3) / 2;
        }
      }

      SolveHighSpline(c, static_cast<int>(th_mode), th1, th2, th3, mb, strength, max_e, reference_display, delta_mode);
      if (th_mode == 3) {
        c->high_clamped = 1;
      }
    }
  }
}

double Cubic(double d, double c, double b, double a, double t) { return ((d * t + c) * t + b) * t + a; }

} // namespace

double HDRVividPQDecode(double pq) {
  constexpr double m1 = 2610.0 / (4096.0 * 4.0);
  constexpr double m2 = 2523.0 * 128.0 / 4096.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 * 32.0 / 4096.0;
  constexpr double c3 = 2392.0 * 32.0 / 4096.0;

  pq = std::clamp(pq, 0.0, 1.0);
  const double t = std::pow(pq, 1.0 / m2);
  return std::pow(std::max(0.0, t - c1) / (c2 - c3 * t), 1.0 / m1);
}

double HDRVividPQEncode(double linear) {
  constexpr double m1 = 2610.0 / (4096.0 * 4.0);
  constexpr double m2 = 2523.0 * 128.0 / 4096.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 * 32.0 / 4096.0;
  constexpr double c3 = 2392.0 * 32.0 / 4096.0;

  linear = std::clamp(linear, 0.0, 1.0);
  const double t = std::pow(linear, m1);
  return std::pow((c2 * t + c1) / (1.0 + c3 * t), m2);
}

void SolveHDRVividCurve(const HDRVividMetadata &meta, double target_nits, double mastering_nits, HDRVividCurve *out) {
  // 0.05 nits black level for the HDR set top box case, see CUVA 005.3-3-2021 table 3
  const double max_display = HDRVividPQEncode(target_nits / 10000.0);
  const double min_display = HDRVividPQEncode(0.05 / 10000.0);
  const double master_pq = HDRVividPQEncode(mastering_nits / 10000.0);

  WorkCurve c;
  double max_e = 0;
  double input_max_e = 0;
  SolveBaseCurve(meta, master_pq, max_display, min_display, &c, &max_e, &input_max_e);
  SolveSplines(meta, max_e, input_max_e, &c);

  std::memset(out, 0, sizeof(*out));
  out->m_p = static_cast<float>(c.m_p);
  out->m_m = static_cast<float>(c.m_m);
  out->m_a = static_cast<float>(c.m_a);
  out->m_b = static_cast<float>(c.m_b);
  out->m_n = static_cast<float>(c.m_n);
  out->k1 = static_cast<float>(c.k1);
  out->k2 = static_cast<float>(c.k2);
  out->k3 = static_cast<float>(c.k3);

  out->low_valid = c.low_valid;
  out->th1 = static_cast<float>(c.th1);
  out->th2 = static_cast<float>(c.th2);
  out->th3 = static_cast<float>(c.th3);
  out->d1 = static_cast<float>(c.d1);
  out->c1 = static_cast<float>(c.c1);
  out->b1 = static_cast<float>(c.b1);
  out->a1 = static_cast<float>(c.a1);
  out->d2 = static_cast<float>(c.d2);
  out->c2 = static_cast<float>(c.c2);
  out->b2 = static_cast<float>(c.b2);
  out->a2 = static_cast<float>(c.a2);
  out->dark_slope = static_cast<float>(c.dark_slope);
  out->dark_offset = static_cast<float>(c.dark_offset);

  out->high_valid = c.high_valid;
  out->th1_high = static_cast<float>(c.th1_high);
  out->th2_high = static_cast<float>(c.th2_high);
  out->th3_high = static_cast<float>(c.th3_high);
  out->d1_high = static_cast<float>(c.d1_high);
  out->c1_high = static_cast<float>(c.c1_high);
  out->b1_high = static_cast<float>(c.b1_high);
  out->a1_high = static_cast<float>(c.a1_high);
  out->d2_high = static_cast<float>(c.d2_high);
  out->c2_high = static_cast<float>(c.c2_high);
  out->b2_high = static_cast<float>(c.b2_high);
  out->a2_high = static_cast<float>(c.a2_high);
  out->high_clamped = c.high_clamped;

  out->tml = static_cast<float>(max_e);
  out->rml = static_cast<float>(std::max(master_pq, max_e));
}

double EvalHDRVividCurve(const HDRVividCurve &curve, double x) {
  const bool low = curve.low_valid != 0;
  const bool high = curve.high_valid != 0;

  if (low) {
    if (x <= curve.th1) {
      return curve.dark_slope * x + curve.dark_offset;
    }
    if (x <= curve.th2) {
      return Cubic(curve.d1, curve.c1, curve.b1, curve.a1, x - curve.th1);
    }
    if (x <= curve.th3) {
      return Cubic(curve.d2, curve.c2, curve.b2, curve.a2, x - curve.th2);
    }
  }

  if (high) {
    if (x > curve.th1_high && x <= curve.th2_high) {
      return Cubic(curve.d1_high, curve.c1_high, curve.b1_high, curve.a1_high, x - curve.th1_high);
    }
    if (x > curve.th2_high && x <= curve.th3_high) {
      return Cubic(curve.d2_high, curve.c2_high, curve.b2_high, curve.a2_high, x - curve.th2_high);
    }
    if (x > curve.th3_high && curve.high_clamped == 0) {
      // Extend the last segment linearly along its end slope
      const double t = curve.th3_high - curve.th2_high;
      const double slope = 3.0 * curve.d2_high * t * t + 2.0 * curve.c2_high * t + curve.b2_high;
      return slope * (x - curve.th3_high) + Cubic(curve.d2_high, curve.c2_high, curve.b2_high, curve.a2_high, t);
    }
  }

  WorkCurve base;
  base.m_p = curve.m_p;
  base.m_m = curve.m_m;
  base.m_a = curve.m_a;
  base.m_b = curve.m_b;
  base.m_n = curve.m_n;
  base.k1 = curve.k1;
  base.k2 = curve.k2;
  base.k3 = curve.k3;
  return BaseCurve(base, x);
}

} // namespace EYEQ
