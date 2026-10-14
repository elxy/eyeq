#include "display/output_encode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <libplacebo/shaders/custom.h>
#include <spdlog/fmt/fmt.h>

#include "log.hpp"

namespace EYEQ {

// ST2084 constants shared by the CPU oracle and the GPU shader body below.
static constexpr float kPqM1 = 2610.0f / 16384.0f;
static constexpr float kPqM2 = 2523.0f / 32.0f;
static constexpr float kPqC1 = 3424.0f / 4096.0f;
static constexpr float kPqC2 = 2413.0f / 128.0f;
static constexpr float kPqC3 = 2392.0f / 128.0f;
static constexpr float kPqPeakNits = 10000.0f;
static constexpr float kSdrWhiteNits = 203.0f;
// ARIB STD-B67 / BT.2100 HLG constants and libplacebo's reference display peak.
static constexpr float kHlgA = 0.17883277f;
static constexpr float kHlgB = 0.28466892f;
static constexpr float kHlgC = 0.55991073f;
static constexpr float kHlgRefNits = 1000.0f;

namespace {

// IEEE-754 binary16 -> float, used by the finite scan so both the F16 and the F32
// work textures can be validated without a libplacebo format conversion.
float HalfBitsToFloat(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half >> 15) & 0x1u;
  const uint32_t exponent = static_cast<uint32_t>(half >> 10) & 0x1fu;
  const uint32_t mantissa = static_cast<uint32_t>(half) & 0x3ffu;
  float value;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa != 0 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
  }
  return sign != 0 ? -value : value;
}

} // namespace

void ValidateLinearTextureFinite(pl_gpu gpu, pl_tex tex) {
  if (!tex || !tex->params.host_readable) {
    throw std::invalid_argument("ValidateLinearTextureFinite: texture is null or not host-readable");
  }
  const std::size_t w = tex->params.w;
  const std::size_t h = tex->params.h;
  const int bits = tex->params.format->component_depth[0];
  if (bits != 16 && bits != 32) {
    throw std::invalid_argument("ValidateLinearTextureFinite: unsupported component depth");
  }
  const std::size_t stride = static_cast<std::size_t>(bits / 8) * 4;
  std::vector<uint8_t> raw(w * h * stride);
  struct pl_tex_transfer_params transfer = {
      .tex = tex,
      .row_pitch = w * stride,
      .ptr = raw.data(),
  };
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("ValidateLinearTextureFinite: failed to download the texture");
  }
  const std::size_t samples = w * h * 4;
  for (std::size_t i = 0; i < samples; ++i) {
    const float value = (bits == 16) ? HalfBitsToFloat(reinterpret_cast<const uint16_t *>(raw.data())[i])
                                     : reinterpret_cast<const float *>(raw.data())[i];
    if (!std::isfinite(value)) {
      throw std::runtime_error("ValidateLinearTextureFinite: display-linear sample " + std::to_string(i) +
                               " is not finite (NaN/Inf); refusing to present or encode it");
    }
  }
}

const char *ToString(OutputTransfer transfer) {
  switch (transfer) {
  case OutputTransfer::PQ:
    return "PQ";
  case OutputTransfer::SDR_BT1886:
    return "SDR-BT1886";
  case OutputTransfer::SRGB:
    return "sRGB";
  case OutputTransfer::HLG:
    return "HLG";
  }
  return "unknown";
}

float PqEncodeNits(float nits) {
  const float y = std::max(nits, 0.0f) / kPqPeakNits;
  const float ym = std::pow(y, kPqM1);
  return std::pow((kPqC1 + kPqC2 * ym) / (1.0f + kPqC3 * ym), kPqM2);
}

float PqDecodeNits(float code) {
  const float c = std::max(code, 0.0f);
  const float cm = std::pow(c, 1.0f / kPqM2);
  const float num = std::max(cm - kPqC1, 0.0f);
  const float den = kPqC2 - kPqC3 * cm;
  if (den <= 0.0f) {
    return 0.0f;
  }
  return std::pow(num / den, 1.0f / kPqM1) * kPqPeakNits;
}

