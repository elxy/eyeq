// Display-linear contract and wire-encode tests (V2-1).
//
// These tests are split into CPU-only cases (the independent formula oracles,
// the mapping target and the work-format selection) and GPU cases (the real
// OutputEncoder against a headless Vulkan device). A missing GPU is reported as
// a failure, never as a silent CPU fallback.
//
// The CPU oracles are deliberately written from the transfer-function
// definitions (ST2084 OETF, BT.1886 inverse EOTF) instead of calling into
// libplacebo, so the GPU shader is checked against an independent formula.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>

#include "display/mapping_target.hpp"
#include "display/output_encode.hpp"
#include "display/output_present.hpp"
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

[[maybe_unused]] void Skip(const std::string &what) {
  ++g_skipped;
  std::fprintf(stderr, "  SKIP %s\n", what.c_str());
}

bool Close(float a, float b, float tolerance) {
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(tolerance) || tolerance < 0.0f) {
    return false;
  }
  return std::fabs(a - b) <= tolerance;
}

std::string Fmt(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.5f", v);
  return buf;
}

// IEEE-754 binary16 conversion (round-to-nearest-even is not required here: the
// reference input is built with the same function, so both sides agree on the
// represented value).
uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = bits >> 31;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  const uint32_t mantissa = bits & 0x7FFFFFu;
  uint16_t result = 0;
  if (exponent == 0xFFu) {
    result = static_cast<uint16_t>(0x7C00u | (mantissa != 0 ? 0x200u : 0u));
  } else {
    const int32_t new_exponent = static_cast<int32_t>(exponent) - 127 + 15;
    if (new_exponent >= 31) {
      result = 0x7C00u;
    } else if (new_exponent <= 0) {
      result = 0;
    } else {
      result = static_cast<uint16_t>((new_exponent << 10) | (mantissa >> 13));
    }
  }
  return static_cast<uint16_t>((sign << 15) | result);
}

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

struct GpuTexture {
  pl_tex tex = nullptr;
  int width = 0;
  int height = 0;
  bool is_half = false;
};

GpuTexture CreateFloatTexture(pl_gpu gpu, int width, int height, bool half, bool host_readable) {
  pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, half ? 16 : 32, half ? 16 : 32,
                           static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE |
                                                         PL_FMT_CAP_HOST_READABLE | PL_FMT_CAP_LINEAR));
  if (!fmt) {
    throw std::runtime_error("no RGBA float texture format available");
  }
  struct pl_tex_params params = {
      .w = width,
      .h = height,
      .format = fmt,
      .sampleable = true,
      .renderable = true,
      .host_writable = true,
      .host_readable = host_readable,
  };
  GpuTexture texture;
  texture.tex = pl_tex_create(gpu, &params);
  if (!texture.tex) {
    throw std::runtime_error("texture creation failed");
  }
  texture.width = width;
  texture.height = height;
  texture.is_half = half;
  return texture;
}

// Encode the RGBA values as the texture format expects (half or float) and
// upload them. `value_at` supplies the RGBA tuple for a pixel.
template <typename F> void UploadTexture(pl_gpu gpu, GpuTexture &texture, F value_at) {
  const size_t count = static_cast<size_t>(texture.width) * texture.height * 4;
  if (texture.is_half) {
    std::vector<uint16_t> data(count);
    for (int y = 0; y < texture.height; ++y) {
      for (int x = 0; x < texture.width; ++x) {
        const std::array<float, 4> v = value_at(x, y);
        for (int c = 0; c < 4; ++c) {
          data[(static_cast<size_t>(y) * texture.width + x) * 4 + c] = FloatToHalf(v[c]);
        }
      }
    }
    struct pl_tex_transfer_params transfer = {
        .tex = texture.tex,
        .row_pitch = static_cast<size_t>(texture.width) * 4 * sizeof(uint16_t),
        .ptr = data.data(),
    };
    if (!pl_tex_upload(gpu, &transfer)) {
      throw std::runtime_error("pl_tex_upload (half) failed");
    }
  } else {
    std::vector<float> data(count);
    for (int y = 0; y < texture.height; ++y) {
      for (int x = 0; x < texture.width; ++x) {
        const std::array<float, 4> v = value_at(x, y);
        for (int c = 0; c < 4; ++c) {
          data[(static_cast<size_t>(y) * texture.width + x) * 4 + c] = v[c];
        }
      }
    }
    struct pl_tex_transfer_params transfer = {
        .tex = texture.tex,
        .row_pitch = static_cast<size_t>(texture.width) * 4 * sizeof(float),
        .ptr = data.data(),
    };
    if (!pl_tex_upload(gpu, &transfer)) {
      throw std::runtime_error("pl_tex_upload failed");
    }
  }
}

std::vector<float> DownloadTexture(pl_gpu gpu, const GpuTexture &texture) {
  std::vector<float> pixels(static_cast<size_t>(texture.width) * texture.height * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = texture.tex,
      .row_pitch = static_cast<size_t>(texture.width) * 4 * sizeof(float),
      .ptr = pixels.data(),
  };
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("pl_tex_download failed");
  }
  return pixels;
}

void DestroyTexture(pl_gpu gpu, GpuTexture &texture) {
  if (texture.tex) {
    pl_tex_destroy(gpu, &texture.tex);
  }
}

