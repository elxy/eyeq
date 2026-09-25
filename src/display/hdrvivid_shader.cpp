#include "hdrvivid_shader.hpp"

#include <algorithm>
#include <cmath>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/sampling.h>

#include "log.hpp"

using namespace EYEQ;

namespace {

// Unique signature so libplacebo can disable this hook if it ever fails to execute
constexpr uint64_t kHookSignature = 0x455945515648'0001ULL; // "EYEQHV" + version

// Helper functions shared by the body. Uniform names are #define'd by pl_shader_custom,
// so they can be referenced directly.
constexpr const char *kShaderHeader = R"(
#define HV_PQ_M1 0.1593017578125
#define HV_PQ_M2 78.84375
#define HV_PQ_C1 0.8359375
#define HV_PQ_C2 18.8515625
#define HV_PQ_C3 18.6875

// PQ code value -> linear, normalized so 1.0 is 10000 nits
float hv_pq_eotf(float x) {
    x = clamp(x, 0.0, 1.0);
    float t = pow(x, 1.0 / HV_PQ_M2);
    float num = max(t - HV_PQ_C1, 0.0);
    float den = max(HV_PQ_C2 - HV_PQ_C3 * t, 1e-6);
    return pow(num / den, 1.0 / HV_PQ_M1);
}

// linear (1.0 is 10000 nits) -> PQ code value
float hv_pq_oetf(float x) {
    x = clamp(x, 0.0, 1.0);
    float t = pow(x, HV_PQ_M1);
    return pow((HV_PQ_C2 * t + HV_PQ_C1) / (1.0 + HV_PQ_C3 * t), HV_PQ_M2);
}

float hv_base_curve(float x) {
    float v = pow(max(x, 0.0), hv_m_n);
    float y = hv_m_p * v / ((hv_k1 * hv_m_p - hv_k2) * v + hv_k3);
    y = pow(max(y, 0.0), hv_m_m);
    return hv_m_a * y + hv_m_b;
}

float hv_cubic(float d, float c, float b, float a, float t) {
    return ((d * t + c) * t + b) * t + a;
}

float hv_high_extend(float x) {
    float t = hv_th3_high - hv_th2_high;
    float slope = 3.0 * hv_md2_high * t * t + 2.0 * hv_mc2_high * t + hv_mb2_high;
    return slope * (x - hv_th3_high) + hv_cubic(hv_md2_high, hv_mc2_high, hv_mb2_high, hv_ma2_high, t);
}

// Mirrors EvalHDRVividCurve() in hdrvivid_curve.cpp
float hv_calc_curve(float x) {
    bool low = hv_curve_low != 0;
    bool high = hv_curve_high != 0;

    if (low) {
        if (x <= hv_th1)
            return hv_dark_s1 * x + hv_dark_offset;
        if (x <= hv_th2)
            return hv_cubic(hv_md1, hv_mc1, hv_mb1, hv_ma1, x - hv_th1);
        if (x <= hv_th3)
            return hv_cubic(hv_md2, hv_mc2, hv_mb2, hv_ma2, x - hv_th2);
    }

    if (high) {
        if (x > hv_th1_high && x <= hv_th2_high)
            return hv_cubic(hv_md1_high, hv_mc1_high, hv_mb1_high, hv_ma1_high, x - hv_th1_high);
        if (x > hv_th2_high && x <= hv_th3_high)
            return hv_cubic(hv_md2_high, hv_mc2_high, hv_mb2_high, hv_ma2_high, x - hv_th2_high);
        if (x > hv_th3_high && hv_high_area_flag == 0)
            return hv_high_extend(x);
    }

    return hv_base_curve(x);
}

// Mirrors the saturation adjustment of the standard; the constant part (B) is solved on the CPU
float hv_saturation(float y) {
    if (hv_sat_c1 == 0.0)
        return hv_sat_b;
    if (y >= hv_rml)
        return max(hv_sat_b - hv_sat_c1 * 0.4, 0.0);
    if (y >= hv_tml) {
        float ratio = pow((y - hv_tml) / max(hv_rml - hv_tml, 1e-6), hv_sat_c2);
        return max(hv_sat_b - hv_sat_c1 * 0.4 * ratio, 0.0);
    }
    return clamp(pow(hv_calc_curve(y) / max(y, 1e-6), hv_sat_c0), 0.8, 1.0);
}
)";

constexpr const char *kShaderBody = R"(
vec3 hv_pq = clamp(color.rgb, 0.0, 1.0);
float hv_max_pq = max(max(hv_pq.r, hv_pq.g), hv_pq.b);

vec3 hv_lin = vec3(hv_pq_eotf(hv_pq.r), hv_pq_eotf(hv_pq.g), hv_pq_eotf(hv_pq.b));
float hv_max_lin = max(max(hv_lin.r, hv_lin.g), hv_lin.b);
float hv_mapped_lin = hv_pq_eotf(hv_calc_curve(hv_max_pq));

