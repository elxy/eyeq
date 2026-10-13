// HDR Vivid / Dolby Vision integration tests for the single comparison renderer.
//
// Everything runs on a headless Vulkan device through the real FrameRenderer and
// its source-mapping compositor: no window, no DeckLink SDK. Both dynamic-HDR
// formats must land in the *same* common display-linear result, and the tests
// pin that down with:
//   - an equivalence case (same input twice is byte-identical),
//   - known luminance points checked against the CPU curve oracle,
//   - a hook-inertness case (no metadata => no change),
//   - processing-count and metadata-residue assertions,
//   - the Dolby Vision reshape path selected through map_dovi=true.
//
// The dynamic metadata is synthesized so the exact code values are known; real
// material is exercised separately (see the V2-4 report), because no
// frame-aligned Vivid/DV A/B pair exists on this machine.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_vivid_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/rational.h>
}
#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/frame_renderer.hpp"
#include "display/hdrvivid_curve.hpp"
#include "display/hdrvivid_shader.hpp"
#include "display/output_encode.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;
int g_skipped = 0;

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
  if (std::fabs(actual - expected) > tolerance) {
    std::fprintf(stderr, "       actual=%.6f expected=%.6f tol=%.6g\n", actual, expected, tolerance);
  }
  Check(std::fabs(actual - expected) <= tolerance, what);
}

using Color = std::array<float, 3>;
using PixelFunc = std::function<Color(int, int)>;

// A PQ / BT.2020 full-range RGB frame. libplacebo treats the stored float as the
// PQ code value, which is what both the CUVA curve and the DV reshape expect.
std::shared_ptr<AVFrame> MakePqRgbFrame(int width, int height, const PixelFunc &pixel) {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("av_frame_alloc failed");
  }
  frame->format = AV_PIX_FMT_GBRPF32LE;
  frame->width = width;
  frame->height = height;
  frame->colorspace = AVCOL_SPC_RGB;
  frame->color_range = AVCOL_RANGE_JPEG;
  frame->color_primaries = AVCOL_PRI_BT2020;
  frame->color_trc = AVCOL_TRC_SMPTE2084;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("av_frame_get_buffer failed");
  }
  for (int y = 0; y < height; ++y) {
    float *g = reinterpret_cast<float *>(frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0]);
    float *b = reinterpret_cast<float *>(frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1]);
    float *r = reinterpret_cast<float *>(frame->data[2] + static_cast<ptrdiff_t>(y) * frame->linesize[2]);
    for (int x = 0; x < width; ++x) {
      const Color c = pixel(x, y);
      g[x] = c[1];
      b[x] = c[2];
      r[x] = c[0];
    }
  }
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

void AttachMastering(AVFrame *frame, double peak_nits) {
  AVMasteringDisplayMetadata *mastering = av_mastering_display_metadata_create_side_data(frame);
  if (!mastering) {
    throw std::runtime_error("av_mastering_display_metadata_create_side_data failed");
  }
  mastering->has_luminance = 1;
  mastering->min_luminance = AVRational{1, 10000};
  mastering->max_luminance = AVRational{static_cast<int>(std::lround(peak_nits)), 1};
}

AVRational Code(int code, int den) { return AVRational{code, den}; }

int TargetLuminanceCode(double nits) {
  const double pq = EYEQ::HDRVividPQEncode(nits / 10000.0);
  return static_cast<int>(std::lround(pq * 4095.0));
}

// Which CUVA spline block the synthesized metadata declares, and whether the
// colour-saturation mapping is enabled. `None` leaves spline_flag clear so the
// solver falls back to its default dark spline (the V2-4 baseline).
enum class SplineKind { None, DarkMetadata, High1, High2, High3, DarkAndHigh };

const char *SplineKindName(SplineKind kind) {
  switch (kind) {
  case SplineKind::None:
    return "none";
  case SplineKind::DarkMetadata:
    return "th_mode 0";
  case SplineKind::High1:
    return "th_mode 1";
  case SplineKind::High2:
    return "th_mode 2";
  case SplineKind::High3:
    return "th_mode 3";
  case SplineKind::DarkAndHigh:
    return "th_mode 0+1";
  }
  return "?";
}

struct VividOptions {
  SplineKind spline = SplineKind::None;
  bool saturation = false;
  unsigned sat_gain0 = 120; // c0 = 120/128
  unsigned sat_gain1 = 3;   // c1 = (3 & 0xFC)/128 = 0, c2 = 2^(3 & 3) = 8
};

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
      const SplineSpec &sp = specs[i];
      meta->spline_th_mode[set][i] = static_cast<unsigned>(sp.th_mode);
      meta->spline_th_mb[set][i] = static_cast<unsigned>(sp.th_mb_code);
      meta->spline_th[set][i][0] = static_cast<unsigned>(std::lround(sp.th1 * 4095.0));
      meta->spline_th[set][i][1] = static_cast<unsigned>(std::lround((sp.th2 - sp.th1) * 1023.0 * 4.0));
      meta->spline_th[set][i][2] = static_cast<unsigned>(std::lround((sp.th3 - sp.th2) * 1023.0 * 4.0));
      meta->spline_strength[set][i] = sp.strength_code;
    }
  }
}