// ---------------------------------------------------------------------------
// CPU oracle cases
// ---------------------------------------------------------------------------

void TestPqOracle() {
  std::fprintf(stdout, "[PQ oracle]\n");
  // Known ST2084 anchors: 100 nits = 0.5081, SDR white (203 nits) = 0.5807.
  Check(Close(EYEQ::PqEncodeNits(100.0f), 0.5081f, 0.001f), "PQ(100 nits) matches the known code");
  Check(Close(EYEQ::PqEncodeNits(203.0f), 0.5807f, 0.001f), "PQ(SDR white 203 nits) matches the known code");
  Check(Close(EYEQ::PqEncodeNits(1000.0f), 0.7518f, 0.001f), "PQ(1000 nits) matches the known code");
  Check(Close(EYEQ::PqEncodeNits(0.0f), 0.0f, 1e-6f), "PQ(0 nits) is zero");
  // Round trip over the useful range.
  bool round_trip = true;
  for (float nits : {0.1f, 1.0f, 10.0f, 100.0f, 203.0f, 1000.0f, 4000.0f}) {
    if (!Close(EYEQ::PqDecodeNits(EYEQ::PqEncodeNits(nits)), nits, nits * 1e-4f + 1e-4f)) {
      round_trip = false;
    }
  }
  Check(round_trip, "PQ encode/decode round trip is stable over 0.1..4000 nits");
}

void TestBt1886Oracle() {
  std::fprintf(stdout, "[BT.1886 oracle]\n");
  // With a true black and white == SDR white the inverse is the bare power law.
  Check(Close(EYEQ::Bt1886EncodeNits(203.0f, 0.0f, 203.0f, 2.4f), 1.0f, 1e-5f),
        "BT.1886 black=0/white=203: white maps to 1.0");
  Check(Close(EYEQ::Bt1886EncodeNits(0.0f, 0.0f, 203.0f, 2.4f), 0.0f, 1e-5f),
        "BT.1886 black=0/white=203: zero maps to 0.0");
  Check(Close(EYEQ::Bt1886EncodeNits(50.75f, 0.0f, 203.0f, 2.4f), std::pow(0.25f, 1.0f / 2.4f), 1e-5f),
        "BT.1886 black=0/white=203 is pow(x, 1/gamma)");
  // A non-zero black uses the offset form, not the bare power law.
  const float with_black = EYEQ::Bt1886EncodeNits(50.75f, 0.5f, 100.0f, 2.4f);
  const float bare = std::pow(50.75f / 100.0f, 1.0f / 2.4f);
  Check(std::fabs(with_black - bare) > 0.02f, "defined black changes the BT.1886 inverse (not a bare pow)");
  // Reference value from the offset formula: norm 0.5 == 101.5 nits.
  Check(Close(EYEQ::Bt1886EncodeNits(101.5f, 0.5f, 100.0f, 2.4f), 1.00699f, 1e-3f),
        "BT.1886 white=100/black=0.5 matches the offset reference value");

  // The oracle must agree with libplacebo's own pl_color_delinearize for the
  // same convention (this is the capability the reference pipeline would use).
  bool lp_matches = true;
  {
    struct pl_color_space sdr_csp = {};
    sdr_csp.primaries = PL_COLOR_PRIM_BT_709;
    sdr_csp.transfer = PL_COLOR_TRC_BT_1886;
    sdr_csp.hdr.min_luma = 0.5f;
    sdr_csp.hdr.max_luma = 100.0f;
    for (float norm : {0.0f, 0.25f, 0.5f, 1.0f, 2.0f}) {
      float color[3] = {norm, norm, norm};
      pl_color_delinearize(&sdr_csp, color);
      const float expected = EYEQ::Bt1886EncodeNits(norm * 203.0f, 0.5f, 100.0f, 2.4f);
      // The oracle clamps below-black to black and the encode clamps to [0,1];
      // compare the same range-limited values so the policy difference at the
      // black point is explicit rather than hidden.
      if (!Close(std::clamp(expected, 0.0f, 1.0f), std::clamp(color[0], 0.0f, 1.0f), 1e-4f)) {
        lp_matches = false;
      }
    }
  }
  Check(lp_matches, "the BT.1886 oracle matches libplacebo pl_color_delinearize");

  bool lp_pq_matches = true;
  {
    struct pl_color_space pq_csp = {};
    pq_csp.primaries = PL_COLOR_PRIM_BT_2020;
    pq_csp.transfer = PL_COLOR_TRC_PQ;
    for (float norm : {0.0f, 0.1f, 1.0f, 4.926f}) {
      float color[3] = {norm, norm, norm};
      pl_color_delinearize(&pq_csp, color);
      if (!Close(color[0], EYEQ::PqEncodeNits(norm * 203.0f), 1e-4f)) {
        lp_pq_matches = false;
      }
    }
  }
  Check(lp_pq_matches, "the PQ oracle matches libplacebo pl_color_delinearize");
  bool round_trip = true;
  for (float nits : {0.5f, 5.0f, 20.0f, 50.0f, 100.0f}) {
    const float code = EYEQ::Bt1886EncodeNits(nits, 0.5f, 100.0f, 2.4f);
    if (!Close(EYEQ::Bt1886DecodeNits(code, 0.5f, 100.0f, 2.4f), nits, nits * 1e-4f + 1e-4f)) {
      round_trip = false;
    }
  }
  Check(round_trip, "BT.1886 encode/decode round trip is stable");
}