float Bt1886EncodeNits(float nits, float black_nits, float white_nits, float gamma) {
  const float black = std::max(black_nits, 0.0f);
  const float b = std::pow(black / white_nits, 1.0f / gamma);
  const float luminance = std::max(nits, black);
  return (std::pow(luminance / white_nits, 1.0f / gamma) - b) / (1.0f - b);
}

float Bt1886DecodeNits(float code, float black_nits, float white_nits, float gamma) {
  const float black = std::max(black_nits, 0.0f);
  const float b = std::pow(black / white_nits, 1.0f / gamma);
  const float v = b + std::max(code, 0.0f) * (1.0f - b);
  return std::pow(v, gamma) * white_nits;
}

float SrgbEncode(float relative) {
  const float x = std::max(relative, 0.0f);
  return x >= 0.0031308f ? 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f : 12.92f * x;
}

float SrgbDecode(float code) {
  const float x = std::max(code, 0.0f);
  return x > 0.04045f ? std::pow((x + 0.055f) / 1.055f, 2.4f) : x / 12.92f;
}

namespace {

// HLG system gamma and black lift for a display peak/black (libplacebo).
void HlgDisplayParams(float peak_nits, float black_nits, float *gamma, float *lift) {
  *gamma = 1.2f * std::pow(1.111f, std::log2(peak_nits / kHlgRefNits));
  *lift = std::sqrt(3.0f * std::pow(std::max(black_nits, 0.0f) / peak_nits, 1.0f / *gamma));
}

} // namespace

void HlgEncodeNits(float rgb[3], float peak_nits, float black_nits, const float luma[3]) {
  float gamma;
  float lift;
  HlgDisplayParams(peak_nits, black_nits, &gamma, &lift);
  for (int i = 0; i < 3; ++i) {
    rgb[i] = std::max(rgb[i], 0.0f);
  }
  // Inverse OOTF: display light relative to the peak -> scene light in [0, 12].
  const float y = luma[0] * rgb[0] + luma[1] * rgb[1] + luma[2] * rgb[2];
  // Clamped before the power (as libplacebo's shader), so black stays finite.
  const float scale = std::pow(std::max(1e-6f, y / peak_nits), (1.0f - gamma) / gamma);
  for (int i = 0; i < 3; ++i) {
    const float e = 12.0f / peak_nits * scale * rgb[i];
    const float code = e > 1.0f ? kHlgA * std::log(e - kHlgB) + kHlgC : 0.5f * std::sqrt(e);
    rgb[i] = (code - lift) / (1.0f - lift);
  }
}

static const float kNeutralLuma[3] = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};

float EncodeNormToCode(float norm, const EncodeParams &params) {
  if (!std::isfinite(norm)) {
    throw std::invalid_argument("EncodeNormToCode: non-finite display-linear value (NaN/Inf is a hard error)");
  }
  const float nits = norm * kSdrWhiteNits;
  float code;
  switch (params.transfer) {
  case OutputTransfer::PQ:
    code = PqEncodeNits(nits);
    break;
  case OutputTransfer::SDR_BT1886:
    code = Bt1886EncodeNits(nits, params.black_nits, params.sdr_white_nits, params.gamma);
    break;
  case OutputTransfer::SRGB:
    code = SrgbEncode(nits / params.sdr_white_nits);
    break;
  case OutputTransfer::HLG: {
    // A neutral value: the luma weights sum to 1, so any weights give the same Y.
    float rgb[3] = {nits, nits, nits};
    HlgEncodeNits(rgb, params.peak_nits, params.black_nits, kNeutralLuma);
    code = rgb[0];
    break;
  }
  default:
    throw std::invalid_argument("unknown OutputTransfer");
  }
  return std::clamp(code, 0.0f, 1.0f);
}