void ApplySplineKind(EYEQ::HDRVividMetadata *meta, SplineKind kind) {
  SplineSpec specs[2];
  int count = 0;
  switch (kind) {
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
  ApplySplines(meta, specs, count);
}

// The synthesized CUVA metadata: a bright scene with a base curve authored so
// the target is a fixed point, plus the requested spline/saturation blocks. The
// same struct is used for the C++ oracle and for the attached side data, so the
// GPU and the oracle are guaranteed to see identical numbers.
EYEQ::HDRVividMetadata VividMetadataFor(double target_nits, VividOptions opt = {}) {
  EYEQ::HDRVividMetadata meta{};
  std::memset(&meta, 0, sizeof(meta));
  meta.system_start_code = 1;
  const double average = 0.35;
  const double variance = 0.02;
  const double maximum = 0.90;
  meta.minimum_maxrgb = 0;
  meta.average_maxrgb = static_cast<unsigned>(std::lround(average * 4095.0));
  meta.variance_maxrgb = static_cast<unsigned>(std::lround(variance * 4095.0));
  meta.maximum_maxrgb = static_cast<unsigned>(std::lround(maximum * 4095.0));
  meta.tone_mapping_mode = 1;
  meta.tone_mapping_param_num = 2;
  const int target_code = TargetLuminanceCode(target_nits);
  const double tml = EYEQ::HDRVividPQEncode(target_nits / 10000.0);
  const double m_p = 5.0;
  const double m_m = 2.4;
  double input_peak = 0.8 * (average + variance / 2.0) + 0.2 * maximum;
  if (input_peak < 0.5081) {
    input_peak = 0.5081;
  }
  if (input_peak < tml) {
    input_peak = tml;
  }
  const double knee = m_p * input_peak / ((m_p - 1.0) * input_peak + 1.0);
  const double m_a = tml / std::pow(knee, m_m);
  for (int i = 0; i < 2; ++i) {
    meta.targeted_system_display_maximum_luminance[i] = static_cast<unsigned>(target_code);
    meta.base_flag[i] = 1;
    meta.base_param_m_p[i] = static_cast<unsigned>(std::lround(m_p * 16383.0 / 10.0));
    meta.base_param_m_m[i] = static_cast<unsigned>(std::lround(m_m * 10.0));
    meta.base_param_m_a[i] = static_cast<unsigned>(std::lround(m_a * 1023.0));
    meta.base_param_m_b[i] = 0;
    meta.base_param_m_n[i] = 10;
    meta.base_param_k1[i] = 1;
    meta.base_param_k2[i] = 1;
    meta.base_param_k3[i] = 1;
    meta.base_param_delta_mode[i] = 3;
    meta.base_param_delta[i] = 0;
    meta.spline_flag[i] = 0;
  }
  ApplySplineKind(&meta, opt.spline);
  meta.color_saturation_mapping_flag = opt.saturation ? 1 : 0;
  meta.color_saturation_num = opt.saturation ? 2 : 0;
  meta.color_saturation_gain[0] = opt.sat_gain0;
  meta.color_saturation_gain[1] = opt.sat_gain1;
  return meta;
}

// Attach a well-formed CUVA side data block so ParseHDRVividMetadata accepts it.
void AttachVivid(AVFrame *frame, double target_nits, VividOptions opt = {}) {
  AVDynamicHDRVivid *vivid = av_dynamic_hdr_vivid_create_side_data(frame);
  if (!vivid) {
    throw std::runtime_error("av_dynamic_hdr_vivid_create_side_data failed");
  }
  const EYEQ::HDRVividMetadata meta = VividMetadataFor(target_nits, opt);
  vivid->system_start_code = static_cast<uint8_t>(meta.system_start_code);
  vivid->num_windows = 1;
  AVHDRVividColorTransformParams &p = vivid->params[0];
  p.minimum_maxrgb = Code(static_cast<int>(meta.minimum_maxrgb), 4095);
  p.average_maxrgb = Code(static_cast<int>(meta.average_maxrgb), 4095);
  p.variance_maxrgb = Code(static_cast<int>(meta.variance_maxrgb), 4095);
  p.maximum_maxrgb = Code(static_cast<int>(meta.maximum_maxrgb), 4095);
  p.tone_mapping_mode_flag = 1;
  p.tone_mapping_param_num = static_cast<int>(meta.tone_mapping_param_num);
  for (int i = 0; i < 2; ++i) {
    p.tm_params[i].targeted_system_display_maximum_luminance =
        Code(static_cast<int>(meta.targeted_system_display_maximum_luminance[i]), 4095);
    p.tm_params[i].base_enable_flag = static_cast<int>(meta.base_flag[i]);
    p.tm_params[i].base_param_m_p = Code(static_cast<int>(meta.base_param_m_p[i]), 16383);
    p.tm_params[i].base_param_m_m = Code(static_cast<int>(meta.base_param_m_m[i]), 10);
    p.tm_params[i].base_param_m_a = Code(static_cast<int>(meta.base_param_m_a[i]), 1023);
    p.tm_params[i].base_param_m_b = Code(0, 1023);
    p.tm_params[i].base_param_m_n = Code(static_cast<int>(meta.base_param_m_n[i]), 10);
    p.tm_params[i].base_param_k1 = static_cast<int>(meta.base_param_k1[i]);
    p.tm_params[i].base_param_k2 = static_cast<int>(meta.base_param_k2[i]);
    p.tm_params[i].base_param_k3 = static_cast<int>(meta.base_param_k3[i]);
    p.tm_params[i].base_param_Delta_enable_mode = static_cast<int>(meta.base_param_delta_mode[i]);
    p.tm_params[i].base_param_Delta = Code(0, 127);
    p.tm_params[i].three_Spline_enable_flag = meta.spline_flag[i] ? 1 : 0;
    p.tm_params[i].three_Spline_num = static_cast<int>(meta.spline_num[i]);
    for (int k = 0; k < static_cast<int>(meta.spline_num[i]); ++k) {
      AVHDRVivid3SplineParams &sp = p.tm_params[i].three_spline[k];
      sp.th_mode = static_cast<int>(meta.spline_th_mode[i][k]);
      sp.th_enable_mb = Code(static_cast<int>(meta.spline_th_mb[i][k]), 255);
      sp.th_enable = Code(static_cast<int>(meta.spline_th[i][k][0]), 4095);
      // FFmpeg stores th_delta1/2 as a 10-bit code over 1023; the solver then
      // scales that code by 1/4, which is the CUVA 0..0.25 delta range.
      sp.th_delta1 = Code(static_cast<int>(meta.spline_th[i][k][1]), 1023);
      sp.th_delta2 = Code(static_cast<int>(meta.spline_th[i][k][2]), 1023);
      sp.enable_strength = Code(static_cast<int>(meta.spline_strength[i][k]), 255);
    }
  }
  p.color_saturation_mapping_flag = meta.color_saturation_mapping_flag ? 1 : 0;
  p.color_saturation_num = static_cast<int>(meta.color_saturation_num);
  for (int i = 0; i < static_cast<int>(meta.color_saturation_num); ++i) {
    p.color_saturation_gain[i] = Code(static_cast<int>(meta.color_saturation_gain[i]), 128);
  }
}

// A Dolby Vision RPU that is deliberately close to the identity: one polynomial
// segment per component with slope 1, an identity YCC->RGB matrix/offset and an
// identity RGB->LMS matrix. The decode then reduces to
// PQ-OETF(LMS->RGB * PQ-EOTF(input)), i.e. a neutral input stays neutral.
void AttachDolbyVision(AVFrame *frame) {
  size_t size = 0;
  AVDOVIMetadata *md = av_dovi_metadata_alloc(&size);
  if (!md) {
    throw std::runtime_error("av_dovi_metadata_alloc failed");
  }
  AVDOVIRpuDataHeader *header = av_dovi_get_header(md);
  header->rpu_type = 2;
  header->vdr_rpu_profile = 8;
  header->bl_bit_depth = 10;
  header->coef_log2_denom = 23;
  header->disable_residual_flag = 1; // no enhancement layer: libplacebo maps it

  AVDOVIDataMapping *mapping = av_dovi_get_mapping(md);
  for (int c = 0; c < 3; ++c) {
    AVDOVIReshapingCurve &curve = mapping->curves[c];
    curve.num_pivots = 2;
    curve.pivots[0] = 0;
    curve.pivots[1] = 1023;
    curve.mapping_idc[0] = AV_DOVI_MAPPING_POLYNOMIAL;
    curve.poly_order[0] = 1;
    curve.poly_coef[0][0] = 0;
    curve.poly_coef[0][1] = 1LL << header->coef_log2_denom; // slope 1
  }
  mapping->nlq_method_idc = AV_DOVI_NLQ_NONE;

  AVDOVIColorMetadata *color = av_dovi_get_color(md);
  const AVRational one = AVRational{1, 1};
  const AVRational zero = AVRational{0, 1};
  for (int i = 0; i < 9; ++i) {
    color->ycc_to_rgb_matrix[i] = (i % 4 == 0) ? one : zero;
    color->rgb_to_lms_matrix[i] = (i % 4 == 0) ? one : zero;
  }
  for (int i = 0; i < 3; ++i) {
    color->ycc_to_rgb_offset[i] = zero;
  }
  color->source_min_pq = 0;
  // A 1000-nit source peak, so the declared source luminance comes from the RPU
  // rather than from an arbitrary full-scale code.
  color->source_max_pq = static_cast<uint16_t>(TargetLuminanceCode(1000.0));

  AVFrameSideData *side = av_frame_new_side_data(frame, AV_FRAME_DATA_DOVI_METADATA, size);
  if (!side) {
    av_free(md);
    throw std::runtime_error("av_frame_new_side_data(DOVI_METADATA) failed");
  }
  std::memcpy(side->data, md, size);
  av_free(md);
}

EYEQ::DisplayState MakeState(int main_id) {
  EYEQ::DisplayState state{};
  state.ids = {main_id};
  state.scale = 1.0f;
  state.offset_x = 0.0f;
  state.offset_y = 0.0f;
  state.fill.id = main_id;
  state.slide.left_id = main_id;
  state.slide.right_id = main_id;
  state.slide.split_pos = 0.5f;
  state.grid.cols = 1;
  state.grid.rows = 1;
  return state;
}

EYEQ::MappingTarget MakeMappingTarget(float peak_nits) {
  EYEQ::MappingTarget mt;
  mt.primaries = PL_COLOR_PRIM_BT_2020;
  mt.peak_nits = peak_nits;
  mt.black_nits = 0.0f;
  mt.sdr_white_nits = PL_COLOR_SDR_WHITE;
  return mt;
}

std::unique_ptr<EYEQ::FrameRenderer> MakeRenderer(EYEQ::RenderContext &context, const EYEQ::MappingTarget &target,
                                                  bool vivid) {
  EYEQ::FrameRenderer::Config cfg;
  cfg.mode = EYEQ::DisplayMode::Fill;
  cfg.scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.plane_scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.pixel_density = 1.0f;
  cfg.enable_osd = false;
  cfg.colorspace_hint = false;
  cfg.present_dither = false;
  auto renderer = std::make_unique<EYEQ::FrameRenderer>(context, cfg, nullptr);
  renderer->SetMappingTarget(target, EYEQ::WorkPrecision::F32);
  renderer->SetSdrWhiteOnHdr(0.0f);
  renderer->SetHdrVividEnabled(vivid);
  renderer->SetValidateLinearResult(true);
  return renderer;
}

// IEEE-754 binary16 -> binary32. Needed because pl_tex_download writes the
// texture's native format: an RGBA16F work texture comes back as raw half floats,
// not as floats.
float HalfToFloat(uint16_t value) {
  const uint32_t sign = (value >> 15) & 1u;
  const uint32_t exponent = (value >> 10) & 0x1Fu;
  const uint32_t mantissa = value & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    bits = (sign << 31) | (mantissa == 0 ? 0u : (static_cast<uint32_t>(127 - 15 + 1) << 23));
  } else if (exponent == 0x1Fu) {
    bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
  } else {
    bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

// Download through the context's GPU (the renderer does not expose it). Handles
// both work-texture precisions.
std::vector<float> Download(pl_gpu gpu, pl_tex tex, int width, int height) {
  const std::size_t count = static_cast<std::size_t>(width) * height * 4;
  std::vector<float> pixels(count);
  const bool half = tex->params.format->type == PL_FMT_FLOAT && tex->params.format->component_depth[0] == 16;
  if (half) {
    std::vector<uint16_t> raw(count);
    struct pl_tex_transfer_params transfer = {
        .tex = tex,
        .row_pitch = static_cast<size_t>(width) * 4 * sizeof(uint16_t),
        .ptr = raw.data(),
    };
    if (!pl_tex_download(gpu, &transfer)) {
      throw std::runtime_error("RGBA16F texture download failed");
    }
    for (std::size_t i = 0; i < count; ++i) {
      pixels[i] = HalfToFloat(raw[i]);
    }
  } else {
    struct pl_tex_transfer_params transfer = {
        .tex = tex,
        .row_pitch = static_cast<size_t>(width) * 4 * sizeof(float),
        .ptr = pixels.data(),
    };
    if (!pl_tex_download(gpu, &transfer)) {
      throw std::runtime_error("texture download failed");
    }
  }
  return pixels;
}

float PixelLuma(const std::vector<float> &pixels, int width, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
  return pixels[i];
}

// Display-linear PL_HDR_NORM value (1.0 == 203 cd/m^2) for a PQ code.
double PqToDisplayLinearNorm(double pq) { return EYEQ::HDRVividPQDecode(pq) * 10000.0 / 203.0; }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
void TestVividCountsAndResidue(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid: single mapping, no residual metadata]\n");
  const int w = 32;
  const int h = 24;
  const float target_nits = 1000.0f;
  auto renderer = MakeRenderer(context, MakeMappingTarget(target_nits), /*vivid=*/true);

  const double input_pq = 0.6;
  auto frame = MakePqRgbFrame(w, h, [&](int, int) {
    return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
  });
  AttachMastering(frame.get(), 4000.0);
  AttachVivid(frame.get(), target_nits);
  renderer->ResetMappingCounts();
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
  renderer->FeedFrames(frames);
  Check(renderer->hdrvivid_frame_count() == 1, "one fed frame carried HDR Vivid metadata");

  const EYEQ::DisplayState state = MakeState(0);
  renderer->RenderScene(w, h, state);

  Check(renderer->MappingPassCount(0) == 1, "the source was mapped exactly once for one compose");
  Check(renderer->HdrVividApplyCount() == 1, "the HDR Vivid hook ran exactly once, not once per output");

  const struct pl_frame &result = renderer->GetLinearResult();
  Check(result.repr.sys == PL_COLOR_SYSTEM_RGB, "the common result is RGB, not a source YCC system");
  Check(result.repr.dovi == nullptr, "the common result carries no Dolby Vision reshape metadata");
  Check(result.color.transfer == PL_COLOR_TRC_LINEAR, "the common result is display-linear");
  Check(result.color.hdr.max_pq_y == 0.0f && result.color.hdr.avg_pq_y == 0.0f,
        "the common result carries no residual dynamic HDR metadata");
  CheckNear(result.color.hdr.max_luma, target_nits, 1.0, "the common result declares the mapping target peak");
}

void TestVividKnownPoints(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid: known luminance points vs the CPU curve oracle]\n");
  const int w = 16;
  const int h = 16;
  const float target_nits = 1000.0f;
  const double mastering_nits = 4000.0;
  auto renderer = MakeRenderer(context, MakeMappingTarget(target_nits), /*vivid=*/true);

  EYEQ::HDRVividCurve curve;
  EYEQ::SolveHDRVividCurve(VividMetadataFor(target_nits), target_nits, mastering_nits, &curve);

  // Four known inputs: SDR reference white (203 nits), a mid grey (400 nits),
  // the target level (1000 nits) and the mastering peak (4000 nits).
  const double known_nits[] = {203.0, 400.0, 1000.0, 4000.0};
  for (double nits : known_nits) {
    const double input_pq = EYEQ::HDRVividPQEncode(nits / 10000.0);
    auto frame = MakePqRgbFrame(w, h, [&](int, int) {
      return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
    });
    AttachMastering(frame.get(), mastering_nits);
    AttachVivid(frame.get(), target_nits);
    std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
    renderer->FeedFrames(frames);
    renderer->RenderScene(w, h, MakeState(0));

    const std::vector<float> pixels = Download(context.gpu(), renderer->GetLinearResultTexture(), w, h);
    const double mapped_pq = EYEQ::EvalHDRVividCurve(curve, input_pq);
    const double expected = PqToDisplayLinearNorm(mapped_pq);
    const double actual = PixelLuma(pixels, w, w / 2, h / 2);
    std::fprintf(stdout, "       %.0f nits: pq %.4f -> mapped pq %.4f -> linear %.4f (expected %.4f)\n", nits, input_pq,
                 mapped_pq, actual, expected);
    // 2% of the target peak plus a 1e-3 floor: the GPU path is F32 but the
    // generic mapper still applies its own transfer/primaries handling.
    const double tolerance = 0.02 * PqToDisplayLinearNorm(curve.tml) + 1e-3;
    CheckNear(actual, expected, tolerance, "mapped display-linear value matches the CPU oracle");
  }
}

