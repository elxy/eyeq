#include "display/output_encode.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

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

void OutputEncoder::Encode(pl_tex dst, const pl_tex linear, const EncodeParams &params) {
  if (!dst || !linear) {
    throw std::invalid_argument("OutputEncoder::Encode: null texture");
  }
  if (dst->params.w != linear->params.w || dst->params.h != linear->params.h) {
    throw std::invalid_argument(fmt::format("OutputEncoder::Encode: size mismatch ({}x{} -> {}x{})", linear->params.w,
                                            linear->params.h, dst->params.w, dst->params.h));
  }
  if (!(linear->params.sampleable)) {
    throw std::invalid_argument("OutputEncoder::Encode: source texture is not sampleable");
  }
  if (!(dst->params.renderable)) {
    throw std::invalid_argument("OutputEncoder::Encode: destination texture is not renderable");
  }

  const int is_pq = (params.transfer == OutputTransfer::PQ) ? 1 : 0;
  float sdr_white = params.sdr_white_nits;
  float black = params.black_nits;
  float gamma = params.gamma;

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
              .sample_mode = PL_TEX_SAMPLE_NEAREST,
          },
  };
  struct pl_shader_var vars[] = {
      {.var = pl_var_int("is_pq"), .data = &is_pq},
      {.var = pl_var_float("sdr_white_nits"), .data = &sdr_white},
      {.var = pl_var_float("black_nits"), .data = &black},
      {.var = pl_var_float("gamma"), .data = &gamma},
  };

  // The body mirrors EncodeNormToCode()/PqEncodeNits()/Bt1886EncodeNits(): the
  // display-linear value is converted to nits first, then only the transfer
  // function and the single [0,1] range clamp run. No tone or gamut mapping is
  // applied here.
  static const char *body = "vec2 tex_size = vec2(textureSize(src_tex, 0));\n"
                            "vec2 pos = gl_FragCoord.xy / tex_size;\n"
                            "vec3 lin = texture(src_tex, pos).rgb;\n"
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
      .num_variables = 4,
  };
  pl_shader_custom(shader, &custom);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst};
  if (!pl_dispatch_finish(dp_, &dp_params)) {
    throw std::runtime_error("OutputEncoder::Encode: pl_dispatch_finish failed");
  }
}

} // namespace EYEQ