float hv_ratio = hv_max_lin <= 0.0 ? 1.0 : hv_mapped_lin / hv_max_lin;
hv_lin *= hv_ratio;

vec3 hv_out = vec3(hv_pq_oetf(hv_lin.r), hv_pq_oetf(hv_lin.g), hv_pq_oetf(hv_lin.b));

if (hv_sat_enabled != 0 && hv_max_pq > 0.0) {
    float hv_y = dot(hv_out, vec3(0.2627, 0.678, 0.0593));
    float hv_u = dot(hv_out, vec3(-0.1396, -0.3604, 0.5));
    float hv_v = dot(hv_out, vec3(0.5, -0.4598, -0.0402));
    float hv_sm = hv_saturation(hv_max_pq);
    hv_u *= hv_sm;
    hv_v *= hv_sm;
    hv_out = vec3(hv_y + 1.4746 * hv_v,
                  hv_y - 0.1645 * hv_u - 0.5713 * hv_v,
                  hv_y + 1.8814 * hv_u);
}

color.rgb = hv_out;
)";

} // namespace

void EYEQ::SolveHDRVividShaderParams(const HDRVividMetadata &metadata, double target_nits, double mastering_nits,
                                     HDRVividShaderParams *out) {
  *out = HDRVividShaderParams{};
  SolveHDRVividCurve(metadata, target_nits, mastering_nits, &out->curve);

  if (metadata.color_saturation_mapping_flag && metadata.color_saturation_num > 0) {
    const double c0 = static_cast<double>(metadata.color_saturation_gain[0]) / 128.0;
    double c1 = 0.0;
    double c2 = 1.0;
    if (metadata.color_saturation_num > 1) {
      c1 = static_cast<double>(metadata.color_saturation_gain[1] & 0xFC) / 128.0;
      c2 = std::pow(2.0, static_cast<double>(metadata.color_saturation_gain[1] & 0x3));
    }

    // B only depends on the target display luminance, so it is constant for the whole frame
    double b = 1.0;
    if (out->curve.tml > 0.0f) {
      const double mapped = EvalHDRVividCurve(out->curve, out->curve.tml);
      b = std::clamp(std::pow(mapped / out->curve.tml, c0), 0.8, 1.0);
    }

    out->saturation_enabled = true;
    out->saturation_b = static_cast<float>(b);
    out->saturation_c0 = static_cast<float>(c0);
    out->saturation_c1 = static_cast<float>(c1);
    out->saturation_c2 = static_cast<float>(c2);
  }

  out->valid = true;
}

HDRVividShader::HDRVividShader() {
  hook_.stages = PL_HOOK_RGB;
  // SIG_TEX rather than SIG_COLOR: libplacebo derives the new image size from the returned
  // shader's output size, and the shader feeding this stage has no fixed size, which would
  // leave the image at 0x0. Taking a texture gives us the exact dimensions to declare.
  hook_.input = PL_HOOK_SIG_TEX;
  hook_.priv = this;
  hook_.hook = HDRVividShader::HookCallback;
  hook_.signature = kHookSignature;
}

struct pl_hook_res HDRVividShader::HookCallback(void *priv, const struct pl_hook_params *params) {
  return static_cast<HDRVividShader *>(priv)->Run(params);
}

struct pl_hook_res HDRVividShader::Run(const struct pl_hook_params *params) {
  // Returning PL_HOOK_SIG_NONE leaves the image untouched
  struct pl_hook_res passthrough = {.output = PL_HOOK_SIG_NONE};

  if (!params_.valid || params->components < 3 || !params->tex) {
    return passthrough;
  }

  // The CUVA algorithm is defined on PQ-encoded RGB; anything else would need a different
  // input transfer function, so skip rather than produce wrong colors
  if (params->color.transfer != PL_COLOR_TRC_PQ) {
    static bool warned = false;
    if (!warned) {
      Logger->warn("HDR Vivid GPU rendering skipped: expected PQ transfer, got {}",
                   pl_color_transfer_name(params->color.transfer));
      warned = true;
    }
    return passthrough;
  }

  const HDRVividCurve &c = params_.curve;
  const int sat_enabled = params_.saturation_enabled ? 1 : 0;
  const int tex_w = params->tex->params.w;
  const int tex_h = params->tex->params.h;

