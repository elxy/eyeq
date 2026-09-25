#pragma once

#include "fill_render.hpp"

namespace EYEQ {

class SlideRender : public FillRender {
public:
  SlideRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density = 1.0,
              ICCQueryFunc icc_query = nullptr);
  ~SlideRender() override;

  void RenderLinear(int width, int height, const std::map<int, struct pl_frame *> &frames,
                    const DisplayState &state) override;

protected:
  pl_dispatch split_dp_;

  struct pl_frame left_frame_;
  pl_tex left_tex_[PL_MAX_PLANES];
  struct pl_frame right_frame_;
  pl_tex right_tex_[PL_MAX_PLANES];

  // Final linear result carrying the comparison split line (kept separate from
  // the split composite so the line can be drawn after amplification without a
  // read/write feedback loop).
  struct pl_frame line_frame_;
  pl_tex line_tex_[PL_MAX_PLANES];

  void SplitFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *left,
                  const struct pl_frame *right, float split_position, WorkPrecision precision);
  void DrawSplitLine(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *src, float split_position,
                     WorkPrecision precision);
};

}; // namespace EYEQ