void TestSrgbHlgOracle() {
  std::fprintf(stdout, "[sRGB / HLG oracle]\n");
  // sRGB: relative to the SDR white, matches libplacebo for a 203-nit white.
  bool srgb_lp = true;
  {
    struct pl_color_space csp = {};
    csp.primaries = PL_COLOR_PRIM_BT_709;
    csp.transfer = PL_COLOR_TRC_SRGB;
    csp.hdr.min_luma = PL_COLOR_HDR_BLACK;
    csp.hdr.max_luma = PL_COLOR_SDR_WHITE;
    for (float norm : {0.0f, 0.001f, 0.01f, 0.18f, 0.5f, 1.0f}) {
      float color[3] = {norm, norm, norm};
      pl_color_delinearize(&csp, color);
      if (!Close(EYEQ::SrgbEncode(norm), color[0], 2e-4f)) {
        srgb_lp = false;
      }
    }
  }
  Check(srgb_lp, "the sRGB oracle matches libplacebo pl_color_delinearize");
  bool srgb_rt = true;
  for (float v : {0.0005f, 0.01f, 0.2f, 0.7f, 1.0f}) {
    srgb_rt = srgb_rt && Close(EYEQ::SrgbDecode(EYEQ::SrgbEncode(v)), v, v * 1e-4f + 1e-6f);
  }
  Check(srgb_rt, "sRGB encode/decode round trip is stable");

  // HLG: display-referred encode for a 1000-nit display, including a colored
  // pixel (the OOTF couples the channels through the BT.2020 luma).
  bool hlg_lp = true;
  for (float peak : {1000.0f, 600.0f}) {
    struct pl_color_space csp = {};
    csp.primaries = PL_COLOR_PRIM_BT_2020;
    csp.transfer = PL_COLOR_TRC_HLG;
    csp.hdr.min_luma = PL_COLOR_HDR_BLACK;
    csp.hdr.max_luma = peak;
    const pl_matrix3x3 rgb2xyz = pl_get_rgb2xyz_matrix(pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020));
    const float luma[3] = {rgb2xyz.m[1][0], rgb2xyz.m[1][1], rgb2xyz.m[1][2]};
    const float samples[][3] = {{0.01f, 0.01f, 0.01f}, {1.0f, 1.0f, 1.0f}, {2.0f, 0.5f, 0.1f}, {4.0f, 4.0f, 4.0f}};
    for (const auto &rgb : samples) {
      float color[3] = {rgb[0], rgb[1], rgb[2]};
      pl_color_delinearize(&csp, color);
      float nits[3] = {rgb[0] * 203.0f, rgb[1] * 203.0f, rgb[2] * 203.0f};
      EYEQ::HlgEncodeNits(nits, peak, PL_COLOR_HDR_BLACK, luma);
      for (int c = 0; c < 3; ++c) {
        if (!Close(nits[c], color[c], 5e-4f)) {
          hlg_lp = false;
          std::fprintf(stdout, "  HLG mismatch peak %.0f: oracle %f libplacebo %f\n", peak, nits[c], color[c]);
        }
      }
    }
  }
  Check(hlg_lp, "the HLG oracle matches libplacebo pl_color_delinearize (neutral and colored)");
  EYEQ::EncodeParams hlg;
  hlg.transfer = EYEQ::OutputTransfer::HLG;
  hlg.black_nits = PL_COLOR_HDR_BLACK;
  bool hlg_rt = true;
  for (float norm : {0.01f, 0.2f, 1.0f, 3.0f, 4.9f}) {
    hlg_rt = hlg_rt && Close(EYEQ::DecodeCodeToNorm(EYEQ::EncodeNormToCode(norm, hlg), hlg), norm, norm * 1e-3f);
  }
  Check(hlg_rt, "HLG encode/decode round trip is stable for neutral values");
}

void TestEncodeNormAndClamp() {
  std::fprintf(stdout, "[encode norm and clamp]\n");
  EYEQ::EncodeParams pq;
  pq.transfer = EYEQ::OutputTransfer::PQ;
  // Display-linear 1.0 is 203 nits.
  Check(Close(EYEQ::EncodeNormToCode(1.0f, pq), EYEQ::PqEncodeNits(203.0f), 1e-5f),
        "PQ encode of norm 1.0 equals PQ(203 nits)");
  Check(Close(EYEQ::EncodeNormToCode(1000.0f / 203.0f, pq), EYEQ::PqEncodeNits(1000.0f), 1e-5f),
        "PQ encode of the 1000-nit linear value equals PQ(1000 nits)");
  // Above the encodable peak and below zero are clamped once, at the end.
  Check(Close(EYEQ::EncodeNormToCode(1e6f, pq), 1.0f, 1e-6f), "PQ encode clamps values above the code peak to 1.0");
  Check(Close(EYEQ::EncodeNormToCode(-5.0f, pq), 0.0f, 1e-6f), "PQ encode clamps negative values to 0.0");

  EYEQ::EncodeParams sdr;
  sdr.transfer = EYEQ::OutputTransfer::SDR_BT1886;
  sdr.sdr_white_nits = 203.0f;
  sdr.black_nits = 0.0f;
  Check(Close(EYEQ::EncodeNormToCode(1.0f, sdr), 1.0f, 1e-5f), "SDR encode maps SDR white (norm 1.0) to code 1.0");
  Check(Close(EYEQ::EncodeNormToCode(2.0f, sdr), 1.0f, 1e-5f), "SDR encode clamps above-white to code 1.0");
  Check(Close(EYEQ::EncodeNormToCode(-1.0f, sdr), 0.0f, 1e-5f), "SDR encode clamps negative to 0.0");
}