float DecodeCodeToNorm(float code, const EncodeParams &params) {
  if (!std::isfinite(code)) {
    throw std::invalid_argument("DecodeCodeToNorm: non-finite code value (NaN/Inf is a hard error)");
  }
  float nits;
  switch (params.transfer) {
  case OutputTransfer::PQ:
    nits = PqDecodeNits(code);
    break;
  case OutputTransfer::SDR_BT1886:
    nits = Bt1886DecodeNits(code, params.black_nits, params.sdr_white_nits, params.gamma);
    break;
  case OutputTransfer::SRGB:
    nits = SrgbDecode(code) * params.sdr_white_nits;
    break;
  case OutputTransfer::HLG: {
    float gamma;
    float lift;
    HlgDisplayParams(params.peak_nits, params.black_nits, &gamma, &lift);
    const float x = (1.0f - lift) * std::max(code, 0.0f) + lift;
    const float e = (x > 0.5f ? std::exp((x - kHlgC) / kHlgA) + kHlgB : 4.0f * x * x) / 12.0f;
    nits = params.peak_nits * std::pow(e, gamma); // neutral: Y == e
    break;
  }
  default:
    throw std::invalid_argument("unknown OutputTransfer");
  }
  return nits / kSdrWhiteNits;
}

OutputEncoder::OutputEncoder(pl_log log, pl_gpu gpu) : gpu_(gpu) {
  dp_ = pl_dispatch_create(log, gpu_);
  if (!dp_) {
    throw std::runtime_error("OutputEncoder: pl_dispatch_create failed");
  }
}

OutputEncoder::~OutputEncoder() {
  if (dp_) {
    pl_dispatch_destroy(&dp_);
  }
}

