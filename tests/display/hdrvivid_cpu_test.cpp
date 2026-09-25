// CPU-side tests for the HDR Vivid 1.0 building blocks and for the Dolby Vision
// metadata handover. Pure CPU: no GPU, no window, no DeckLink SDK. The curve
// solver is the oracle the GPU hook is compared against, so its known points are
// fixed here independently of any rendering.
//
// The metadata used below is synthesized, not decoded from real material: it is
// the only way to pin exact code values, and it exercises the same parsing and
// solving code paths the player uses.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_vivid_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/rational.h>
}
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/hdrvivid_curve.hpp"
#include "display/hdrvivid_meta.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (condition) {
    std::fprintf(stdout, "  ok   %s\n", what.c_str());
  } else {
    std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

void CheckNear(double actual, double expected, double tolerance, const std::string &what) {
  const bool ok = std::fabs(actual - expected) <= tolerance;
  if (!ok) {
    std::fprintf(stderr, "       actual=%.6f expected=%.6f tol=%.6g\n", actual, expected, tolerance);
  }
  Check(ok, what);
}

// FFmpeg stores every CUVA code as code/denominator; the solver multiplies the
// denominator back out, so a rational of {code, den} round-trips exactly.
AVRational Code(int code, int den) { return AVRational{code, den}; }

// PQ code (0..1) that corresponds to @p nits, expressed as the 12-bit code the
// metadata carries for the targeted display luminance.
int TargetLuminanceCode(double nits) {
  const double pq = EYEQ::HDRVividPQEncode(nits / 10000.0);
  return static_cast<int>(std::lround(pq * 4095.0));
}

// Which CUVA spline blocks the synthesized metadata declares. "None" leaves
// spline_flag clear, so the solver falls back to its default dark spline.
enum class SplineKind { None, DarkMetadata, High1, High2, High3, DarkAndHigh };

const char *SplineKindName(SplineKind kind) {
  switch (kind) {
  case SplineKind::None:
    return "none (default low spline)";
  case SplineKind::DarkMetadata:
    return "th_mode 0 (metadata dark spline)";
  case SplineKind::High1:
    return "th_mode 1 (highlight)";
  case SplineKind::High2:
    return "th_mode 2 (highlight)";
  case SplineKind::High3:
    return "th_mode 3 (clamped highlight)";
  case SplineKind::DarkAndHigh:
    return "th_mode 0 + th_mode 1";
  }
  return "?";
}

// Thresholds in PQ code units plus the packed th_enable_mb and strength codes.
struct SplineSpec {
  int th_mode;
  double th1, th2, th3;
  int th_mb_code;
  unsigned strength_code;
};

SplineSpec DarkSplineSpec() { return {0, 0.15, 0.30, 0.42, (40 << 2) | 0, 200}; }
SplineSpec HighSplineSpec(int th_mode) { return {th_mode, 0.55, 0.64, 0.72, 40, 150}; }

void ApplySplines(EYEQ::HDRVividMetadata *meta, const SplineSpec *specs, int count) {
  for (int set = 0; set < 2; ++set) {
    meta->spline_flag[set] = count > 0 ? 1 : 0;
    meta->spline_num[set] = static_cast<unsigned>(count);
    for (int i = 0; i < count; ++i) {
      const SplineSpec &s = specs[i];
      meta->spline_th_mode[set][i] = static_cast<unsigned>(s.th_mode);
      meta->spline_th_mb[set][i] = static_cast<unsigned>(s.th_mb_code);
      meta->spline_th[set][i][0] = static_cast<unsigned>(std::lround(s.th1 * 4095.0));
      meta->spline_th[set][i][1] = static_cast<unsigned>(std::lround((s.th2 - s.th1) * 1023.0 * 4.0));
      meta->spline_th[set][i][2] = static_cast<unsigned>(std::lround((s.th3 - s.th2) * 1023.0 * 4.0));
      meta->spline_strength[set][i] = s.strength_code;
    }
  }
}

// A code value chosen so the CUVA "input peak" term (0.8*mean + 0.2*max) sits
// clearly below the mastering peak: a typical bright HDR scene.
//
// The base-curve gain m_a is authored so the curve maps the solver's input peak
// (= tml here, because the content peak is below the target) onto the target
// itself, i.e. the target is a fixed point of the authored curve. That is what
// makes the known-point assertion below meaningful rather than tautological.
EYEQ::HDRVividMetadata MakeMetadata(double target_nits, SplineKind spline = SplineKind::None) {
  EYEQ::HDRVividMetadata meta{};
  std::memset(&meta, 0, sizeof(meta));
  meta.system_start_code = 1;
  const unsigned average = static_cast<unsigned>(std::lround(0.35 * 4095.0));
  const unsigned variance = static_cast<unsigned>(std::lround(0.02 * 4095.0));
  const unsigned maximum = static_cast<unsigned>(std::lround(0.90 * 4095.0));
  meta.minimum_maxrgb = 0;
  meta.average_maxrgb = average;
  meta.variance_maxrgb = variance;
  meta.maximum_maxrgb = maximum;
  meta.tone_mapping_mode = 1;
  meta.tone_mapping_param_num = 2;

  const int target_code = TargetLuminanceCode(target_nits);
  const double tml = EYEQ::HDRVividPQEncode(target_nits / 10000.0);
  const double m_p = 5.0;
  const double m_m = 2.4;
  double input_peak = 0.8 * (average / 4095.0 + variance / 4095.0 / 2.0) + 0.2 * (maximum / 4095.0);
  if (input_peak < 0.5081) {
    input_peak = 0.5081; // CUVA floor near 100 nits
  }
  if (input_peak < tml) {
    input_peak = tml;
  }
  const double knee = m_p * input_peak / ((m_p - 1.0) * input_peak + 1.0);
  const double m_a = tml / std::pow(knee, m_m);

  for (int i = 0; i < 2; ++i) {
    meta.targeted_system_display_maximum_luminance[i] = static_cast<unsigned>(target_code);
    meta.base_flag[i] = 1;
    // A representative CUVA base curve (m_p = 5.0, m_m = 2.4, m_n = 1, k = 1).
    meta.base_param_m_p[i] = static_cast<unsigned>(std::lround(m_p * 16383.0 / 10.0));
    meta.base_param_m_m[i] = static_cast<unsigned>(std::lround(m_m * 10.0));
    meta.base_param_m_a[i] = static_cast<unsigned>(std::lround(m_a * 1023.0));
    meta.base_param_m_b[i] = 0;
    meta.base_param_m_n[i] = 10; // 10/10 = 1.0
    meta.base_param_k1[i] = 1;
    meta.base_param_k2[i] = 1;
    meta.base_param_k3[i] = 1;
    meta.base_param_delta_mode[i] = 3; // parameter set already targets the display
    meta.base_param_delta[i] = 0;
    meta.spline_flag[i] = 0;
  }

  SplineSpec specs[2];
  int count = 0;
  switch (spline) {
  case SplineKind::None:
    break;
  case SplineKind::DarkMetadata:
    specs[count++] = DarkSplineSpec();
    break;
  case SplineKind::High1:
    specs[count++] = HighSplineSpec(1);
    break;
  case SplineKind::High2:
    specs[count++] = HighSplineSpec(2);
    break;
  case SplineKind::High3:
    specs[count++] = HighSplineSpec(3);
    break;
  case SplineKind::DarkAndHigh:
    specs[count++] = DarkSplineSpec();
    specs[count++] = HighSplineSpec(1);
    break;
  }
  ApplySplines(&meta, specs, count);

  meta.color_saturation_mapping_flag = 0;
  meta.color_saturation_num = 0;
  return meta;
}

// ---------------------------------------------------------------------------
// PQ transfer
// ---------------------------------------------------------------------------
void TestPqRoundTrip() {
  std::fprintf(stdout, "[PQ transfer round-trip]\n");
  const double points[] = {0.0, 0.01, 0.1, 0.5, 0.75, 1.0};
  for (double pq : points) {
    const double linear = EYEQ::HDRVividPQDecode(pq);
    const double back = EYEQ::HDRVividPQEncode(linear);
    CheckNear(back, pq, 1e-5, "PQ decode/encode round-trips");
  }
  // Absolute anchors of ST 2084, each independently recomputed from the standard
  // constants: 0.5081 is 100 nits, 0.7518 is 1000 nits and 1.0 is 10000 nits.
  // The zero point is not exact because the inverse OETF asymptote c1^m2 is not
  // zero; the standard's own ~6e-6 residual is asserted instead of 0.
  CheckNear(EYEQ::HDRVividPQEncode(EYEQ::HDRVividPQDecode(0.0)), 0.0, 1e-5, "PQ zero stays within one code of zero");
  CheckNear(EYEQ::HDRVividPQDecode(1.0), 1.0, 1e-9, "PQ 1.0 is the 10000-nit endpoint");
  CheckNear(EYEQ::HDRVividPQEncode(100.0 / 10000.0), 0.5081, 5e-4, "PQ 0.5081 is 100 nits");
  CheckNear(EYEQ::HDRVividPQEncode(1000.0 / 10000.0), 0.7518, 1e-3, "PQ 0.7518 is 1000 nits");
  CheckNear(10000.0 * EYEQ::HDRVividPQDecode(0.5), 92.25, 0.5, "PQ 0.5 is ~92 nits");
}

// ---------------------------------------------------------------------------
// Curve solver known points
// ---------------------------------------------------------------------------
void TestCurveKnownPoints() {
  std::fprintf(stdout, "[HDR Vivid curve solver known points]\n");
  const double mastering_nits = 4000.0;
  const double target_nits = 1000.0;
  const EYEQ::HDRVividMetadata meta = MakeMetadata(target_nits);

  EYEQ::HDRVividCurve curve;
  EYEQ::SolveHDRVividCurve(meta, target_nits, mastering_nits, &curve);

  const double tml = EYEQ::HDRVividPQEncode(target_nits / 10000.0);
  const double rml = EYEQ::HDRVividPQEncode(mastering_nits / 10000.0);
  CheckNear(curve.tml, tml, 1e-4, "curve records the target display luminance (tml)");
  Check(std::fabs(curve.rml - rml) < 1e-3 || curve.rml > tml, "curve records a mastering peak at or above tml");

  // Monotonicity: a tone mapping curve must never invert a luminance ordering.
  bool monotonic = true;
  double previous = -1e30;
  for (int i = 0; i <= 200; ++i) {
    const double x = i / 200.0;
    const double y = EYEQ::EvalHDRVividCurve(curve, x);
    if (y + 1e-6 < previous) {
      monotonic = false;
    }
    previous = y;
  }
  Check(monotonic, "curve is monotonic non-decreasing over PQ [0,1]");

  // Black: PQ 0 maps to a small positive floor (never to the target peak).
  const double black = EYEQ::EvalHDRVividCurve(curve, 0.0);
  Check(black >= 0.0 && black < 0.05 * tml, "black (PQ 0) stays near the target black");

  // Target peak: the curve must map the target's own PQ code essentially to
  // itself, so a 1000-nit target rendered on a 1000-nit display is not dimmed.
  const double at_target = EYEQ::EvalHDRVividCurve(curve, tml);
  CheckNear(at_target, tml, 0.02, "target peak maps to itself (1000-nit target)");

  // The content peak is compressed towards the target: the curve must lower the
  // mastering peak and never exceed the full-scale PQ code.
  const double at_master = EYEQ::EvalHDRVividCurve(curve, rml);
  Check(at_master > at_target - 1e-6, "mapping stays monotonic past the target peak");
  Check(at_master < rml, "the mastering peak is compressed towards the target");
  Check(at_master <= 1.0, "the mapped mastering peak never exceeds full-scale PQ");

  // Reference white (203 nits) must land between black and the target peak, and
  // a 203-nit SDR-style grey must not be pushed above the target.
  const double ref_pq = EYEQ::HDRVividPQEncode(203.0 / 10000.0);
  const double ref = EYEQ::EvalHDRVividCurve(curve, ref_pq);
  Check(ref > 0.0 && ref < tml, "reference white is mapped inside (black, target]");

  // A lower target must compress harder: same source, 600-nit target.
  EYEQ::HDRVividCurve lower;
  EYEQ::SolveHDRVividCurve(meta, 600.0, mastering_nits, &lower);
  Check(lower.tml < curve.tml, "a lower target yields a lower target code");
  Check(EYEQ::EvalHDRVividCurve(lower, rml) <= at_master + 1e-6, "a lower target never maps the peak higher");
}

// ---------------------------------------------------------------------------
// Spline branch coverage
// ---------------------------------------------------------------------------
void TestSplineBranchSolving() {
  std::fprintf(stdout, "[HDR Vivid curve spline branches]\n");
  const double mastering_nits = 4000.0;
  const double target_nits = 1000.0;
  const SplineKind kinds[] = {SplineKind::None,  SplineKind::DarkMetadata, SplineKind::High1,
                              SplineKind::High2, SplineKind::High3,        SplineKind::DarkAndHigh};

  EYEQ::HDRVividCurve baseline;
  EYEQ::SolveHDRVividCurve(MakeMetadata(target_nits), target_nits, mastering_nits, &baseline);

  for (SplineKind kind : kinds) {
    const EYEQ::HDRVividMetadata meta = MakeMetadata(target_nits, kind);
    EYEQ::HDRVividCurve curve;
    EYEQ::SolveHDRVividCurve(meta, target_nits, mastering_nits, &curve);

    Check(curve.low_valid == 1, std::string(SplineKindName(kind)) + ": a dark spline is always present");
    Check(curve.tml > 0.0f, std::string(SplineKindName(kind)) + ": a target luminance is solved");

    bool monotonic = true;
    double previous = -1e30;
    for (int i = 0; i <= 200; ++i) {
      const double y = EYEQ::EvalHDRVividCurve(curve, i / 200.0);
      if (y + 1e-6 < previous) {
        monotonic = false;
      }
      previous = y;
    }
    Check(monotonic, std::string(SplineKindName(kind)) + ": the solved curve is monotonic");

    const bool metadata_dark = kind == SplineKind::DarkMetadata || kind == SplineKind::DarkAndHigh;
    if (metadata_dark) {
      CheckNear(curve.th1, 0.15, 0.02, std::string(SplineKindName(kind)) + ": dark-spline th1 comes from metadata");
      CheckNear(curve.th2, 0.30, 0.02, std::string(SplineKindName(kind)) + ": dark-spline th2 comes from metadata");
      CheckNear(curve.th3, 0.42, 0.02, std::string(SplineKindName(kind)) + ": dark-spline th3 comes from metadata");
      CheckNear(curve.dark_slope, 40.0 / 63.0, 0.02,
                std::string(SplineKindName(kind)) + ": dark-spline slope is decoded from th_enable_mb");
    } else {
      // The default dark spline is derived from the frame average brightness.
      CheckNear(curve.th1, 0.225, 0.02, std::string(SplineKindName(kind)) + ": default dark spline th1");
    }

    const bool expect_high = kind == SplineKind::High1 || kind == SplineKind::High2 || kind == SplineKind::High3 ||
                             kind == SplineKind::DarkAndHigh;
    Check(curve.high_valid == (expect_high ? 1 : 0),
          std::string(SplineKindName(kind)) + ": highlight-spline presence matches the metadata");
    if (expect_high) {
      CheckNear(curve.th1_high, 0.55, 0.02, std::string(SplineKindName(kind)) + ": highlight th1 from metadata");
      CheckNear(curve.th3_high, 0.74, 0.03, std::string(SplineKindName(kind)) + ": highlight th3 from metadata");
      const bool expect_clamped = kind == SplineKind::High3;
      Check(curve.high_clamped == (expect_clamped ? 1 : 0),
            std::string(SplineKindName(kind)) + ": th_mode 3 is the only clamped highlight mode");
    }

    if (kind != SplineKind::None) {
      double max_delta = 0.0;
      for (int i = 0; i <= 200; ++i) {
        const double x = i / 200.0;
        max_delta =
            std::max(max_delta, std::fabs(EYEQ::EvalHDRVividCurve(curve, x) - EYEQ::EvalHDRVividCurve(baseline, x)));
      }
      std::fprintf(stdout, "      %-42s max |spline - default| = %.5f\n", SplineKindName(kind), max_delta);
      Check(max_delta > 1e-4, std::string(SplineKindName(kind)) + ": the metadata spline changes the curve");
    }
  }

  // A metadata dark spline with the high-threshold set must keep the highlight
  // spline above it (the solver drops a highlight that starts inside the dark
  // region); this is the documented ordering rule.
  EYEQ::HDRVividCurve ordered;
  EYEQ::SolveHDRVividCurve(MakeMetadata(target_nits, SplineKind::DarkAndHigh), target_nits, mastering_nits, &ordered);
  Check(ordered.high_valid == 1 && ordered.th1_high > ordered.th3, "the highlight spline starts above the dark spline");
}

// ---------------------------------------------------------------------------
// Spline-strength boundary (characterization, not a correctness claim)
// ---------------------------------------------------------------------------
//
// At near-maximum spline strength the cubic pieces can overshoot by a fraction of
// a 10-bit code value. This records the measured bound instead of asserting
// strict monotonicity that the solver does not guarantee. Whether the CUVA
// reference implementation behaves identically is NOT verified here.
void TestSplineStrengthBoundary() {
  std::fprintf(stdout, "[HDR Vivid spline strength boundary]\n");
  EYEQ::HDRVividMetadata meta = MakeMetadata(1000.0);
  // Rewrite the spline block with the strongest legal strength (code 240 -> +0.88).
  const SplineSpec spec{1, 0.55, 0.64, 0.72, 40, 240};
  ApplySplines(&meta, &spec, 1);

  EYEQ::HDRVividCurve curve;
  EYEQ::SolveHDRVividCurve(meta, 1000.0, 4000.0, &curve);

  double previous = -1e30;
  double worst_backstep = 0.0;
  for (int i = 0; i <= 400; ++i) {
    const double y = EYEQ::EvalHDRVividCurve(curve, i / 400.0);
    worst_backstep = std::max(worst_backstep, previous - y);
    previous = y;
  }
  std::fprintf(stdout, "      strength code 240: max backstep = %.6f PQ code (1 10-bit code = %.6f)\n", worst_backstep,
               1.0 / 1023.0);
  Check(worst_backstep < 1e-3, "near-maximum spline strength overshoot stays below 1e-3 PQ code");
}

// ---------------------------------------------------------------------------
// Metadata parsing
// ---------------------------------------------------------------------------
EYEQ::HDRVividMetadata WriteVividSideData(AVFrame *frame, double target_nits) {
  AVDynamicHDRVivid *vivid = av_dynamic_hdr_vivid_create_side_data(frame);
  if (!vivid) {
    throw std::runtime_error("av_dynamic_hdr_vivid_create_side_data failed");
  }
  vivid->system_start_code = 1;
  vivid->num_windows = 1;
  AVHDRVividColorTransformParams &p = vivid->params[0];
  p.minimum_maxrgb = Code(120, 4095);
  p.average_maxrgb = Code(1433, 4095);
  p.variance_maxrgb = Code(82, 4095);
  p.maximum_maxrgb = Code(3685, 4095);
  p.tone_mapping_mode_flag = 1;
  p.tone_mapping_param_num = 1;
  p.tm_params[0].targeted_system_display_maximum_luminance = Code(TargetLuminanceCode(target_nits), 4095);
  p.tm_params[0].base_enable_flag = 1;
  p.tm_params[0].base_param_m_p = Code(8192, 16383);
  p.tm_params[0].base_param_m_m = Code(24, 10);
  p.tm_params[0].base_param_m_a = Code(700, 1023);
  p.tm_params[0].base_param_m_b = Code(10, 1023);
  p.tm_params[0].base_param_m_n = Code(10, 10);
  p.tm_params[0].base_param_k1 = 1;
  p.tm_params[0].base_param_k2 = 1;
  p.tm_params[0].base_param_k3 = 2;
  p.tm_params[0].base_param_Delta_enable_mode = 3;
  p.tm_params[0].base_param_Delta = Code(0, 127);
  p.tm_params[0].three_Spline_enable_flag = 1;
  p.tm_params[0].three_Spline_num = 1;
  p.tm_params[0].three_spline[0].th_mode = 2;
  p.tm_params[0].three_spline[0].th_enable_mb = Code(10, 255);
  p.tm_params[0].three_spline[0].th_enable = Code(1024, 4095);
  p.tm_params[0].three_spline[0].th_delta1 = Code(100, 1023);
  p.tm_params[0].three_spline[0].th_delta2 = Code(50, 1023);
  p.tm_params[0].three_spline[0].enable_strength = Code(200, 255);
  p.color_saturation_mapping_flag = 1;
  p.color_saturation_num = 2;
  p.color_saturation_gain[0] = Code(120, 128);
  p.color_saturation_gain[1] = Code(130, 128);

  EYEQ::HDRVividMetadata meta{};
  if (!EYEQ::ParseHDRVividMetadata(frame, &meta)) {
    throw std::runtime_error("ParseHDRVividMetadata rejected the synthesized side data");
  }
  return meta;
}

void TestMetadataParsing() {
  std::fprintf(stdout, "[HDR Vivid metadata parsing]\n");
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("av_frame_alloc failed");
  }

  const EYEQ::HDRVividMetadata meta = WriteVividSideData(frame, 1000.0);
  Check(meta.system_start_code == 1, "system start code parsed");
  Check(meta.minimum_maxrgb == 120, "minimum_maxrgb code parsed exactly");
  Check(meta.average_maxrgb == 1433, "average_maxrgb code parsed exactly");
  Check(meta.variance_maxrgb == 82, "variance_maxrgb code parsed exactly");
  Check(meta.maximum_maxrgb == 3685, "maximum_maxrgb code parsed exactly");
  Check(meta.tone_mapping_mode == 1, "tone mapping mode parsed");
  Check(meta.tone_mapping_param_num == 1, "tone mapping parameter count parsed");
  Check(meta.targeted_system_display_maximum_luminance[0] == static_cast<unsigned>(TargetLuminanceCode(1000.0)),
        "targeted display luminance code parsed");
  Check(meta.base_flag[0] == 1, "base parameter flag parsed");
  Check(meta.base_param_m_p[0] == 8192, "base_param_m_p parsed exactly");
  Check(meta.base_param_m_m[0] == 24, "base_param_m_m parsed exactly");
  Check(meta.base_param_m_n[0] == 10, "base_param_m_n parsed exactly");
  Check(meta.base_param_k3[0] == 2, "base_param_k3 parsed exactly");
  Check(meta.base_param_delta_mode[0] == 3, "delta mode parsed");
  Check(meta.spline_flag[0] == 1 && meta.spline_num[0] == 1, "spline flag/count parsed");
  Check(meta.spline_th_mode[0][0] == 2, "spline th_mode parsed");
  Check(meta.spline_th[0][0][0] == 1024, "spline threshold parsed exactly");
  Check(meta.color_saturation_mapping_flag == 1 && meta.color_saturation_num == 2, "saturation flag/count parsed");
  Check(meta.color_saturation_gain[0] == 120 && meta.color_saturation_gain[1] == 130, "saturation gains parsed");
  av_frame_free(&frame);
}