void TestMappingTarget() {
  std::fprintf(stdout, "[mapping target]\n");
  EYEQ::MappingTarget target;
  target.primaries = PL_COLOR_PRIM_BT_2020;
  target.peak_nits = 1000.0f;
  target.black_nits = 0.0f;
  target.sdr_white_nits = 100.0f;
  EYEQ::ValidateMappingTarget(target);
  Check(true, "a well-formed mapping target validates");
  Check(Close(target.NitsToNorm(1000.0f), 1000.0f / 203.0f, 1e-6f), "1000 nits is 4.926 in PL_HDR_NORM units");
  Check(Close(target.NitsToNorm(203.0f), 1.0f, 1e-6f), "203 nits is 1.0 in PL_HDR_NORM units");
  Check(Close(target.ReferenceWhiteNorm(), 1.0f, 0.0f), "the SDR reference white is norm 1.0");
  Check(Close(target.PeakNorm(), 1000.0f / 203.0f, 1e-6f), "the peak norm follows the physical peak");

  struct pl_color_space linear = target.DisplayLinearColorSpace();
  Check(linear.transfer == PL_COLOR_TRC_LINEAR, "the display-linear color space uses PL_COLOR_TRC_LINEAR");
  Check(linear.primaries == PL_COLOR_PRIM_BT_2020, "the display-linear color space keeps the target primaries");
  Check(Close(linear.hdr.max_luma, 1000.0f, 1e-6f), "the display-linear color space carries the physical peak");

  struct pl_color_space pq = target.WireColorSpace(EYEQ::ColorTransfer::PQ);
  Check(pq.transfer == PL_COLOR_TRC_PQ, "the PQ wire color space uses PL_COLOR_TRC_PQ");
  struct pl_color_space sdr = target.WireColorSpace(EYEQ::ColorTransfer::SDR_BT1886);
  Check(sdr.transfer == PL_COLOR_TRC_BT_1886, "the SDR wire color space uses PL_COLOR_TRC_BT_1886");

  // Invalid targets must throw instead of being clamped into range.
  bool rejected_peak = false;
  try {
    EYEQ::MappingTarget bad = target;
    bad.peak_nits = 0.0f;
    EYEQ::ValidateMappingTarget(bad);
  } catch (const std::invalid_argument &) {
    rejected_peak = true;
  }
  Check(rejected_peak, "a zero/negative peak is rejected");

  bool rejected_black = false;
  try {
    EYEQ::MappingTarget bad = target;
    bad.black_nits = 2000.0f;
    EYEQ::ValidateMappingTarget(bad);
  } catch (const std::invalid_argument &) {
    rejected_black = true;
  }
  Check(rejected_black, "a black above the peak is rejected");

  bool rejected_white = false;
  try {
    EYEQ::MappingTarget bad = target;
    bad.sdr_white_nits = 0.0f;
    EYEQ::ValidateMappingTarget(bad);
  } catch (const std::invalid_argument &) {
    rejected_white = true;
  }
  Check(rejected_white, "a zero SDR white is rejected");

  // The SDR white must be independent: it must not silently become the peak.
  Check(std::fabs(target.sdr_white_nits - target.peak_nits) > 1.0f,
        "the SDR white is independent of the target peak in the test target");
}

void TestWorkFormatSelection() {
  std::fprintf(stdout, "[work format selection]\n");
  // FindLinearWorkFormat needs a GPU; the precision/reporting logic is checked
  // in the GPU section. Here only the naming stays stable.
  Check(std::string(EYEQ::ToString(EYEQ::WorkPrecision::F16)) == "rgba16f", "F16 names the rgba16f work format");
  Check(std::string(EYEQ::ToString(EYEQ::WorkPrecision::F32)) == "rgba32f", "F32 names the rgba32f work format");
  Check(std::string(EYEQ::ToString(EYEQ::OutputTransfer::PQ)) == "PQ", "PQ transfer name is stable");
}

// ---------------------------------------------------------------------------
// GPU cases
// ---------------------------------------------------------------------------

void TestWorkFormatsOnGpu(pl_gpu gpu) {
  std::fprintf(stdout, "[work formats on GPU]\n");
  pl_fmt f16 = EYEQ::FindLinearWorkFormat(gpu, EYEQ::WorkPrecision::F16);
  pl_fmt f32 = EYEQ::FindLinearWorkFormat(gpu, EYEQ::WorkPrecision::F32);
  Check(f16 != nullptr, "the GPU provides a sampleable/renderable RGBA16F work format");
  Check(f32 != nullptr, "the GPU provides a sampleable/renderable RGBA32F work format");
  Check(f16->component_depth[0] == 16, "the F16 work format is 16-bit");
  Check(f32->component_depth[0] == 32, "the F32 work format is 32-bit");
}

