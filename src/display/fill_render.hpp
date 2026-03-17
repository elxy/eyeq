#pragma once

#include <functional>
#include <map>

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/renderer.h>

#include "render.hpp"

namespace EYEQ {

class FillRender {
public:
  using ICCQueryFunc = std::function<pl_icc_object(int video_id)>;

  FillRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density = 1.0,
             ICCQueryFunc icc_query = nullptr);
  virtual ~FillRender();

  void UpdateMainFrame(const struct pl_frame *main);
  void SetOsdOverlay(pl_tex osd_overlay) { osd_overlay_ = osd_overlay; }

  virtual void Render(struct pl_frame &swap_frame, const std::map<int, struct pl_frame *> &frames,
                      const DisplayState &state);

protected:
  pl_gpu gpu_;

  pl_renderer renderer_;
  struct pl_render_params *render_params_;
  float pixel_density_;
  ICCQueryFunc icc_query_;

  int main_w_, main_h_;

  struct pl_frame render_frame_; // Same format as swap_frame, but with scaled dimensions
  pl_tex render_tex_[PL_MAX_PLANES];

  struct pl_frame ref_frame_; // Same format as swap_frame, but with scaled dimensions
  pl_tex ref_tex_[PL_MAX_PLANES];

  pl_dispatch amp_dp_;
  struct pl_frame amplify_frame_; // Same format as swap_frame, but with scaled dimensions
  pl_tex amplify_tex_[PL_MAX_PLANES];

  pl_dispatch osd_dp_;
  pl_tex osd_overlay_ = nullptr; // OSD overlay texture (provided externally)

  void RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *swap_frame,
                   const struct pl_frame *src, const DisplayState &state, int video_id);
  void RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *swap_frame,
                   const struct pl_frame *src, const pl_rect2df &dst_crop, int video_id);

  void AmplifyFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *dis,
                    const struct pl_frame *ref, const DisplayState &state);

  void PlotOSD(struct pl_frame &swap_frame, const struct pl_frame *src);
};

}; // namespace EYEQ
