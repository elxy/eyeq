#pragma once

#include <libplacebo/shaders/custom.h>

#include "hdrvivid_curve.hpp"

namespace EYEQ {

/**
 * @brief Everything the HDR Vivid shader needs for one frame
 *
 * The tone curve and the constant part of the saturation adjustment are solved once per
 * frame on the CPU; the shader only evaluates the per-pixel part.
 */
struct HDRVividShaderParams {
  bool valid = false;
  HDRVividCurve curve{};

  bool saturation_enabled = false;
  float saturation_b = 1.0f;  // B, constant for the frame
  float saturation_c0 = 0.0f; // exponent used below TML
  float saturation_c1 = 0.0f; // strength, 0 disables the luminance-dependent part
  float saturation_c2 = 1.0f; // exponent used between TML and RML
};

/**
 * @brief Solve the shader parameters for one frame of dynamic metadata
 *
 * @param metadata Dynamic metadata of the current frame
 * @param target_nits Target display peak luminance
 * @param mastering_nits Mastering display peak luminance
 */
void SolveHDRVividShaderParams(const HDRVividMetadata &metadata, double target_nits, double mastering_nits,
                               HDRVividShaderParams *out);

/**
 * @brief libplacebo render hook applying HDR Vivid tone mapping on the GPU
 *
 * Hooks PL_HOOK_RGB, i.e. after the planes have been merged and converted to RGB but
 * before linearization, which is where the source is still PQ-encoded RGB as expected by
 * the CUVA algorithm.
 */
class HDRVividShader {
public:
  HDRVividShader();

  HDRVividShader(const HDRVividShader &) = delete;
  HDRVividShader &operator=(const HDRVividShader &) = delete;

  /**
   * @brief Set the parameters used by the next pl_render_image() call
   */
  void SetParams(const HDRVividShaderParams &params) { params_ = params; }

  const struct pl_hook *Hook() const { return &hook_; }

private:
  struct pl_hook hook_{};
  HDRVividShaderParams params_{};
  bool applied_logged_ = false;

  static struct pl_hook_res HookCallback(void *priv, const struct pl_hook_params *params);
  struct pl_hook_res Run(const struct pl_hook_params *params);
};

} // namespace EYEQ
