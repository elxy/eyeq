#include "display/output_encode.hpp"

#include <algorithm>
#include <atomic>
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

// Test/diagnostic counter for the full-frame host readback gate. The continuous
// playback/monitor path must never call it (it uses FiniteGate); tests assert the
// counter stays at zero while driving that path. It is a plain atomic counter with
// no product-side effect.
static std::atomic<uint64_t> g_linear_texture_readbacks{0};

void ValidateLinearTextureFinite(pl_gpu gpu, pl_tex tex) {
  ++g_linear_texture_readbacks;
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

uint64_t LinearTextureReadbackCount() { return g_linear_texture_readbacks.load(); }
void ResetLinearTextureReadbackCount() { g_linear_texture_readbacks.store(0); }

// GPU-side finiteness reduction. Only one thread can ever be the first to observe
// a bad texel per invocation; the atomicOr makes that observation globally visible
// through the flag buffer, which is the only thing read back to the host.
FiniteGate::FiniteGate(pl_log log, pl_gpu gpu) : gpu_(gpu) {
  if (gpu_ == nullptr) {
    throw std::invalid_argument("FiniteGate requires a pl_gpu");
  }
  if (!gpu_->glsl.compute || gpu_->limits.max_ssbo_size < sizeof(uint32_t)) {
    throw std::runtime_error("FiniteGate requires a GPU with compute shader and SSBO support");
  }
  dp_ = pl_dispatch_create(log, gpu_);
  if (dp_ == nullptr) {
    throw std::runtime_error("FiniteGate: pl_dispatch_create failed");
  }
  struct pl_buf_params params = {
      .size = sizeof(uint32_t),
      .host_writable = true,
      .host_readable = true,
      .storable = true,
      .memory_type = PL_BUF_MEM_HOST,
  };
  flag_ = pl_buf_create(gpu_, &params);
  if (flag_ == nullptr) {
    pl_dispatch_destroy(&dp_);
    throw std::runtime_error("FiniteGate: flag buffer allocation failed");
  }
}

FiniteGate::~FiniteGate() {
  if (flag_ != nullptr) {
    pl_buf_destroy(gpu_, &flag_);
  }
  if (dp_ != nullptr) {
    pl_dispatch_destroy(&dp_);
  }
}

void FiniteGate::Validate(pl_tex tex) {
  if (tex == nullptr || !tex->params.sampleable) {
    throw std::invalid_argument("FiniteGate::Validate: texture is null or not sampleable");
  }

  // Reset the flag from the host; the previous read completed, so the buffer is
  // free. This is a 4-byte write, not a frame readback.
  const uint32_t zero = 0;
  pl_buf_write(gpu_, flag_, 0, &zero, sizeof(zero));

  pl_dispatch_reset_frame(dp_);
  pl_shader shader = pl_dispatch_begin(dp_);
  if (shader == nullptr) {
    throw std::runtime_error("FiniteGate: pl_dispatch_begin failed");
  }

  struct pl_shader_desc tex_desc = {
      .desc = {.name = "src_tex", .type = PL_DESC_SAMPLED_TEX},
      .binding = {.object = tex, .sample_mode = PL_TEX_SAMPLE_NEAREST},
  };
  struct pl_buffer_var flag_var = {
      .var = {.name = "flag_buf", .type = PL_VAR_UINT, .dim_v = 1, .dim_m = 1, .dim_a = 1},
  };
  struct pl_shader_desc flag_desc = {
      .desc = {.name = "flag_block", .type = PL_DESC_BUF_STORAGE, .access = PL_DESC_ACCESS_READWRITE},
      .binding = {.object = flag_},
      .buffer_vars = &flag_var,
      .num_buffer_vars = 1,
  };
  struct pl_shader_desc descs[2] = {tex_desc, flag_desc};

  // One invocation per texel; non-finite samples set the shared flag. The body runs
  // as the generated compute entry point, so an out-of-range thread returns early.
  static const char *body = "ivec2 coord = ivec2(gl_GlobalInvocationID.xy);\n"
                            "ivec2 size = textureSize(src_tex, 0);\n"
                            "if (coord.x >= size.x || coord.y >= size.y)\n"
                            "    return;\n"
                            "vec4 v = texelFetch(src_tex, coord, 0);\n"
                            "if (any(isnan(v)) || any(isinf(v)))\n"
                            "    atomicOr(flag_buf, 1u);\n";
  struct pl_custom_shader custom = {
      .body = body,
      .input = PL_SHADER_SIG_NONE,
      .output = PL_SHADER_SIG_NONE,
      .descriptors = descs,
      .num_descriptors = 2,
      .compute = true,
      .compute_group_size = {16, 16},
  };
  if (!pl_shader_custom(shader, &custom)) {
    pl_dispatch_abort(dp_, &shader);
    throw std::runtime_error("FiniteGate: could not build the finite-check compute shader");
  }

  const int w = static_cast<int>(tex->params.w);
  const int h = static_cast<int>(tex->params.h);
  const int groups_x = (w + 15) / 16;
  const int groups_y = (h + 15) / 16;
  struct pl_dispatch_compute_params params = {
      .shader = &shader,
      .dispatch_size = {groups_x, groups_y, 1},
  };
  if (!pl_dispatch_compute(dp_, &params)) {
    // On failure pl_dispatch_compute already released the shader.
    throw std::runtime_error("FiniteGate: compute dispatch failed");
  }

  uint32_t flag = 0;
  if (!pl_buf_read(gpu_, flag_, 0, &flag, sizeof(flag))) {
    throw std::runtime_error("FiniteGate: flag readback failed");
  }
  if (flag != 0) {
    throw std::runtime_error("FiniteGate: display-linear result contains a NaN/Inf sample; refusing to encode it");
  }
}

const char *ToString(OutputTransfer transfer) {
  switch (transfer) {
  case OutputTransfer::PQ:
    return "PQ";
  case OutputTransfer::SDR_BT1886:
    return "SDR-BT1886";
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

  const int is_pq = (params.transfer == OutputTransfer::PQ) ? 1 : 0;
  float sdr_white = params.sdr_white_nits;
  float black = params.black_nits;
  float gamma = params.gamma;
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
      {.var = pl_var_int("is_pq"), .data = &is_pq},        {.var = pl_var_float("sdr_white_nits"), .data = &sdr_white},
      {.var = pl_var_float("black_nits"), .data = &black}, {.var = pl_var_float("gamma"), .data = &gamma},
      {.var = pl_var_float("dst_w"), .data = &dst_w},      {.var = pl_var_float("dst_h"), .data = &dst_h},
      {.var = pl_var_float("rect_x"), .data = &rect_x},    {.var = pl_var_float("rect_y"), .data = &rect_y},
      {.var = pl_var_float("rect_w"), .data = &rect_w},    {.var = pl_var_float("rect_h"), .data = &rect_h},
  };

  // The body mirrors EncodeNormToCode()/PqEncodeNits()/Bt1886EncodeNits(): the
  // display-linear value is converted to nits first, then only the transfer
  // function and the single [0,1] range clamp run. No tone or gamut mapping is
  // applied here. The destination rectangle selects the preview region; outside
  // it the output is black.
  static const char *body = "vec2 p = gl_FragCoord.xy / vec2(dst_w, dst_h);\n"
                            "float inside = step(rect_x, p.x) * step(p.x, rect_x + rect_w) * step(rect_y, p.y) *\n"
                            "               step(p.y, rect_y + rect_h);\n"
                            "vec2 q = clamp((p - vec2(rect_x, rect_y)) / vec2(rect_w, rect_h), vec2(0.0), vec2(1.0));\n"
                            "vec3 lin = texture(src_tex, q).rgb * inside;\n"
                            "vec3 nits = max(lin, vec3(0.0)) * 203.0;\n"
                            "vec3 code;\n"
                            "if (is_pq != 0) {\n"
                            "    vec3 y = nits / 10000.0;\n"
                            "    vec3 ym = pow(y, vec3(2610.0 / 16384.0));\n"
                            "    vec3 num = vec3(3424.0 / 4096.0) + vec3(2413.0 / 128.0) * ym;\n"
                            "    vec3 den = vec3(1.0) + vec3(2392.0 / 128.0) * ym;\n"
                            "    code = pow(num / den, vec3(2523.0 / 32.0));\n"
                            "} else {\n"
                            "    float b = pow(max(black_nits, 0.0) / sdr_white_nits, 1.0 / gamma);\n"
                            "    vec3 luminance = max(nits, vec3(black_nits));\n"
                            "    code = (pow(luminance / sdr_white_nits, vec3(1.0 / gamma)) - vec3(b)) / (1.0 - b);\n"
                            "}\n"
                            "color = vec4(clamp(code, 0.0, 1.0), 1.0);\n";

  struct pl_custom_shader custom = {
      .body = body,
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = &desc,
      .num_descriptors = 1,
      .variables = vars,
      .num_variables = 10,
  };
  pl_shader_custom(shader, &custom);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst, .timer = timer};
  if (!pl_dispatch_finish(dp_, &dp_params)) {
    throw std::runtime_error("OutputEncoder::Encode: pl_dispatch_finish failed");
  }
}

} // namespace EYEQ
