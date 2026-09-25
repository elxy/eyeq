#pragma once

#include <functional>
#include <map>

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/renderer.h>

#include "mapping_target.hpp"
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
  using ICCQueryFunc = std::function<pl_icc_object(int video_id)>;

  FillRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density = 1.0,
             ICCQueryFunc icc_query = nullptr);
  virtual ~FillRender();

  void SetMappingTarget(const MappingTarget &target, WorkPrecision precision);

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

  /*
   * Final color-managed conversion of the display-linear result into the
   * presentation/encode target (swapchain or offscreen save texture). Kept in
   * the compositor so the pl_renderer that owns the color management stays
   * encapsulated. No application ICC is applied here: ICC already belongs to
   * the per-source mapping in V2-1.
   */
  void PresentTo(struct pl_frame &target, const struct pl_frame *src);

  /*
   * Blends the text OSD overlay onto an already-encoded target. Kept separate
   * from the comparison geometry so the linear result stays OSD-free.
   */
  void PlotOsdText(pl_tex target, enum pl_color_transfer target_trc);

protected:
  pl_gpu gpu_;

  pl_renderer renderer_;
  struct pl_render_params *render_params_;
  // Presentation hop parameters: a pure color/transfer conversion with no tone
  // or gamut compression (clip), so an already-mapped display-linear result is
  // not re-mapped on its way to the swapchain/save target.
  struct pl_color_map_params present_color_map_;
  struct pl_render_params present_params_;
  float pixel_density_;
  ICCQueryFunc icc_query_;

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
  // Scratch texture for the desktop OSD blend. The target is also the blend
  // source, so the blend is written here first and then copied to the target;
  // sampling and writing one texture in a single pass is a feedback hazard.
  pl_tex osd_scratch_ = nullptr;

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