void TestVividInertWithoutMetadata(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid: inert without dynamic metadata]\n");
  const int w = 24;
  const int h = 16;
  const double input_pq = 0.55;
  auto frame = MakePqRgbFrame(w, h, [&](int, int) {
    return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
  });
  AttachMastering(frame.get(), 4000.0);
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};

  auto off = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/false);
  off->FeedFrames(frames);
  off->RenderScene(w, h, MakeState(0));
  const std::vector<float> off_pixels = Download(context.gpu(), off->GetLinearResultTexture(), w, h);

  auto on = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/true);
  on->FeedFrames(frames);
  on->RenderScene(w, h, MakeState(0));
  const std::vector<float> on_pixels = Download(context.gpu(), on->GetLinearResultTexture(), w, h);

  Check(on->HdrVividApplyCount() == 0, "no hook was attached for a source without dynamic metadata");
  Check(off_pixels == on_pixels, "a source without HDR Vivid metadata renders byte-identically with the feature on");
}

void TestVividChangesResultAndEquivalence(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid: active, deterministic and equivalent]\n");
  const int w = 24;
  const int h = 16;
  const double input_pq = 0.75;
  auto frame = MakePqRgbFrame(w, h, [&](int, int) {
    return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
  });
  AttachMastering(frame.get(), 4000.0);
  AttachVivid(frame.get(), 1000.0);
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};

  auto off = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/false);
  off->FeedFrames(frames);
  off->RenderScene(w, h, MakeState(0));
  const std::vector<float> off_pixels = Download(context.gpu(), off->GetLinearResultTexture(), w, h);

  auto on_a = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/true);
  on_a->FeedFrames(frames);
  on_a->RenderScene(w, h, MakeState(0));
  const std::vector<float> on_a_pixels = Download(context.gpu(), on_a->GetLinearResultTexture(), w, h);

  auto on_b = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/true);
  on_b->FeedFrames(frames);
  on_b->RenderScene(w, h, MakeState(0));
  const std::vector<float> on_b_pixels = Download(context.gpu(), on_b->GetLinearResultTexture(), w, h);

  Check(off_pixels != on_a_pixels, "HDR Vivid tone mapping changes the common result when metadata is present");
  Check(on_a_pixels == on_b_pixels, "the same input through the same algorithm is byte-identical (equivalence case)");
  Check(on_a->HdrVividApplyCount() == 1 && on_b->HdrVividApplyCount() == 1, "each renderer applied the hook once");
}

