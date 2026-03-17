#include "grid_render.hpp"

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/swapchain.h>

#include "log.hpp"
#include "render.hpp"

using namespace EYEQ;

GridRender::GridRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density,
                       ICCQueryFunc icc_query)
    : FillRender(log, gpu, render_params, pixel_density, std::move(icc_query)) {

  grid_dp_ = pl_dispatch_create(log, gpu_);
}

GridRender::~GridRender() {
  if (grid_dp_) {
    pl_dispatch_destroy(&grid_dp_);
  }
}

void GridRender::Render(struct pl_frame &swap_frame, const std::map<int, struct pl_frame *> &frames,
                        const DisplayState &state) {
  RenderGrid(render_frame_, render_tex_, &swap_frame, frames, state);

  if (!state.show_ref && state.amplify.has_value() && state.ref_id.has_value()) {
    DisplayState state_ref = state;
    state_ref.show_ref = true;
    RenderGrid(ref_frame_, ref_tex_, &swap_frame, frames, state_ref);

    AmplifyFrame(amplify_frame_, amplify_tex_, &render_frame_, &ref_frame_, state);
    PlotOSD(swap_frame, &amplify_frame_, state_ref);
    return;
  }

  // Finally draw text and info overlays
  PlotOSD(swap_frame, &render_frame_, state);
}

void GridRender::RenderGrid(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *swap_frame,
                            const std::map<int, struct pl_frame *> &frames, const DisplayState &state) {
  float line_width = kLineWidth * pixel_density_;
  float swap_width = swap_frame->planes[0].texture->params.w;
  float swap_height = swap_frame->planes[0].texture->params.h;
  float cell_width = (swap_width - (state.grid.cols - 1) * line_width) / state.grid.cols;
  float cell_height = (swap_height - (state.grid.rows - 1) * line_width) / state.grid.rows;

  // Render each frame at the specified position and size in swap_frame's format
  for (int row = 0; row < state.grid.rows; row++) {
    for (int col = 0; col < state.grid.cols; col++) {
      int index = row * state.grid.cols + col;
      if (index >= (int)state.ids.size()) {
        continue;
      }
      int id = state.show_ref ? state.ref_id.value() : state.ids[index];
      struct pl_frame *frame = frames.at(id);
      float texture_w = frame->planes[0].texture->params.w;
      float texture_h = frame->planes[0].texture->params.h;
      // Independently compute base scale for each video to fit the cell, then multiply by state.scale
      float base_scale = std::min(cell_width / texture_w, cell_height / texture_h);
      float real_scale = base_scale * state.scale;
      // Independently compute centering offset for each video when fitting the cell
      float fit_offset_x = (cell_width - texture_w * base_scale) / 2.f;
      float fit_offset_y = (cell_height - texture_h * base_scale) / 2.f;
      pl_rect2df in_crop, out_crop;
      out_crop.x0 = fit_offset_x + state.offset_x + col * cell_width + col * line_width;
      out_crop.y0 = fit_offset_y + state.offset_y + row * cell_height + row * line_width;
      out_crop.x1 = out_crop.x0 + texture_w * real_scale;
      out_crop.y1 = out_crop.y0 + texture_h * real_scale;

      // Render in_crop content at out_crop position
      in_crop.x0 = 0;
      in_crop.y0 = 0;
      in_crop.x1 = in_crop.x0 + texture_w;
      in_crop.y1 = in_crop.y0 + texture_h;

      // Handle out-of-bounds cases
      float oob = 0;
      if (out_crop.x0 < col * (cell_width + line_width)) {
        oob = col * (cell_width + line_width) - out_crop.x0;
        // x0 is out of bounds, hide overflowing content
        out_crop.x0 += oob;
        in_crop.x0 += oob / real_scale;
      } else if (out_crop.x0 >= col * (cell_width + line_width) + cell_width) {
        // Completely out of bounds
        out_crop.x0 = col * (cell_width + line_width) + cell_width;
        out_crop.x1 = out_crop.x0 + line_width;
      }
      if (out_crop.x1 > (col + 1) * (cell_width + line_width)) {
        oob = out_crop.x1 - ((col + 1) * (cell_width + line_width));
        // x1 is out of bounds, hide overflowing content
        out_crop.x1 -= oob;
        in_crop.x1 -= oob / real_scale;
      } else if (out_crop.x1 < col * (cell_width + line_width)) {
        // Completely out of bounds
        out_crop.x1 = col * (cell_width + line_width);
        out_crop.x0 = out_crop.x1 - line_width;
      }

      if (out_crop.y0 < row * (cell_height + line_width)) {
        oob = row * (cell_height + line_width) - out_crop.y0;
        // y0 is out of bounds, hide overflowing content
        out_crop.y0 += oob;
        in_crop.y0 += oob / real_scale;
      } else if (out_crop.y0 >= row * (cell_height + line_width) + cell_height) {
        // Completely out of bounds
        out_crop.y0 = row * (cell_height + line_width) + cell_height;
        out_crop.y1 = out_crop.y0 + line_width;
      }

      if (out_crop.y1 > (row + 1) * (cell_height + line_width)) {
        oob = out_crop.y1 - (row + 1) * (cell_height + line_width);
        // y1 is out of bounds, hide overflowing content
        out_crop.y1 -= oob;
        in_crop.y1 -= oob / real_scale;
      } else if (out_crop.y1 < row * (cell_height + line_width)) {
        // Completely out of bounds
        out_crop.y1 = row * (cell_height + line_width);
        out_crop.y0 = out_crop.y1 - line_width;
      }

      pl_rect2df tmp_crop = frame->crop;
      frame->crop = in_crop;
      if (0 == index) {
        render_params_->border = PL_CLEAR_COLOR;
        RenderFrame(dst, tex, swap_frame, frame, out_crop, id);
      } else {
        render_params_->border = PL_CLEAR_SKIP;
        dst.crop = out_crop;
        // Apply ICC profile to target frame
        if (icc_query_) {
          pl_icc_object icc = icc_query_(id);
          if (icc)
            dst.icc = icc;
        }
        if (!pl_render_image(renderer_, frame, &dst, render_params_)) {
          throw std::runtime_error("pl_render_image failed.");
        }
      }
      render_params_->border = PL_CLEAR_COLOR;
      frame->crop = tmp_crop;
    }
  }
}

