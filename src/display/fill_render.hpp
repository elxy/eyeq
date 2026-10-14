#pragma once

#include <functional>
#include <map>

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/renderer.h>

#include "mapping_target.hpp"
#include "output_encode.hpp"
#include "render.hpp"

namespace EYEQ {

/*
 * Compositor for the display-linear comparison core.
 *
 * Every source is mapped into the shared MappingTarget (RGB, full range,
 * PL_COLOR_TRC_LINEAR) before it is composed, so Fill/Slide/Grid, the reference
 * switch and the difference amplification all operate on display-linear data.
 * The mapped work textures are storage-decoupled from the final
 * swapchain/encode format: their format is chosen by the requested precision
 * (F16 for plain display once verified, F32 for the difference path).
 *
 * Comparison geometry (the split/grid lines) is part of the public linear
 * result; the text OSD is blended later, on the desktop target only, so the
 * public result never contains burned-in text.
 */
class FillRender {
public:
  // Returns the HDR Vivid hook already solved for this video (or nullptr to skip
  // the dynamic tone mapping for that source). The hook maps the source to the
  // same common MappingTarget the generic mapper uses, so a source with dynamic
  // metadata still lands in the shared display-linear working space.
  using HdrVividQueryFunc = std::function<const struct pl_hook *(int video_id)>;

  FillRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density = 1.0,
             HdrVividQueryFunc hdrvivid_query = nullptr);
  virtual ~FillRender();

  void SetMappingTarget(const MappingTarget &target, WorkPrecision precision);
  // --tone-mapping clip / clip-rgb: the source mapping runs without tone mapping
  // (values above the target peak pass through) and @p hook, when set, clips the
  // result at the end of the pass. Without a hook the presentation hop clips every
  // channel in the output gamut (clip-rgb with a SDR target). Sources mapped by
  // their own HDR Vivid curve are not affected. The hook is owned by the caller.
  void SetClip(bool enabled, const struct pl_hook *hook) {
    clip_ = enabled;
    clip_hook_ = hook;
  }

  void UpdateMainFrame(const struct pl_frame *main);
  void SetOsdOverlay(pl_tex osd_overlay) { osd_overlay_ = osd_overlay; }

  /*
   * Composes `frames` into the display-linear result at the given target size.
   * The result is exposed through LinearResult()/LinearResultTexture() and must
   * be encoded by the caller (FrameRenderer) before presentation.
   */
  virtual void RenderLinear(int width, int height, const std::map<int, struct pl_frame *> &frames,
                            const DisplayState &state);

  // The display-linear composition produced by the last RenderLinear() call.
  const struct pl_frame &LinearResult() const;
  pl_tex LinearResultTexture() const;
  WorkPrecision ResultPrecision() const { return result_precision_; }
  // Diagnostics: precision of the source/reference work textures used by the
  // last amplification. Both must be F32 *before* the subtraction (V2.1 3.4).
  WorkPrecision DisPrecision() const { return dis_precision_; }
  WorkPrecision RefPrecision() const { return ref_precision_; }

  // Diagnostics: how many times the source mapping ran for @p video_id since the
  // last ResetMappingCounts(), and how many of those attached an HDR Vivid hook.
  // Used to prove that each compared source is mapped exactly once per compose
  // and that the dynamic-HDR hook runs once per frame, never per output.
  uint64_t MappingPassCount(int video_id) const;
  uint64_t HdrVividApplyCount() const { return hdrvivid_apply_count_; }
  void ResetMappingCounts();

  /*
   * Blends the text OSD overlay onto an already-encoded target. Kept separate
   * from the comparison geometry so the linear result stays OSD-free.
   *
   * @p encoded must be a sampleable copy of the encoded content: the desktop
   * presentation target (swapchain) cannot be sampled, so the caller stages the
   * encode/present result through PrepareOsdStaging() first.
   */
  void PlotOsdText(pl_tex target, pl_tex encoded, enum pl_color_transfer target_trc);

  /*
   * Returns a sampleable, renderable texture matching the presentation target's
   * size and format. The swapchain image is wrapped with COLOR_ATTACHMENT |
   * TRANSFER_DST only, so it is never sampleable; the OSD blend needs the
   * encoded content as a sample source, hence this explicit staging copy.
   */
  pl_tex PrepareOsdStaging(pl_tex target);

protected:
  pl_gpu gpu_;

  pl_renderer renderer_;
  struct pl_render_params *render_params_;
  float pixel_density_;
  HdrVividQueryFunc hdrvivid_query_;
  bool clip_ = false;
  const struct pl_hook *clip_hook_ = nullptr;
  // The single source mapping call: selects the per-source hook (HDR Vivid curve
  // or clip) and runs pl_render_image() into the display-linear @p dst.
  void MapSource(const struct pl_frame *src, const struct pl_frame *dst, const struct pl_render_params *params,
                 int video_id);
  // Mapping-pass accounting (see MappingPassCount); test/diagnostic only.
  std::map<int, uint64_t> mapping_pass_count_;
  uint64_t hdrvivid_apply_count_ = 0;

  MappingTarget mapping_target_;
  WorkPrecision default_precision_ = WorkPrecision::F16;
  bool mapping_target_set_ = false;

  int main_w_, main_h_;

  struct pl_frame render_frame_; // display-linear, target-sized
  pl_tex render_tex_[PL_MAX_PLANES];

  struct pl_frame ref_frame_; // display-linear reference, for amplification
  pl_tex ref_tex_[PL_MAX_PLANES];

  pl_dispatch amp_dp_;
  struct pl_frame amplify_frame_; // display-linear amplification result
  pl_tex amplify_tex_[PL_MAX_PLANES];

  pl_dispatch osd_dp_;
  pl_tex osd_overlay_ = nullptr; // text OSD overlay (provided externally)
  // Staging texture for the encoded content the desktop OSD blend samples. The
  // presentation target itself cannot be sampled (see PrepareOsdStaging), and
  // sampling plus writing one texture in a single pass is a feedback hazard.
  pl_tex osd_staging_ = nullptr;

  // The pl_frame that currently holds the public display-linear result.
  struct pl_frame *result_frame_ = nullptr;
  pl_tex result_texture_ = nullptr;
  WorkPrecision result_precision_ = WorkPrecision::F16;
  WorkPrecision dis_precision_ = WorkPrecision::F16;
  WorkPrecision ref_precision_ = WorkPrecision::F16;

  // Prepares a display-linear work texture of the requested precision.
  void PrepareLinearFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height,
                          WorkPrecision precision);
  void SetResult(struct pl_frame &frame, pl_tex texture, WorkPrecision precision);

  void RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height, const struct pl_frame *src,
                   const DisplayState &state, int video_id, WorkPrecision precision);
  // @p params overrides the instance render params for this call (used by Grid
  // to vary the border without mutating shared state); null means use the
  // instance params.
  void RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height, const struct pl_frame *src,
                   const struct pl_rect2df &dst_crop, int video_id, WorkPrecision precision,
                   const struct pl_render_params *params = nullptr);

  // Display-linear difference amplification. The dis/ref inputs and the result
  // are F32, and the result is hard-clipped to the mapping target's RGB range.
  void AmplifyFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *dis,
                    const struct pl_frame *ref, const DisplayState &state, WorkPrecision precision);
};

}; // namespace EYEQ