void TestMetadataAbsent() {
  std::fprintf(stdout, "[no dynamic metadata]\n");
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("av_frame_alloc failed");
  }
  EYEQ::HDRVividMetadata meta{};
  Check(!EYEQ::ParseHDRVividMetadata(frame, &meta), "a frame without HDR Vivid side data is rejected");

  AVDynamicHDRVivid *vivid = av_dynamic_hdr_vivid_create_side_data(frame);
  vivid->num_windows = 0; // no processing window: unusable
  Check(!EYEQ::ParseHDRVividMetadata(frame, &meta), "a Vivid block with no window is rejected");

  CheckNear(EYEQ::MasteringDisplayMaxLuminance(frame), 1000.0, 1e-9,
            "missing mastering display metadata falls back to the documented 1000 nits");
  av_frame_free(&frame);
}

void TestMasteringLuminance() {
  std::fprintf(stdout, "[mastering display luminance]\n");
  AVFrame *frame = av_frame_alloc();
  AVMasteringDisplayMetadata *mastering = av_mastering_display_metadata_create_side_data(frame);
  if (!mastering) {
    throw std::runtime_error("av_mastering_display_metadata_create_side_data failed");
  }
  mastering->has_luminance = 1;
  mastering->min_luminance = AVRational{1, 10000};
  mastering->max_luminance = AVRational{4000, 1};
  CheckNear(EYEQ::MasteringDisplayMaxLuminance(frame), 4000.0, 1e-9, "mastering display peak is read");
  av_frame_free(&frame);
}

} // namespace

int main() {
  try {
    TestPqRoundTrip();
    TestCurveKnownPoints();
    TestSplineBranchSolving();
    TestSplineStrengthBoundary();
    TestMetadataParsing();
    TestMetadataAbsent();
    TestMasteringLuminance();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }
  std::fprintf(stdout, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