  // All values change every frame, so mark them dynamic to keep libplacebo from baking them
  // into the shader source and recompiling on every frame
  const struct pl_shader_var vars[] = {
      {.var = pl_var_float("hv_m_p"), .data = &c.m_p, .dynamic = true},
      {.var = pl_var_float("hv_m_m"), .data = &c.m_m, .dynamic = true},
      {.var = pl_var_float("hv_m_a"), .data = &c.m_a, .dynamic = true},
      {.var = pl_var_float("hv_m_b"), .data = &c.m_b, .dynamic = true},
      {.var = pl_var_float("hv_m_n"), .data = &c.m_n, .dynamic = true},
      {.var = pl_var_float("hv_k1"), .data = &c.k1, .dynamic = true},
      {.var = pl_var_float("hv_k2"), .data = &c.k2, .dynamic = true},
      {.var = pl_var_float("hv_k3"), .data = &c.k3, .dynamic = true},

      {.var = pl_var_int("hv_curve_low"), .data = &c.low_valid, .dynamic = true},
      {.var = pl_var_float("hv_th1"), .data = &c.th1, .dynamic = true},
      {.var = pl_var_float("hv_th2"), .data = &c.th2, .dynamic = true},
      {.var = pl_var_float("hv_th3"), .data = &c.th3, .dynamic = true},
      {.var = pl_var_float("hv_md1"), .data = &c.d1, .dynamic = true},
      {.var = pl_var_float("hv_mc1"), .data = &c.c1, .dynamic = true},
      {.var = pl_var_float("hv_mb1"), .data = &c.b1, .dynamic = true},
      {.var = pl_var_float("hv_ma1"), .data = &c.a1, .dynamic = true},
      {.var = pl_var_float("hv_md2"), .data = &c.d2, .dynamic = true},
      {.var = pl_var_float("hv_mc2"), .data = &c.c2, .dynamic = true},
      {.var = pl_var_float("hv_mb2"), .data = &c.b2, .dynamic = true},
      {.var = pl_var_float("hv_ma2"), .data = &c.a2, .dynamic = true},
      {.var = pl_var_float("hv_dark_s1"), .data = &c.dark_slope, .dynamic = true},
      {.var = pl_var_float("hv_dark_offset"), .data = &c.dark_offset, .dynamic = true},

      {.var = pl_var_int("hv_curve_high"), .data = &c.high_valid, .dynamic = true},
      {.var = pl_var_float("hv_th1_high"), .data = &c.th1_high, .dynamic = true},
      {.var = pl_var_float("hv_th2_high"), .data = &c.th2_high, .dynamic = true},
      {.var = pl_var_float("hv_th3_high"), .data = &c.th3_high, .dynamic = true},
      {.var = pl_var_float("hv_md1_high"), .data = &c.d1_high, .dynamic = true},
      {.var = pl_var_float("hv_mc1_high"), .data = &c.c1_high, .dynamic = true},
      {.var = pl_var_float("hv_mb1_high"), .data = &c.b1_high, .dynamic = true},
      {.var = pl_var_float("hv_ma1_high"), .data = &c.a1_high, .dynamic = true},
      {.var = pl_var_float("hv_md2_high"), .data = &c.d2_high, .dynamic = true},
      {.var = pl_var_float("hv_mc2_high"), .data = &c.c2_high, .dynamic = true},
      {.var = pl_var_float("hv_mb2_high"), .data = &c.b2_high, .dynamic = true},
      {.var = pl_var_float("hv_ma2_high"), .data = &c.a2_high, .dynamic = true},
      {.var = pl_var_int("hv_high_area_flag"), .data = &c.high_clamped, .dynamic = true},

      {.var = pl_var_float("hv_tml"), .data = &c.tml, .dynamic = true},
      {.var = pl_var_float("hv_rml"), .data = &c.rml, .dynamic = true},

      {.var = pl_var_int("hv_sat_enabled"), .data = &sat_enabled, .dynamic = true},
      {.var = pl_var_float("hv_sat_b"), .data = &params_.saturation_b, .dynamic = true},
      {.var = pl_var_float("hv_sat_c0"), .data = &params_.saturation_c0, .dynamic = true},
      {.var = pl_var_float("hv_sat_c1"), .data = &params_.saturation_c1, .dynamic = true},
      {.var = pl_var_float("hv_sat_c2"), .data = &params_.saturation_c2, .dynamic = true},
  };

  struct pl_custom_shader custom = {
      .header = kShaderHeader,
      .description = "HDR Vivid dynamic tone mapping",
      .body = kShaderBody,
      .input = PL_SHADER_SIG_COLOR,
      .output = PL_SHADER_SIG_COLOR,
      .variables = vars,
      .num_variables = static_cast<int>(sizeof(vars) / sizeof(vars[0])),
      .output_w = tex_w,
      .output_h = tex_h,
  };

  // Sample the input 1:1; this also fixes the shader's output size, which libplacebo
  // copies into the image state
  struct pl_sample_src src = {.tex = params->tex};
  pl_shader sh = pl_dispatch_begin(params->dispatch);
  pl_shader_sample_direct(sh, &src);

  if (!pl_shader_custom(sh, &custom)) {
    Logger->error("Failed to attach HDR Vivid shader");
    return {.failed = true};
  }

  if (!applied_logged_) {
    Logger->debug("HDR Vivid shader attached at PL_HOOK_RGB, {}x{}, {} components", tex_w, tex_h, params->components);
    applied_logged_ = true;
  }

  // The mapping stays in the source color space (PQ / BT.2020), only the content changes
  return {
      .output = PL_HOOK_SIG_COLOR,
      .sh = sh,
      .repr = params->repr,
      .color = params->color,
      .components = params->components,
      .rect = params->rect,
  };
}