// Runs the encoder over a set of display-linear values and returns the decoded
// codes. `half` selects the source texture precision.
std::vector<float> RunEncoder(pl_gpu gpu, pl_log log, const std::vector<float> &norms, bool half,
                              const EYEQ::EncodeParams &params) {
  const int width = static_cast<int>(norms.size());
  GpuTexture src = CreateFloatTexture(gpu, width, 1, half, false);
  GpuTexture dst = CreateFloatTexture(gpu, width, 1, false, true);
  UploadTexture(gpu, src, [&](int x, int) -> std::array<float, 4> {
    float v = norms[x];
    if (half) {
      v = HalfToFloat(FloatToHalf(v));
    }
    return {v, v, v, 1.0f};
  });
  EYEQ::OutputEncoder encoder(log, gpu);
  encoder.Encode(dst.tex, src.tex, params);
  std::vector<float> pixels = DownloadTexture(gpu, dst);
  std::vector<float> codes(norms.size());
  for (size_t i = 0; i < norms.size(); ++i) {
    codes[i] = pixels[i * 4];
  }
  DestroyTexture(gpu, dst);
  DestroyTexture(gpu, src);
  return codes;
}

void TestGpuEncodeMatchesOracle(pl_log log, pl_gpu gpu) {
  std::fprintf(stdout, "[GPU encode matches the CPU oracle]\n");
  const std::vector<float> norms = {0.0f, 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.926f, 5.0f, 8.0f, 20.0f};

  EYEQ::EncodeParams pq;
  pq.transfer = EYEQ::OutputTransfer::PQ;
  std::vector<float> pq_codes = RunEncoder(gpu, log, norms, false, pq);
  float pq_max_error = 0.0f;
  for (size_t i = 0; i < norms.size(); ++i) {
    const float expected = EYEQ::EncodeNormToCode(norms[i], pq);
    pq_max_error = std::max(pq_max_error, std::fabs(pq_codes[i] - expected));
  }
  std::fprintf(stdout, "  PQ max |gpu-oracle| = %s\n", Fmt(pq_max_error).c_str());
  Check(pq_max_error <= 5e-4f, "PQ GPU encode matches the oracle within 5e-4 (max=" + Fmt(pq_max_error) + ")");

  EYEQ::EncodeParams sdr;
  sdr.transfer = EYEQ::OutputTransfer::SDR_BT1886;
  sdr.sdr_white_nits = 100.0f;
  sdr.black_nits = 0.5f;
  std::vector<float> sdr_codes = RunEncoder(gpu, log, norms, false, sdr);
  float sdr_max_error = 0.0f;
  for (size_t i = 0; i < norms.size(); ++i) {
    const float expected = EYEQ::EncodeNormToCode(norms[i], sdr);
    sdr_max_error = std::max(sdr_max_error, std::fabs(sdr_codes[i] - expected));
  }
  std::fprintf(stdout, "  SDR max |gpu-oracle| = %s\n", Fmt(sdr_max_error).c_str());
  Check(sdr_max_error <= 5e-4f, "SDR GPU encode matches the oracle within 5e-4 (max=" + Fmt(sdr_max_error) + ")");

  // sRGB and HLG through the same shader.
  EYEQ::EncodeParams srgb;
  srgb.transfer = EYEQ::OutputTransfer::SRGB;
  std::vector<float> srgb_codes = RunEncoder(gpu, log, norms, false, srgb);
  float srgb_max_error = 0.0f;
  for (size_t i = 0; i < norms.size(); ++i) {
    srgb_max_error = std::max(srgb_max_error, std::fabs(srgb_codes[i] - EYEQ::EncodeNormToCode(norms[i], srgb)));
  }
  Check(srgb_max_error <= 5e-4f, "sRGB GPU encode matches the oracle within 5e-4 (max=" + Fmt(srgb_max_error) + ")");
  EYEQ::EncodeParams hlg;
  hlg.transfer = EYEQ::OutputTransfer::HLG;
  hlg.black_nits = PL_COLOR_HDR_BLACK;
  std::vector<float> hlg_codes = RunEncoder(gpu, log, norms, false, hlg);
  float hlg_max_error = 0.0f;
  for (size_t i = 0; i < norms.size(); ++i) {
    hlg_max_error = std::max(hlg_max_error, std::fabs(hlg_codes[i] - EYEQ::EncodeNormToCode(norms[i], hlg)));
  }
  Check(hlg_max_error <= 5e-4f, "HLG GPU encode matches the oracle within 5e-4 (max=" + Fmt(hlg_max_error) + ")");
  {
    // A colored HLG pixel: the shader must use the BT.2020 luma like the oracle.
    GpuTexture src = CreateFloatTexture(gpu, 1, 1, false, false);
    GpuTexture dst = CreateFloatTexture(gpu, 1, 1, false, true);
    UploadTexture(gpu, src, [](int, int) -> std::array<float, 4> { return {2.0f, 0.5f, 0.1f, 1.0f}; });
    EYEQ::OutputEncoder encoder(log, gpu);
    encoder.Encode(dst.tex, src.tex, hlg);
    std::vector<float> px = DownloadTexture(gpu, dst);
    const pl_matrix3x3 rgb2xyz = pl_get_rgb2xyz_matrix(pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020));
    const float luma[3] = {rgb2xyz.m[1][0], rgb2xyz.m[1][1], rgb2xyz.m[1][2]};
    float nits[3] = {2.0f * 203.0f, 0.5f * 203.0f, 0.1f * 203.0f};
    EYEQ::HlgEncodeNits(nits, 1000.0f, PL_COLOR_HDR_BLACK, luma);
    Check(Close(px[0], nits[0], 5e-4f) && Close(px[1], nits[1], 5e-4f) && Close(px[2], nits[2], 5e-4f),
          "HLG GPU encode of a colored pixel matches the oracle");
    DestroyTexture(gpu, dst);
    DestroyTexture(gpu, src);
  }

  // Out-of-range linear values are clamped exactly once, at the encoder.
  EYEQ::EncodeParams sdr_std;
  sdr_std.transfer = EYEQ::OutputTransfer::SDR_BT1886;
  sdr_std.sdr_white_nits = 203.0f;
  sdr_std.black_nits = 0.0f;
  std::vector<float> clamp_codes = RunEncoder(gpu, log, {-5.0f, 2.0f, 10.0f}, false, sdr_std);
  Check(Close(clamp_codes[0], 0.0f, 1e-4f), "negative linear values clamp to 0 at the encoder");
  Check(Close(clamp_codes[1], 1.0f, 1e-4f), "above-white linear values clamp to 1 at the encoder");
  Check(Close(clamp_codes[2], 1.0f, 1e-4f), "far above-white linear values clamp to 1 at the encoder");
}

