#include "display/clip_hook.hpp"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>

#include "log.hpp"

namespace EYEQ {

namespace {

constexpr uint64_t kSignature = 0x45594551434c5247ULL; // "EYEQCLRG"

const char kMaxRgbBody[] = "float clip_max = max(color.r, max(color.g, color.b));\n"
                           "if (clip_max > clip_peak) {\n"
                           "    color.rgb *= clip_peak / clip_max;\n"
                           "}\n";

const char kPerChannelBody[] = "color.rgb = min(color.rgb, vec3(clip_peak));\n";

} // namespace

ClipHook::ClipHook(Mode mode) : mode_(mode) {
  hook_.stages = PL_HOOK_OUTPUT;
  // SIG_TEX rather than SIG_COLOR: the stage is not resizable, so the returned
  // shader must declare the exact image size, which only a texture provides.
  hook_.input = PL_HOOK_SIG_TEX;
  hook_.priv = this;
  hook_.hook = ClipHook::HookCallback;
  hook_.signature = kSignature;
}

struct pl_hook_res ClipHook::HookCallback(void *priv, const struct pl_hook_params *params) {
  return static_cast<ClipHook *>(priv)->Run(params);
}

struct pl_hook_res ClipHook::Run(const struct pl_hook_params *params) {
  if (params->components < 3 || !params->tex) {
    Logger->error("Clip hook: unexpected input ({} components)", params->components);
    return {.failed = true};
  }

  const struct pl_shader_var vars[] = {
      {.var = pl_var_float("clip_peak"), .data = &peak_norm_, .dynamic = true},
  };
  struct pl_custom_shader custom = {
      .description = mode_ == Mode::MaxRgb ? "clip to the target peak (max RGB)" : "clip RGB to the target peak",
      .body = mode_ == Mode::MaxRgb ? kMaxRgbBody : kPerChannelBody,
      .input = PL_SHADER_SIG_COLOR,
      .output = PL_SHADER_SIG_COLOR,
      .variables = vars,
      .num_variables = 1,
      .output_w = params->tex->params.w,
      .output_h = params->tex->params.h,
  };

  struct pl_sample_src src = {.tex = params->tex};
  pl_shader sh = pl_dispatch_begin(params->dispatch);
  pl_shader_sample_direct(sh, &src);
  if (!pl_shader_custom(sh, &custom)) {
    Logger->error("Failed to attach the clip shader");
    return {.failed = true};
  }

  if (!applied_logged_) {
    Logger->debug("Clip hook ({}) attached at PL_HOOK_OUTPUT, {}x{}, peak {:.3f}",
                  mode_ == Mode::MaxRgb ? "max RGB" : "per channel", params->tex->params.w, params->tex->params.h,
                  peak_norm_);
    applied_logged_ = true;
  }

  return {
      .output = PL_HOOK_SIG_COLOR,
      .sh = sh,
      .repr = params->repr,
      .color = params->color,
      .components = params->components,
      .rect = params->rect,
  };
}

} // namespace EYEQ
