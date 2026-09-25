#pragma once

#include "fill_render.hpp"

namespace EYEQ {

class GridRender : public FillRender {
public:
  GridRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density = 1.0,
             ICCQueryFunc icc_query = nullptr, HdrVividQueryFunc hdrvivid_query = nullptr);
  ~GridRender() override;

  void RenderLinear(int width, int height, const std::map<int, struct pl_frame *> &frames,
                    const DisplayState &state) override;

protected:
  pl_dispatch grid_dp_;

  // Final linear result carrying the grid lines (separate from the cell
  // composite so the lines are not read/write dependent on it).
  struct pl_frame lines_frame_;
  pl_tex lines_tex_[PL_MAX_PLANES];

  void RenderGrid(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height,
                  const std::map<int, struct pl_frame *> &frames, const DisplayState &state, WorkPrecision precision);

  void DrawGridLines(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *src,
                     const DisplayState &state, WorkPrecision precision);
};

}; // namespace EYEQ
