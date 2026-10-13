#pragma once

#include <libplacebo/shaders/custom.h>

namespace EYEQ {

/*
 * Clips the mapped source to the mapping target peak (--tone-mapping clip /
 * clip-rgb) without touching in-range pixels. libplacebo has no such mode: its
 * "clip" tone mapping works on the IPT intensity and also darkens saturated
 * in-range colors near the peak. The source mapping pass therefore runs without
 * any tone mapping and this hook clips the result.
 *
 * The hook runs at PL_HOOK_OUTPUT of the source mapping pass, i.e. on the
 * display-linear RGB of the mapping target (PL_HDR_NORM units), after libplacebo's
 * color management.
 */
class ClipHook {
public:
  enum class Mode {
    MaxRgb,     // keep the hue: scale RGB so its largest channel is at most the peak
    PerChannel, // clip every channel on its own
  };

  explicit ClipHook(Mode mode);

  // Target peak in display-linear PL_HDR_NORM units (nits / PL_COLOR_SDR_WHITE).
  void SetPeak(float peak_norm) { peak_norm_ = peak_norm; }
  const struct pl_hook *hook() const { return &hook_; }

private:
  static struct pl_hook_res HookCallback(void *priv, const struct pl_hook_params *params);
  struct pl_hook_res Run(const struct pl_hook_params *params);

  Mode mode_;
  float peak_norm_ = 1.0f;
  bool applied_logged_ = false;
  struct pl_hook hook_{};
};

} // namespace EYEQ