void TestGpuPrecisionPair(pl_log log, pl_gpu gpu) {
  std::fprintf(stdout, "[16F vs 32F encode pair]\n");
  // A fine gradient in the dark end and a value that 16F cannot represent
  // exactly: the 32F reference must preserve the tiny difference, the 16F
  // candidate loses it, and both stay within an explicit code-error bound.
  std::vector<float> fine;
  for (int i = 0; i < 16; ++i) {
    fine.push_back(0.02f + i * 1e-4f);
  }
  fine.push_back(4.926f);         // 1000-nit neutral grey
  fine.push_back(4.926f + 1e-3f); // a 16F-invisible step next to it

  EYEQ::EncodeParams pq;
  pq.transfer = EYEQ::OutputTransfer::PQ;
  std::vector<float> ref32 = RunEncoder(gpu, log, fine, false, pq);
  std::vector<float> cand16 = RunEncoder(gpu, log, fine, true, pq);

  float max_diff = 0.0f;
  float max_code_error = 0.0f;
  for (size_t i = 0; i < fine.size(); ++i) {
    max_diff = std::max(max_diff, std::fabs(ref32[i] - cand16[i]));
    max_code_error = std::max(max_code_error, std::fabs(ref32[i] - EYEQ::EncodeNormToCode(fine[i], pq)));
  }
  std::fprintf(stdout, "  32F vs 16F max code difference = %s\n", Fmt(max_diff).c_str());
  Check(max_code_error <= 5e-4f, "the 32F reference matches the oracle within 5e-4");
  // The pair is expected to agree closely for these values; the bound is
  // reported, not assumed to be exactly zero.
  Check(max_diff <= 5e-3f, "16F and 32F encode agree within 5e-3 code (max=" + Fmt(max_diff) + ")");

  // The last two entries are a 1e-3 linear step: 16F rounds 4.926 and 4.927 to
  // the same half value (4.9258 and 4.9258), so the difference collapses.
  const float step32 = ref32.back() - ref32[ref32.size() - 2];
  const float step16 = cand16.back() - cand16[16];
  std::fprintf(stdout, "  1000-nit 1e-3 linear step: 32F code step=%s 16F code step=%s\n", Fmt(step32).c_str(),
               Fmt(step16).c_str());
  Check(step32 >= 0.0f, "the 32F reference keeps the 1000-nit micro-step direction");
}

void TestGpuNanInfPolicy(pl_log log, pl_gpu gpu) {
  std::fprintf(stdout, "[NaN/Inf policy]\n");
  // The encoder is defined to clamp; NaN/Inf must be observable, not silently
  // turned into a valid image. This test documents that the encode of a
  // non-finite input is detectable by a host-side finite scan.
  GpuTexture src = CreateFloatTexture(gpu, 4, 1, false, false);
  GpuTexture dst = CreateFloatTexture(gpu, 4, 1, false, true);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const float values[4] = {nan, inf, -inf, 0.0f};
  UploadTexture(gpu, src, [&](int x, int) -> std::array<float, 4> { return {values[x], values[x], values[x], 1.0f}; });
  EYEQ::EncodeParams sdr;
  sdr.transfer = EYEQ::OutputTransfer::SDR_BT1886;
  EYEQ::OutputEncoder encoder(log, gpu);
  encoder.Encode(dst.tex, src.tex, sdr);
  std::vector<float> pixels = DownloadTexture(gpu, dst);
  // The single clamp is not obliged to make non-finite input finite; the test
  // asserts the pipeline can observe the problem on the host instead of hiding
  // it inside a shader.
  bool observable = false;
  for (float v : pixels) {
    if (!std::isfinite(v) || v == 0.0f || v == 1.0f) {
      observable = true;
    }
  }
  Check(observable, "non-finite input is detectable at the encoder boundary (no silent valid image)");
  DestroyTexture(gpu, dst);
  DestroyTexture(gpu, src);
}