void TestVividTargetMovesResult(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid: the common mapping target drives the curve]\n");
  const int w = 16;
  const int h = 16;
  const double input_pq = 0.75;
  auto frame = MakePqRgbFrame(w, h, [&](int, int) {
    return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
  });
  AttachMastering(frame.get(), 4000.0);
  AttachVivid(frame.get(), 1000.0);
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};

  auto high = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/true);
  high->FeedFrames(frames);
  high->RenderScene(w, h, MakeState(0));
  const std::vector<float> high_pixels = Download(context.gpu(), high->GetLinearResultTexture(), w, h);

  // Point the same source at a lower common target. A fresh frame is needed
  // because attaching side data again would leave the first block in place, and
  // the parser reads the first one.
  auto low_frame = MakePqRgbFrame(w, h, [&](int, int) {
    return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
  });
  AttachMastering(low_frame.get(), 4000.0);
  AttachVivid(low_frame.get(), 600.0);
  std::map<int, std::shared_ptr<AVFrame>> low_frames{{0, low_frame}};
  auto low = MakeRenderer(context, MakeMappingTarget(600.0f), /*vivid=*/true);
  low->FeedFrames(low_frames);
  low->RenderScene(w, h, MakeState(0));
  const std::vector<float> low_pixels = Download(context.gpu(), low->GetLinearResultTexture(), w, h);

  const float high_value = PixelLuma(high_pixels, w, w / 2, h / 2);
  const float low_value = PixelLuma(low_pixels, w, w / 2, h / 2);
  std::fprintf(stdout, "       1000-nit target -> %.4f, 600-nit target -> %.4f\n", high_value, low_value);
  Check(std::isfinite(high_value) && std::isfinite(low_value), "both targets produce a finite mapped value");
  Check(low_value < high_value, "a lower common target maps the same source darker");
}

