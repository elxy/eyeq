#include "slide_render.hpp"

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/swapchain.h>

#include "log.hpp"
#include "render.hpp"

using namespace EYEQ;

SlideRender::SlideRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density,
                         ICCQueryFunc icc_query)
    : FillRender(log, gpu, render_params, pixel_density, std::move(icc_query)) {

  split_dp_ = pl_dispatch_create(log, gpu_);

  for (int i = 0; i < PL_MAX_PLANES; i++) {
    left_tex_[i] = nullptr;
    right_tex_[i] = nullptr;
  }
}

SlideRender::~SlideRender() {
  for (int i = 0; i < PL_MAX_PLANES; i++) {
    if (left_tex_[i]) {
      pl_tex_destroy(gpu_, &(left_tex_[i]));
    }
    if (right_tex_[i]) {
      pl_tex_destroy(gpu_, &(right_tex_[i]));
    }
  }

  if (split_dp_) {
    pl_dispatch_destroy(&split_dp_);
  }
}

void SlideRender::Render(struct pl_frame &swap_frame, const std::map<int, struct pl_frame *> &frames,
                         const DisplayState &state) {
  if (state.show_ref) {
    struct pl_frame *ref = frames.at(state.ref_id.value());
    RenderFrame(render_frame_, render_tex_, &swap_frame, ref, state, state.ref_id.value());
  } else {
    // First render scaled frames in swap_frame's format
    struct pl_frame *left = frames.at(state.slide.left_id);
    struct pl_frame *right = frames.at(state.slide.right_id);
    RenderFrame(left_frame_, left_tex_, &swap_frame, left, state, state.slide.left_id);
    RenderFrame(right_frame_, right_tex_, &swap_frame, right, state, state.slide.right_id);

    // Then split left and right frames based on the divider position
    SplitFrame(render_frame_, render_tex_, &left_frame_, &right_frame_, state.slide.split_pos);

    if (state.amplify.has_value() && state.ref_id.has_value()) {
      struct pl_frame *ref = frames.at(state.ref_id.value());
      RenderFrame(ref_frame_, ref_tex_, &swap_frame, ref, state, state.ref_id.value());

      AmplifyFrame(amplify_frame_, amplify_tex_, &render_frame_, &ref_frame_, state);
      PlotOSD(swap_frame, &amplify_frame_, state.slide.split_pos);
      return;
    }
  }

  // Finally draw slider, text, and info overlays
  PlotOSD(swap_frame, &render_frame_, state.slide.split_pos);
}

void SlideRender::SplitFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *left,
                             const struct pl_frame *right, float split_pos) {
  struct pl_tex_params tex_params = {
      .w = left->planes[0].texture->params.w,
      .h = left->planes[0].texture->params.h,
      .format = left->planes[0].texture->params.format,
      .sampleable = true,
      .renderable = true,
  };
  if (!pl_tex_recreate(gpu_, &(tex[0]), &tex_params)) {
    throw std::runtime_error("Failed to create texture");
  }

  dst = {
      .num_planes = 1,
      .planes =
          {
              {
                  .texture = tex[0],
                  .components = 4,
                  .component_mapping = {0, 1, 2, 3},
              },
          },
      .repr = left->repr,
      .color = left->color,
  };

  pl_shader shader = pl_dispatch_begin(split_dp_);
  struct pl_shader_desc desc[] = {
      {
          .desc =
              {
                  .name = "left_tex",
                  .type = PL_DESC_SAMPLED_TEX,
              },
          .binding =
              {
                  .object = left->planes[0].texture,
                  .sample_mode = PL_TEX_SAMPLE_NEAREST,
              },
      },
      {
          .desc =
              {
                  .name = "right_tex",
                  .type = PL_DESC_SAMPLED_TEX,
              },
          .binding =
              {
                  .object = right->planes[0].texture,
                  .sample_mode = PL_TEX_SAMPLE_NEAREST,
              },
      },
  };
  struct pl_shader_var vars[] = {
      {
          .var = pl_var_float("split_pos"),
          .data = &split_pos,
      },
  };
  struct pl_custom_shader params = {
      .body = "vec2 tex_size = vec2(textureSize(left_tex, 0));\n"
              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
              "\n"
              "// Output color\n"
              "if (pos.x <= split_pos) {\n"
              "    // Left side shows video A\n"
              "    color = texture(left_tex, pos);\n"
              "} else {\n"
              "    // Right side shows video B\n"
              "    color = texture(right_tex, pos);\n"
              "}\n",
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = 2,
      .variables = vars,
      .num_variables = 1,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst.planes[0].texture};
  if (!pl_dispatch_finish(split_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}

void SlideRender::PlotOSD(struct pl_frame &swap_frame, const struct pl_frame *src, const float split_pos) {
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
          .var = pl_var_float("split_pos"),
          .data = &split_pos,
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

  const char *body_no_osd = "vec2 tex_size = vec2(textureSize(tex, 0));\n"
                            "vec2 pos = gl_FragCoord.xy / tex_size;\n"
                            "color = texture(tex, pos);\n"
                            "float line_width_x = line_width / tex_size.x;\n"
                            "if (abs(pos.x - split_pos) < (line_width_x / 2)) {\n"
                            "    color = vec4(light, light, light, 1.0);\n"
                            "}\n";

  const char *body_with_osd = "vec2 tex_size = vec2(textureSize(tex, 0));\n"
                              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
                              "color = texture(tex, pos);\n"
                              "float line_width_x = line_width / tex_size.x;\n"
                              "if (abs(pos.x - split_pos) < (line_width_x / 2)) {\n"
                              "    color = vec4(light, light, light, 1.0);\n"
                              "}\n"
                              "vec4 osd_color = texture(osd_tex, pos);\n"
                              "osd_color.rgb *= ref_white;\n"
                              "color.rgb = mix(color.rgb, osd_color.rgb, osd_color.a);\n";

  struct pl_custom_shader params = {
      .body = has_osd ? body_with_osd : body_no_osd,
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = num_desc,
      .variables = vars,
      .num_variables = 4,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = swap_frame.planes[0].texture};
  if (!pl_dispatch_finish(osd_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}