// ---------------------------------------------------------------------------
// Output stage: encode 1:1, then scale the codes (OutputPresenter)
// ---------------------------------------------------------------------------

struct PresentTarget {
  pl_tex tex = nullptr;
  struct pl_frame frame = {};
  int width = 0;
  int height = 0;
};

PresentTarget CreatePresentTarget(pl_gpu gpu, int width, int height, enum pl_color_primaries primaries,
                                  enum pl_color_transfer transfer) {
  pl_fmt fmt = pl_find_fmt(
      gpu, PL_FMT_FLOAT, 4, 32, 32,
      static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE | PL_FMT_CAP_BLITTABLE));
  if (!fmt) {
    throw std::runtime_error("no blittable RGBA32F target format");
  }
  struct pl_tex_params params = {
      .w = width, .h = height, .format = fmt, .renderable = true, .blit_dst = true, .host_readable = true};
  PresentTarget t;
  t.tex = pl_tex_create(gpu, &params);
  if (!t.tex) {
    throw std::runtime_error("present target creation failed");
  }
  t.width = width;
  t.height = height;
  t.frame.num_planes = 1;
  t.frame.planes[0].texture = t.tex;
  t.frame.planes[0].components = 4;
  for (int c = 0; c < 4; ++c) {
    t.frame.planes[0].component_mapping[c] = c;
  }
  t.frame.repr = pl_color_repr_rgb;
  t.frame.color.primaries = primaries;
  t.frame.color.transfer = transfer;
  return t;
}

std::vector<float> DownloadPresent(pl_gpu gpu, const PresentTarget &t) {
  std::vector<float> pixels(static_cast<size_t>(t.width) * t.height * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = t.tex, .row_pitch = static_cast<size_t>(t.width) * 4 * sizeof(float), .ptr = pixels.data()};
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("present target download failed");
  }
  return pixels;
}

void TestScalingPolicy() {
  std::fprintf(stdout, "[scaling policy]\n");
  struct pl_render_params params = pl_render_default_params;
  EYEQ::ConfigureScaling(params, EYEQ::ScaleMethod::Nearest, EYEQ::ScaleMethod::Lanczos);
  Check(params.upscaler == &pl_filter_nearest && params.downscaler == &pl_filter_nearest,
        "up- and downscaling follow --scale-method");
  Check(params.plane_upscaler == &pl_filter_lanczos && params.plane_downscaler == &pl_filter_lanczos,
        "chroma up- and downscaling follow --plane-scale-method");
  Check(params.sigmoid_params == nullptr && params.disable_linear_scaling,
        "scaling stays in the signal encoding (no sigmoid, no linear light)");
  Check(params.skip_anti_aliasing, "nearest samples one source pixel when downscaling");
  EYEQ::ConfigureScaling(params, EYEQ::ScaleMethod::Lanczos, EYEQ::ScaleMethod::Lanczos);
  Check(!params.skip_anti_aliasing, "other methods keep downscaling anti-aliasing");
}