void TestDolbyVisionPath(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[Dolby Vision: reshape path, common result, no residue]\n");
  const int w = 16;
  const int h = 16;
  const float target_nits = 1000.0f;
  auto renderer = MakeRenderer(context, MakeMappingTarget(target_nits), /*vivid=*/false);

  const double input_pq = 0.6;
  auto frame = MakePqRgbFrame(w, h, [&](int, int) {
    return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
  });
  AttachMastering(frame.get(), 1000.0);
  AttachDolbyVision(frame.get());
  renderer->ResetMappingCounts();
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
  renderer->FeedFrames(frames);

  const struct pl_frame *source = renderer->GetSourceFrame(0);
  Check(source != nullptr, "the mapped source frame is available");
  if (source != nullptr) {
    Check(source->repr.sys == PL_COLOR_SYSTEM_DOLBYVISION,
          "map_dovi=true selects the Dolby Vision color system on this build");
    Check(source->repr.dovi != nullptr, "the Dolby Vision reshape metadata reached libplacebo");
    Check(source->color.transfer == PL_COLOR_TRC_PQ && source->color.primaries == PL_COLOR_PRIM_BT_2020,
          "the Dolby Vision source is declared BT.2020 / PQ");
    CheckNear(source->color.hdr.max_luma, 1000.0, 1.0, "the Dolby Vision source peak comes from the RPU");
  }

  renderer->RenderScene(w, h, MakeState(0));
  Check(renderer->MappingPassCount(0) == 1, "the Dolby Vision source was decoded/reshaped exactly once");
  Check(renderer->HdrVividApplyCount() == 0, "the HDR Vivid hook does not run for a Dolby Vision source");

  const struct pl_frame &result = renderer->GetLinearResult();
  Check(result.repr.dovi == nullptr, "the common result carries no Dolby Vision reshape metadata");
  Check(result.color.hdr.max_pq_y == 0.0f && result.color.hdr.avg_pq_y == 0.0f,
        "the common result carries no per-frame Dolby Vision brightness metadata");

  const std::vector<float> pixels = Download(context.gpu(), renderer->GetLinearResultTexture(), w, h);
  const float neutral = PixelLuma(pixels, w, w / 2, h / 2);
  const double identity_expectation = PqToDisplayLinearNorm(input_pq);
  std::fprintf(stdout, "       neutral %.4f PQ -> linear %.4f (identity-RPU expectation %.4f)\n", input_pq, neutral,
               identity_expectation);
  Check(neutral > 0.0f && std::isfinite(neutral), "the Dolby Vision result is finite and non-zero");
  // The identity RPU still passes through the hard-coded BT.2020 LMS->RGB matrix,
  // whose neutral rows sum to ~1 but not exactly; 2% covers that plus the F32
  // compose. This is not a claim that the file matches Dolby display management.
  CheckNear(neutral, identity_expectation, 0.02 * identity_expectation + 1e-3,
            "an identity-RPU neutral grey stays within 2% of its input luminance");
}

void TestVividAndDolbyShareOneTarget(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[shared mapping exit: Vivid and Dolby Vision use one target]\n");
  const int w = 16;
  const int h = 16;
  const float target_nits = 1000.0f;
  auto renderer = MakeRenderer(context, MakeMappingTarget(target_nits), /*vivid=*/true);

  auto vivid_frame = MakePqRgbFrame(w, h, [](int, int) { return Color{0.6f, 0.6f, 0.6f}; });
  AttachMastering(vivid_frame.get(), 4000.0);
  AttachVivid(vivid_frame.get(), target_nits);
  auto dv_frame = MakePqRgbFrame(w, h, [](int, int) { return Color{0.6f, 0.6f, 0.6f}; });
  AttachMastering(dv_frame.get(), 1000.0);
  AttachDolbyVision(dv_frame.get());

  // Feed both sources in one call so they share a single mapping generation.
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, vivid_frame}, {1, dv_frame}};
  renderer->FeedFrames(frames);
  renderer->ResetMappingCounts();
  renderer->RenderScene(w, h, MakeState(0));

  Check(renderer->MappingPassCount(0) == 1, "the Vivid source was mapped exactly once");
  Check(renderer->MappingPassCount(1) == 0, "the Dolby Vision source was not mapped in a Vivid-only scene");
  // Both sources are mapped by the same renderer into the same target: switching
  // the displayed source must not change the target description.
  const struct pl_color_space vivid_target = renderer->GetLinearResult().color;
  renderer->SetMainSource(1);
  EYEQ::DisplayState state = MakeState(1);
  renderer->RenderScene(w, h, state);
  const struct pl_color_space dv_target = renderer->GetLinearResult().color;
  CheckNear(vivid_target.hdr.max_luma, dv_target.hdr.max_luma, 1e-6, "both sources map to the same target peak");
  CheckNear(vivid_target.hdr.min_luma, dv_target.hdr.min_luma, 1e-9, "both sources map to the same target black");
  Check(vivid_target.primaries == dv_target.primaries, "both sources map to the same target primaries");
  Check(renderer->MappingPassCount(1) == 1, "the Dolby Vision source was mapped exactly once after the switch");
}

