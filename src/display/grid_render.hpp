#pragma once

#include "fill_render.hpp"

namespace EYEQ {

class GridRender : public FillRender {
public:
  GridRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density = 1.0,
             ICCQueryFunc icc_query = nullptr);
  ~GridRender() override;

  void Render(struct pl_frame &swap_frame, const std::map<int, struct pl_frame *> &frames,
              const DisplayState &state) override;

protected:
  pl_dispatch grid_dp_;

  void RenderGrid(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *swap_frame,
                  const std::map<int, struct pl_frame *> &frames, const DisplayState &state);

  void PlotOSD(struct pl_frame &swap_frame, const struct pl_frame *src, const DisplayState &state);
};

}; // namespace EYEQ