void GridRender::PlotOSD(struct pl_frame &swap_frame, const struct pl_frame *src, const DisplayState &state) {
  pl_shader shader = pl_dispatch_begin(osd_dp_);

  bool has_osd = (osd_overlay_ != nullptr);

  struct pl_shader_desc desc[2];
  int num_desc = 1;
  desc[0] = {
      .desc =
          {
              .name = "tex",
              .type = PL_DESC_SAMPLED_TEX,
          },
      .binding =
          {
              .object = src->planes[0].texture,
              .sample_mode = PL_TEX_SAMPLE_NEAREST,
          },
  };

  if (has_osd) {
    desc[1] = {
        .desc =
            {
                .name = "osd_tex",
                .type = PL_DESC_SAMPLED_TEX,
            },
        .binding =
            {
                .object = osd_overlay_,
                .sample_mode = PL_TEX_SAMPLE_NEAREST,
            },
    };
    num_desc = 2;
  }

  float line_width = kLineWidth * pixel_density_;
  float ref_white = reference_white(swap_frame.color.transfer);
  float light = 0.8 * ref_white;
  struct pl_shader_var vars[] = {
      {
          .var = pl_var_int("grid_rows"),
          .data = &state.grid.rows,
      },
      {
          .var = pl_var_int("grid_cols"),
          .data = &state.grid.cols,
      },
      {
          .var = pl_var_float("line_width"),
          .data = &line_width,
      },
      {
          .var = pl_var_float("light"),
          .data = &light,
      },
      {
          .var = pl_var_float("ref_white"),
          .data = &ref_white,
      },
  };

  // Common grid line rendering
  static const char *grid_body_common =
      "vec2 tex_size = vec2(textureSize(tex, 0));\n"
      "vec2 pos = gl_FragCoord.xy / tex_size;\n"
      "\n"
      "float line_width_x = line_width / tex_size.x;\n"
      "float line_width_y = line_width / tex_size.y;\n"
      "float total_line_width_x = line_width_x * float(grid_cols - 1);\n"
      "float total_line_width_y = line_width_y * float(grid_rows - 1);\n"
      "float cell_width = (1.0 - total_line_width_x) / float(grid_cols);\n"
      "float cell_height = (1.0 - total_line_width_y) / float(grid_rows);\n"
      "\n"
      "bool on_grid_line = false;\n"
      "float x_pos = 0.0;\n"
      "float y_pos = 0.0;\n"
      "int cell_x = 0;\n"
      "int cell_y = 0;\n"
      "\n"
      "while (cell_x < grid_cols - 1) {\n"
      "    x_pos += cell_width;\n"
      "    if (pos.x < x_pos) break;\n"
      "    if (pos.x < x_pos + line_width_x) {\n"
      "        on_grid_line = true;\n"
      "        break;\n"
      "    }\n"
      "    x_pos += line_width_x;\n"
      "    cell_x++;\n"
      "}\n"
      "\n"
      "while (cell_y < grid_rows - 1) {\n"
      "    y_pos += cell_height;\n"
      "    if (pos.y < y_pos) break;\n"
      "    if (pos.y < y_pos + line_width_y) {\n"
      "        on_grid_line = true;\n"
      "        break;\n"
      "    }\n"
      "    y_pos += line_width_y;\n"
      "    cell_y++;\n"
      "}\n"
      "\n"
      "if (on_grid_line) {\n"
      "    color = vec4(light, light, light, 1.0);\n"
      "} else {\n"
      "    color = texture(tex, pos);\n"
      "}\n";

  static const char *osd_blend = "\n"
                                 "vec4 osd_color = texture(osd_tex, pos);\n"
                                 "osd_color.rgb *= ref_white;\n"
                                 "color.rgb = mix(color.rgb, osd_color.rgb, osd_color.a);\n";

  std::string body = grid_body_common;
  if (has_osd) {
    body += osd_blend;
  }

  struct pl_custom_shader params = {
      .body = body.c_str(),
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = num_desc,
      .variables = vars,
      .num_variables = 5,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = swap_frame.planes[0].texture};
  if (!pl_dispatch_finish(osd_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}

namespace EYEQ {
void calculate_best_grid_size(int &grid_w, int &grid_h, const int win_w, const int win_h, const float pixel_density,
                              const int video_w, const int video_h, const int nb_videos) {
  float line_width = kLineWidth * pixel_density;
  double best_scale = 0;

  // Try different row/column combinations
  int max_cols = std::ceil(std::sqrt(nb_videos));

  for (int cols = 1; cols <= max_cols; cols++) {
    int rows = std::ceil(static_cast<double>(nb_videos) / cols);

    // Calculate available space (subtract space occupied by grid lines)
    int available_width = win_w - (cols - 1) * line_width;
    int available_height = win_h - (rows - 1) * line_width;

    // Calculate maximum single cell size
    int cell_width = available_width / cols;
    int cell_height = available_height / rows;

    // Calculate maximum scale while maintaining aspect ratio
    double scale_by_width = static_cast<double>(cell_width) / video_w;
    double scale_by_height = static_cast<double>(cell_height) / video_h;
    double scale = std::min(scale_by_width, scale_by_height);

    // Update result if this layout provides a larger scale
    if (scale > best_scale) {
      best_scale = scale;
      grid_w = cols;
      grid_h = rows;
    }
  }
}

void calculate_best_window_size(const int grid_w, const int grid_h, int &win_w, int &win_h, const int display_w,
                                const int display_h, const float pixel_density, const int video_w, const int video_h) {
  float line_width = kLineWidth * pixel_density;

  // Calculate video aspect ratio
  double video_aspect = static_cast<double>(video_w) / video_h;

  // Calculate available space (subtract space occupied by grid lines)
  int available_width = display_w - (grid_w - 1) * line_width;
  int available_height = display_h - (grid_h - 1) * line_width;

  // Calculate maximum single cell size
  int max_cell_width = available_width / grid_w;
  int max_cell_height = available_height / grid_h;

  // Adjust cell dimensions based on video aspect ratio
  int cell_width, cell_height;
  double scale;

  if ((double)max_cell_width / max_cell_height > video_aspect) {
    // Height is the limiting factor
    cell_height = max_cell_height;
    cell_width = cell_height * video_aspect;
    scale = (double)cell_height / video_h;
  } else {
    // Width is the limiting factor
    cell_width = max_cell_width;
    cell_height = cell_width / video_aspect;
    scale = (double)cell_width / video_w;
  }

  // Ensure size does not exceed original video dimensions
  if (scale > 1.0) {
    scale = 1.0;
    cell_width = video_w;
    cell_height = video_h;
  }

  win_w = grid_w * cell_width + (grid_w - 1) * line_width;
  win_h = grid_h * cell_height + (grid_h - 1) * line_width;
}
} // namespace EYEQ