// ---------------------------------------------------------------------------
// Luminance and saturation oracles (independent C++ transcriptions)
// ---------------------------------------------------------------------------

// The CUVA per-pixel mapping in the shader: a single linear ratio is applied to
// all three channels, then the result is re-encoded to PQ. Mirrors
// hdrvivid_shader.cpp's body (not hdrvivid_curve.cpp, which only evaluates the
// curve itself).
std::array<double, 3> MapPq(const EYEQ::HDRVividCurve &curve, double r, double g, double b) {
  const double lr = EYEQ::HDRVividPQDecode(r);
  const double lg = EYEQ::HDRVividPQDecode(g);
  const double lb = EYEQ::HDRVividPQDecode(b);
  const double max_lin = std::max({lr, lg, lb});
  const double max_pq = std::max({r, g, b});
  const double mapped_lin = EYEQ::HDRVividPQDecode(EYEQ::EvalHDRVividCurve(curve, max_pq));
  const double ratio = max_lin <= 0.0 ? 1.0 : mapped_lin / max_lin;
  return {EYEQ::HDRVividPQEncode(lr * ratio), EYEQ::HDRVividPQEncode(lg * ratio), EYEQ::HDRVividPQEncode(lb * ratio)};
}

// The colour-saturation adjustment of the shader, transcribed from the same
// CUVA formula. Its only purpose is to catch GLSL/C++ divergence: it is not an
// independent authority on the algorithm.
double SaturationStrength(const EYEQ::HDRVividShaderParams &p, double y) {
  if (p.saturation_c1 == 0.0f) {
    return p.saturation_b;
  }
  if (y >= p.curve.rml) {
    return std::max<double>(p.saturation_b - p.saturation_c1 * 0.4, 0.0);
  }
  if (y >= p.curve.tml) {
    const double ratio =
        std::pow((y - p.curve.tml) / std::max<double>(p.curve.rml - p.curve.tml, 1e-6), p.saturation_c2);
    return std::max<double>(p.saturation_b - p.saturation_c1 * 0.4 * ratio, 0.0);
  }
  return std::clamp(std::pow(EYEQ::EvalHDRVividCurve(p.curve, y) / std::max(y, 1e-6), p.saturation_c0), 0.8, 1.0);
}

// The strength is selected by the ORIGINAL input maximum (the shader passes
// `hv_max_pq`, not the mapped one), while the chroma it scales is the mapped
// result. Getting this wrong changes the result visibly, so it is spelled out.
std::array<double, 3> ApplySaturation(const EYEQ::HDRVividShaderParams &p, const std::array<double, 3> &pq,
                                      double input_max_pq) {
  const double y = 0.2627 * pq[0] + 0.678 * pq[1] + 0.0593 * pq[2];
  double u = -0.1396 * pq[0] - 0.3604 * pq[1] + 0.5 * pq[2];
  double v = 0.5 * pq[0] - 0.4598 * pq[1] - 0.0402 * pq[2];
  const double sm = SaturationStrength(p, input_max_pq);
  u *= sm;
  v *= sm;
  return {y + 1.4746 * v, y - 0.1645 * u - 0.5713 * v, y + 1.8814 * u};
}

std::array<float, 3> PixelRgb(const std::vector<float> &pixels, int width, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
  return {pixels[i], pixels[i + 1], pixels[i + 2]};
}

// ---------------------------------------------------------------------------
// Debt 1: spline branch coverage on the GPU against the CPU oracle
// ---------------------------------------------------------------------------
void TestVividSplineBranches(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid spline branches: GPU vs CPU oracle]\n");
  const int w = 16;
  const int h = 16;
  const float target_nits = 1000.0f;
  const double mastering_nits = 4000.0;
  // 0.60 sits inside the highlight-spline window (0.55..0.72); without it a
  // clamped (th_mode 3) highlight spline never changes a sampled point.
  const double inputs[] = {0.20, 0.50, 0.60, 0.7518, 0.90};
  constexpr int kInputCount = 5;
  const SplineKind kinds[] = {SplineKind::None,  SplineKind::DarkMetadata, SplineKind::High1,
                              SplineKind::High2, SplineKind::High3,        SplineKind::DarkAndHigh};

  std::array<double, kInputCount> baseline_luma{};
  for (SplineKind kind : kinds) {
    VividOptions opt;
    opt.spline = kind;
    auto renderer = MakeRenderer(context, MakeMappingTarget(target_nits), /*vivid=*/true);
    EYEQ::HDRVividCurve curve;
    EYEQ::SolveHDRVividCurve(VividMetadataFor(target_nits, opt), target_nits, mastering_nits, &curve);

    if ((kind == SplineKind::High1 || kind == SplineKind::High2 || kind == SplineKind::High3) &&
        curve.high_valid != 1) {
      throw std::runtime_error("highlight spline was not solved");
    }
    if ((kind == SplineKind::DarkMetadata || kind == SplineKind::DarkAndHigh) && std::fabs(curve.th1 - 0.15) > 0.02) {
      throw std::runtime_error("metadata dark spline was not applied");
    }

    double max_spline_delta = 0.0;
    for (int i = 0; i < kInputCount; ++i) {
      const double input_pq = inputs[i];
      auto frame = MakePqRgbFrame(w, h, [&](int, int) {
        return Color{static_cast<float>(input_pq), static_cast<float>(input_pq), static_cast<float>(input_pq)};
      });
      AttachMastering(frame.get(), mastering_nits);
      AttachVivid(frame.get(), target_nits, opt);
      std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
      renderer->FeedFrames(frames);
      renderer->RenderScene(w, h, MakeState(0));

      const std::vector<float> pixels = Download(context.gpu(), renderer->GetLinearResultTexture(), w, h);
      const double expected = PqToDisplayLinearNorm(EYEQ::EvalHDRVividCurve(curve, input_pq));
      const double actual = PixelLuma(pixels, w, w / 2, h / 2);
      const double tolerance = 0.02 * PqToDisplayLinearNorm(curve.tml) + 1e-3;
      CheckNear(actual, expected, tolerance, std::string(SplineKindName(kind)) + ": GPU matches the CPU curve oracle");

      if (kind == SplineKind::None) {
        baseline_luma[i] = actual;
      }
      max_spline_delta = std::max(max_spline_delta, std::fabs(actual - baseline_luma[i]));
    }
    if (kind != SplineKind::None) {
      std::fprintf(stdout, "      %-12s max |GPU spline - GPU default| = %.6f\n", SplineKindName(kind),
                   max_spline_delta);
      Check(max_spline_delta > 1e-4,
            std::string(SplineKindName(kind)) + ": the metadata spline changed the GPU result");
    }
  }
}