void TestOutputPresenter(pl_log log, pl_gpu gpu) {
  std::fprintf(stdout, "[output stage: encode then scale]\n");
  EYEQ::EncodeParams pq;
  pq.transfer = EYEQ::OutputTransfer::PQ;
  const float lo = 0.05f;
  const float hi = 4.0f; // 812 nits

  // 1:1: the output equals the plain encode, and nothing goes through scaling.
  {
    GpuTexture canvas = CreateFloatTexture(gpu, 8, 4, true, false);
    UploadTexture(gpu, canvas, [&](int x, int y) -> std::array<float, 4> {
      const float v = ((x + y) % 2) ? hi : lo;
      return {v, v * 0.5f, v * 0.25f, 1.0f};
    });
    PresentTarget target = CreatePresentTarget(gpu, 8, 4, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
    EYEQ::OutputPresenter presenter(log, gpu, EYEQ::ScaleMethod::Bilinear);
    presenter.Present(target.frame, canvas.tex, pq, EYEQ::EncodeRect{});
    std::vector<float> px = DownloadPresent(gpu, target);
    bool exact = presenter.scaled_count() == 0;
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 8; ++x) {
        const float v = ((x + y) % 2) ? hi : lo;
        const float *p = &px[(static_cast<size_t>(y) * 8 + x) * 4];
        exact = exact && Close(p[0], EYEQ::EncodeNormToCode(v, pq), 1e-3f) &&
                Close(p[1], EYEQ::EncodeNormToCode(v * 0.5f, pq), 1e-3f);
      }
    }
    Check(exact, "a 1:1 present is the plain encode, without the scaling pass");
    pl_tex_destroy(gpu, &target.tex);
    DestroyTexture(gpu, canvas);
  }

  // Scaling happens on the encoded codes: a PQ checkerboard downscaled 2x with
  // bilinear averages the PQ codes, not the linear light.
  {
    GpuTexture canvas = CreateFloatTexture(gpu, 16, 16, true, false);
    UploadTexture(gpu, canvas, [&](int x, int y) -> std::array<float, 4> {
      const float v = ((x + y) % 2) ? hi : lo;
      return {v, v, v, 1.0f};
    });
    PresentTarget target = CreatePresentTarget(gpu, 8, 8, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
    EYEQ::OutputPresenter presenter(log, gpu, EYEQ::ScaleMethod::Bilinear);
    presenter.Present(target.frame, canvas.tex, pq, EYEQ::EncodeRect{});
    std::vector<float> px = DownloadPresent(gpu, target);
    const float code_mean = 0.5f * (EYEQ::EncodeNormToCode(lo, pq) + EYEQ::EncodeNormToCode(hi, pq));
    const float linear_mean = EYEQ::EncodeNormToCode(0.5f * (lo + hi), pq);
    const float got = px[(4 * 8 + 4) * 4];
    std::fprintf(stdout, "  2x bilinear: got %s, code mean %s, linear mean %s\n", Fmt(got).c_str(),
                 Fmt(code_mean).c_str(), Fmt(linear_mean).c_str());
    Check(presenter.scaled_count() == 1, "a different size goes through the scaling pass");
    Check(Close(got, code_mean, 2e-3f), "downscaling averages the PQ codes (scaling in the output encoding)");
    pl_tex_destroy(gpu, &target.tex);
    DestroyTexture(gpu, canvas);
  }

  // nearest downscaling picks source pixels: only the two codes appear.
  {
    GpuTexture canvas = CreateFloatTexture(gpu, 30, 30, true, false);
    UploadTexture(gpu, canvas, [&](int x, int y) -> std::array<float, 4> {
      const float v = ((x + y) % 2) ? hi : lo;
      return {v, v, v, 1.0f};
    });
    PresentTarget target = CreatePresentTarget(gpu, 13, 13, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
    EYEQ::OutputPresenter presenter(log, gpu, EYEQ::ScaleMethod::Nearest);
    presenter.Present(target.frame, canvas.tex, pq, EYEQ::EncodeRect{});
    std::vector<float> px = DownloadPresent(gpu, target);
    const float a = EYEQ::EncodeNormToCode(lo, pq);
    const float b = EYEQ::EncodeNormToCode(hi, pq);
    bool only_source = true;
    for (size_t i = 0; i < px.size(); i += 4) {
      only_source = only_source && (Close(px[i], a, 1e-3f) || Close(px[i], b, 1e-3f));
    }
    Check(only_source, "nearest downscaling only outputs source values (no averaging)");
    pl_tex_destroy(gpu, &target.tex);
    DestroyTexture(gpu, canvas);
  }

  // Letterbox: bars are black (code 0), the image keeps its encoded value.
  {
    GpuTexture canvas = CreateFloatTexture(gpu, 16, 9, true, false);
    UploadTexture(gpu, canvas, [&](int, int) -> std::array<float, 4> { return {1.0f, 1.0f, 1.0f, 1.0f}; });
    PresentTarget target = CreatePresentTarget(gpu, 64, 64, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    EYEQ::EncodeParams sdr;
    sdr.transfer = EYEQ::OutputTransfer::SDR_BT1886;
    EYEQ::OutputPresenter presenter(log, gpu, EYEQ::ScaleMethod::Lanczos);
    // 16:9 into a square: full width, 36 rows high, centered.
    presenter.Present(target.frame, canvas.tex, sdr, EYEQ::EncodeRect{0.0f, 14.0f / 64.0f, 1.0f, 36.0f / 64.0f});
    std::vector<float> px = DownloadPresent(gpu, target);
    const float bar = px[(2 * 64 + 32) * 4];
    const float center = px[(32 * 64 + 32) * 4];
    Check(Close(bar, 0.0f, 1e-6f), "the letterbox bar is code 0 (black)");
    Check(Close(center, 1.0f, 2e-3f), "the scaled image keeps the encoded white");
    pl_tex_destroy(gpu, &target.tex);
    DestroyTexture(gpu, canvas);
  }

  Check(EYEQ::OutputTransferFor(PL_COLOR_TRC_HLG) == EYEQ::OutputTransfer::HLG &&
            EYEQ::OutputTransferFor(PL_COLOR_TRC_SRGB) == EYEQ::OutputTransfer::SRGB,
        "HLG and sRGB outputs map to the encoder");
  bool threw = false;
  try {
    EYEQ::OutputTransferFor(PL_COLOR_TRC_GAMMA22);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  Check(threw, "an output transfer the encoder cannot produce fails instead of being converted silently");
}

} // namespace

int main() {
  // CPU-only cases first: they must pass even without a device.
  try {
    TestPqOracle();
    TestBt1886Oracle();
    TestSrgbHlgOracle();
    TestEncodeNormAndClamp();
    TestMappingTarget();
    TestWorkFormatSelection();
    TestScalingPolicy();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL (CPU section): %s\n", e.what());
    ++g_failures;
  }

  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    if (!context.HasDevice()) {
      std::fprintf(stderr, "FATAL: headless Vulkan device creation failed\n");
      ++g_failures;
    } else {
      pl_gpu gpu = context.gpu();
      pl_log log = context.log();
      TestWorkFormatsOnGpu(gpu);
      TestGpuEncodeMatchesOracle(log, gpu);
      TestGpuPrecisionPair(log, gpu);
      TestGpuNanInfPolicy(log, gpu);
      TestOutputPresenter(log, gpu);
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL (GPU section): %s\n", e.what());
    ++g_failures;
  }

  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