void OutputEncoder::Encode(pl_tex dst, const pl_tex linear, const EncodeParams &params, const EncodeRect &rect,
                           pl_timer timer) {
  if (!dst || !linear) {
    throw std::invalid_argument("OutputEncoder::Encode: null texture");
  }
  if (!(linear->params.sampleable)) {
    throw std::invalid_argument("OutputEncoder::Encode: source texture is not sampleable");
  }
  if (!(dst->params.renderable)) {
    throw std::invalid_argument("OutputEncoder::Encode: destination texture is not renderable");
  }
  if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.w) || !std::isfinite(rect.h) ||
      rect.w <= 0.0f || rect.h <= 0.0f) {
    throw std::invalid_argument("OutputEncoder::Encode: invalid destination rectangle");
  }

  int mode = 0;
  switch (params.transfer) {
  case OutputTransfer::PQ:
    mode = 0;
    break;
  case OutputTransfer::SDR_BT1886:
    mode = 1;
    break;
  case OutputTransfer::SRGB:
    mode = 2;
    break;
  case OutputTransfer::HLG:
    mode = 3;
    break;
  }
  float sdr_white = params.sdr_white_nits;
  float black = params.black_nits;
  float gamma = params.gamma;
  float peak = params.peak_nits;
  float hlg_gamma = 1.0f;
  float hlg_lift = 0.0f;
  float luma[3] = {0.0f, 0.0f, 0.0f};
  if (params.transfer == OutputTransfer::HLG) {
    HlgDisplayParams(peak, black, &hlg_gamma, &hlg_lift);
    const pl_matrix3x3 rgb2xyz = pl_get_rgb2xyz_matrix(pl_raw_primaries_get(params.primaries));
    luma[0] = rgb2xyz.m[1][0];
    luma[1] = rgb2xyz.m[1][1];
    luma[2] = rgb2xyz.m[1][2];
  }
  float dst_w = static_cast<float>(dst->params.w);
  float dst_h = static_cast<float>(dst->params.h);
  float rect_x = rect.x;
  float rect_y = rect.y;
  float rect_w = rect.w;
  float rect_h = rect.h;

  pl_shader shader = pl_dispatch_begin(dp_);
  struct pl_shader_desc desc = {
      .desc =
          {
              .name = "src_tex",
              .type = PL_DESC_SAMPLED_TEX,
          },
      .binding =
          {
              .object = linear,
              .sample_mode = PL_TEX_SAMPLE_LINEAR,
          },
  };
  struct pl_shader_var vars[] = {
      {.var = pl_var_int("mode"), .data = &mode},           {.var = pl_var_float("sdr_white_nits"), .data = &sdr_white},
      {.var = pl_var_float("black_nits"), .data = &black},  {.var = pl_var_float("gamma"), .data = &gamma},
      {.var = pl_var_float("peak_nits"), .data = &peak},    {.var = pl_var_float("hlg_gamma"), .data = &hlg_gamma},
      {.var = pl_var_float("hlg_lift"), .data = &hlg_lift}, {.var = pl_var_vec3("luma"), .data = luma},
      {.var = pl_var_float("dst_w"), .data = &dst_w},       {.var = pl_var_float("dst_h"), .data = &dst_h},
      {.var = pl_var_float("rect_x"), .data = &rect_x},     {.var = pl_var_float("rect_y"), .data = &rect_y},
      {.var = pl_var_float("rect_w"), .data = &rect_w},     {.var = pl_var_float("rect_h"), .data = &rect_h},
  };

  // The body mirrors EncodeNormToCode()/PqEncodeNits()/Bt1886EncodeNits()/
  // SrgbEncode()/HlgEncodeNits(): the display-linear value is converted to nits
  // first, then only the transfer function and the single [0,1] range clamp run.
  // No tone or gamut mapping is applied here. The destination rectangle selects
  // the region; outside it the output is black.
  static const char *body =
      "vec2 p = gl_FragCoord.xy / vec2(dst_w, dst_h);\n"
      "float inside = step(rect_x, p.x) * step(p.x, rect_x + rect_w) * step(rect_y, p.y) *\n"
      "               step(p.y, rect_y + rect_h);\n"
      "vec2 q = clamp((p - vec2(rect_x, rect_y)) / vec2(rect_w, rect_h), vec2(0.0), vec2(1.0));\n"
      "vec3 lin = texture(src_tex, q).rgb * inside;\n"
      "vec3 nits = max(lin, vec3(0.0)) * 203.0;\n"
      "vec3 code;\n"
      "if (mode == 0) {\n"
      "    vec3 y = nits / 10000.0;\n"
      "    vec3 ym = pow(y, vec3(2610.0 / 16384.0));\n"
      "    vec3 num = vec3(3424.0 / 4096.0) + vec3(2413.0 / 128.0) * ym;\n"
      "    vec3 den = vec3(1.0) + vec3(2392.0 / 128.0) * ym;\n"
      "    code = pow(num / den, vec3(2523.0 / 32.0));\n"
      "} else if (mode == 1) {\n"
      "    float b = pow(max(black_nits, 0.0) / sdr_white_nits, 1.0 / gamma);\n"
      "    vec3 luminance = max(nits, vec3(black_nits));\n"
      "    code = (pow(luminance / sdr_white_nits, vec3(1.0 / gamma)) - vec3(b)) / (1.0 - b);\n"
      "} else if (mode == 2) {\n"
      "    vec3 x = nits / sdr_white_nits;\n"
      "    code = mix(vec3(12.92) * x, vec3(1.055) * pow(x, vec3(1.0 / 2.4)) - vec3(0.055),\n"
      "               greaterThanEqual(x, vec3(0.0031308)));\n"
      "} else {\n"
      "    float yl = dot(luma, nits);\n"
      "    float s = pow(max(1e-6, yl / peak_nits), (1.0 - hlg_gamma) / hlg_gamma);\n"
      "    vec3 e = vec3(12.0 / peak_nits * s) * nits;\n"
      "    code = mix(vec3(0.5) * sqrt(e), vec3(0.17883277) * log(max(e - vec3(0.28466892), vec3(1e-6))) +\n"
      "               vec3(0.55991073), greaterThan(e, vec3(1.0)));\n"
      "    code = (code - vec3(hlg_lift)) / (1.0 - hlg_lift);\n"
      "}\n"
      "color = vec4(clamp(code, 0.0, 1.0), 1.0);\n";

  struct pl_custom_shader custom = {
      .body = body,
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = &desc,
      .num_descriptors = 1,
      .variables = vars,
      .num_variables = sizeof(vars) / sizeof(vars[0]),
  };
  pl_shader_custom(shader, &custom);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst, .timer = timer};
  if (!pl_dispatch_finish(dp_, &dp_params)) {
    throw std::runtime_error("OutputEncoder::Encode: pl_dispatch_finish failed");
  }
}

} // namespace EYEQ