// ---------------------------------------------------------------------------
// Debt 1: colour-saturation mapping on the GPU
// ---------------------------------------------------------------------------
void TestVividSaturation(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[HDR Vivid colour-saturation mapping]\n");
  const int w = 16;
  const int h = 16;
  const float target_nits = 1000.0f;
  const double mastering_nits = 4000.0;

  VividOptions off;
  VividOptions on;
  on.saturation = true;
  // A non-zero c1 exercises the "y >= tml" saturation branch (the constant-only
  // branch is used when c1 == 0). c2 = 2^(gain1 & 3) = 1.
  on.sat_gain0 = 120; // c0 = 0.9375
  on.sat_gain1 = 16;  // c1 = 16/128 = 0.125

  const double neutral = 0.62;
  // The max channel sits in the compressive part of the curve (> the target
  // level), so the saturation strength is genuinely below 1.
  const Color colored{0.88f, 0.70f, 0.55f};

  auto render = [&](const Color &color, VividOptions opt) {
    auto renderer = MakeRenderer(context, MakeMappingTarget(target_nits), /*vivid=*/true);
    auto frame = MakePqRgbFrame(w, h, [&](int, int) { return color; });
    AttachMastering(frame.get(), mastering_nits);
    AttachVivid(frame.get(), target_nits, opt);
    std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
    renderer->FeedFrames(frames);
    renderer->RenderScene(w, h, MakeState(0));
    return Download(context.gpu(), renderer->GetLinearResultTexture(), w, h);
  };

  const std::vector<float> neutral_off =
      render(Color{static_cast<float>(neutral), static_cast<float>(neutral), static_cast<float>(neutral)}, off);
  const std::vector<float> neutral_on =
      render(Color{static_cast<float>(neutral), static_cast<float>(neutral), static_cast<float>(neutral)}, on);
  Check(neutral_off == neutral_on, "saturation leaves a neutral grey byte-identical (u = v = 0)");

  const std::vector<float> colored_off = render(colored, off);
  const std::vector<float> colored_on = render(colored, on);
  Check(colored_off != colored_on, "saturation changes a coloured input");

  // With saturation off, the coloured input must match the ratio-mapping oracle
  // channel by channel (this also pins the plane order of the source frame).
  {
    EYEQ::HDRVividShaderParams p_off;
    EYEQ::SolveHDRVividShaderParams(VividMetadataFor(target_nits, off), target_nits, mastering_nits, &p_off);
    const std::array<double, 3> mapped_off = MapPq(p_off.curve, colored[0], colored[1], colored[2]);
    const std::array<float, 3> actual_off = PixelRgb(colored_off, w, w / 2, h / 2);
    for (int c = 0; c < 3; ++c) {
      const double tolerance = 0.02 * PqToDisplayLinearNorm(p_off.curve.tml) + 2e-3;
      CheckNear(actual_off[c], PqToDisplayLinearNorm(mapped_off[c]), tolerance,
                "without saturation the coloured channel matches the ratio-mapping oracle");
    }
  }

  // Oracle: map with the solved params, apply the transcribed saturation, then
  // decode PQ to display-linear.
  EYEQ::HDRVividShaderParams params;
  EYEQ::SolveHDRVividShaderParams(VividMetadataFor(target_nits, on), target_nits, mastering_nits, &params);
  Check(params.valid && params.saturation_enabled, "the saturation parameters were solved");
  Check(params.saturation_b >= 0.8f && params.saturation_b <= 1.0f, "the saturation constant B is clamped to [0.8, 1]");
  CheckNear(params.saturation_c0, 120.0 / 128.0, 1e-6, "saturation c0 comes from gain[0]");
  CheckNear(params.saturation_c2, 1.0, 1e-6, "saturation c2 is 2^exponent from gain[1]");

  const std::array<double, 3> mapped = MapPq(params.curve, colored[0], colored[1], colored[2]);
  const std::array<double, 3> saturated =
      ApplySaturation(params, mapped, std::max({colored[0], colored[1], colored[2]}));
  const std::array<float, 3> actual = PixelRgb(colored_on, w, w / 2, h / 2);
  for (int c = 0; c < 3; ++c) {
    const double expected = PqToDisplayLinearNorm(saturated[c]);
    const double tolerance = 0.02 * PqToDisplayLinearNorm(params.curve.tml) + 2e-3;
    std::fprintf(stdout, "      channel %d: actual=%.6f expected=%.6f mapped_pq=%.5f saturated_pq=%.5f\n", c, actual[c],
                 expected, mapped[c], saturated[c]);
    CheckNear(actual[c], expected, tolerance, "saturated channel matches the transcribed CUVA oracle");
  }

  // The luminance row of the saturation matrix sums to 1, so the PQ-space Y is
  // preserved. (The display-linear Y is not, because PQ is non-linear; asserting
  // it there would be wrong.)
  const std::array<float, 3> off_rgb = PixelRgb(colored_off, w, w / 2, h / 2);
  auto to_pq = [](float norm) { return EYEQ::HDRVividPQEncode(norm * 203.0f / 10000.0f); };
  const double y_off = 0.2627 * to_pq(off_rgb[0]) + 0.678 * to_pq(off_rgb[1]) + 0.0593 * to_pq(off_rgb[2]);
  const double y_on = 0.2627 * to_pq(actual[0]) + 0.678 * to_pq(actual[1]) + 0.0593 * to_pq(actual[2]);
  std::fprintf(stdout, "      PQ-space Y: off=%.6f on=%.6f\n", y_off, y_on);
  CheckNear(y_on, y_off, 5e-3, "saturation preserves the PQ-space luminance component");

  const double chroma_off = std::hypot(off_rgb[0] - off_rgb[1], off_rgb[2] - off_rgb[1]);
  const double chroma_on = std::hypot(actual[0] - actual[1], actual[2] - actual[1]);
  std::fprintf(stdout, "      chroma (R-G,B-G distance): off=%.5f on=%.5f\n", chroma_off, chroma_on);
  Check(chroma_on < chroma_off, "saturation reduces chroma for a compressive tone curve");

  VividOptions disabled_gain = on;
  disabled_gain.saturation = false;
  EYEQ::HDRVividShaderParams no_sat;
  EYEQ::SolveHDRVividShaderParams(VividMetadataFor(target_nits, disabled_gain), target_nits, mastering_nits, &no_sat);
  Check(!no_sat.saturation_enabled, "saturation is disabled when the metadata flag is clear");
}

// ---------------------------------------------------------------------------
// Debt 2: 16F vs 32F on dynamic-HDR sources
// ---------------------------------------------------------------------------
struct PrecisionStats {
  double max_linear_diff = 0.0;
  double mean_linear_diff = 0.0;
  double max_code_diff = 0.0;
};

PrecisionStats ComparePrecision(const std::vector<float> &f16, const std::vector<float> &f32) {
  PrecisionStats stats;
  double sum = 0.0;
  std::size_t count = 0;
  EYEQ::EncodeParams pq;
  pq.transfer = EYEQ::OutputTransfer::PQ;
  for (std::size_t i = 0; i < f32.size(); ++i) {
    const double d = std::fabs(static_cast<double>(f16[i]) - static_cast<double>(f32[i]));
    stats.max_linear_diff = std::max(stats.max_linear_diff, d);
    sum += d;
    ++count;
    const double c16 = EYEQ::EncodeNormToCode(f16[i], pq);
    const double c32 = EYEQ::EncodeNormToCode(f32[i], pq);
    stats.max_code_diff = std::max(stats.max_code_diff, std::fabs(c16 - c32) * 1023.0);
  }
  stats.mean_linear_diff = count ? sum / count : 0.0;
  return stats;
}

std::vector<float> RenderAtPrecision(EYEQ::RenderContext &context, bool vivid, bool with_dovi,
                                     EYEQ::WorkPrecision precision) {
  const int w = 32;
  const int h = 24;
  auto renderer = MakeRenderer(context, MakeMappingTarget(1000.0f), vivid);
  renderer->SetMappingTarget(MakeMappingTarget(1000.0f), precision);
  auto frame = MakePqRgbFrame(w, h, [](int, int) { return Color{0.58f, 0.42f, 0.30f}; });
  AttachMastering(frame.get(), 4000.0);
  if (vivid) {
    AttachVivid(frame.get(), 1000.0);
  }
  if (with_dovi) {
    AttachDolbyVision(frame.get());
  }
  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
  renderer->FeedFrames(frames);
  renderer->RenderScene(w, h, MakeState(0));
  return Download(context.gpu(), renderer->GetLinearResultTexture(), w, h);
}

void TestPrecisionDynamicSources(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[16F vs 32F on dynamic-HDR sources]\n");

  const PrecisionStats vivid = ComparePrecision(RenderAtPrecision(context, true, false, EYEQ::WorkPrecision::F16),
                                                RenderAtPrecision(context, true, false, EYEQ::WorkPrecision::F32));
  const PrecisionStats dv = ComparePrecision(RenderAtPrecision(context, false, true, EYEQ::WorkPrecision::F16),
                                             RenderAtPrecision(context, false, true, EYEQ::WorkPrecision::F32));

  std::fprintf(stdout, "      Vivid source: max linear |16F-32F| = %.6f, mean = %.8f, max 10-bit code = %.4f\n",
               vivid.max_linear_diff, vivid.mean_linear_diff, vivid.max_code_diff);
  std::fprintf(stdout, "      DV source:    max linear |16F-32F| = %.6f, mean = %.8f, max 10-bit code = %.4f\n",
               dv.max_linear_diff, dv.mean_linear_diff, dv.max_code_diff);

  Check(std::isfinite(vivid.max_code_diff) && std::isfinite(dv.max_code_diff),
        "both dynamic-HDR sources produce finite results at F16 and F32");

  // Micro-difference visibility: two inputs one 10-bit PQ code apart must stay
  // ordered after the 16F work texture. This is a boundary measurement, not a
  // pass/fail release condition; the numbers are reported in the V2-4 debt report.
  const double step_code = 1.0 / 1023.0;
  for (double base_pq : {0.30, 0.55, 0.7518, 0.90}) {
    auto render_level = [&](double pq, EYEQ::WorkPrecision precision) {
      auto renderer = MakeRenderer(context, MakeMappingTarget(1000.0f), /*vivid=*/true);
      renderer->SetMappingTarget(MakeMappingTarget(1000.0f), precision);
      auto frame = MakePqRgbFrame(16, 16, [&](int, int) {
        return Color{static_cast<float>(pq), static_cast<float>(pq), static_cast<float>(pq)};
      });
      AttachMastering(frame.get(), 4000.0);
      AttachVivid(frame.get(), 1000.0);
      std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
      renderer->FeedFrames(frames);
      renderer->RenderScene(16, 16, MakeState(0));
      return PixelLuma(Download(context.gpu(), renderer->GetLinearResultTexture(), 16, 16), 16, 8, 8);
    };
    const float f32_lo = render_level(base_pq, EYEQ::WorkPrecision::F32);
    const float f32_hi = render_level(base_pq + step_code, EYEQ::WorkPrecision::F32);
    const float f16_lo = render_level(base_pq, EYEQ::WorkPrecision::F16);
    const float f16_hi = render_level(base_pq + step_code, EYEQ::WorkPrecision::F16);
    const double step32 = f32_hi - f32_lo;
    const double step16 = f16_hi - f16_lo;
    std::fprintf(stdout,
                 "      1-code PQ step at %.4f: 32F lo=%.6f hi=%.6f delta=%.6f | 16F lo=%.6f hi=%.6f delta=%.6f\n",
                 base_pq, f32_lo, f32_hi, step32, f16_lo, f16_hi, step16);
    Check(step32 > 0.0, "the 32F reference resolves a one-code input step");
    Check(step16 > 0.0, "the 16F work texture still resolves a one-code input step");
  }
}

} // namespace

int main() {
  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    if (!context.gpu()) {
      std::fprintf(stderr, "FATAL: no headless Vulkan device\n");
      return 1;
    }
    TestVividCountsAndResidue(context);
    TestVividKnownPoints(context);
    TestVividInertWithoutMetadata(context);
    TestVividChangesResultAndEquivalence(context);
    TestVividTargetMovesResult(context);
    TestDolbyVisionPath(context);
    TestVividAndDolbyShareOneTarget(context);
    TestVividSplineBranches(context);
    TestVividSaturation(context);
    TestPrecisionDynamicSources(context);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }
  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
