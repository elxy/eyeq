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

// Linear-light brightness of the comparison split line (PL_HDR_NORM units).
static constexpr float kComparisonLineLightNorm = 0.5f;

SlideRender::SlideRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density,
                         HdrVividQueryFunc hdrvivid_query)
    : FillRender(log, gpu, render_params, pixel_density, std::move(hdrvivid_query)) {

  split_dp_ = pl_dispatch_create(log, gpu_);

  for (int i = 0; i < PL_MAX_PLANES; i++) {
    left_tex_[i] = nullptr;
    right_tex_[i] = nullptr;
    line_tex_[i] = nullptr;
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
    if (line_tex_[i]) {
      pl_tex_destroy(gpu_, &(line_tex_[i]));
    }
  }

  if (split_dp_) {
    pl_dispatch_destroy(&split_dp_);
  }
}

void SlideRender::RenderLinear(int width, int height, const std::map<int, struct pl_frame *> &frames,
                               const DisplayState &state) {
  const bool amplify = !state.show_ref && state.amplify.has_value() && state.ref_id.has_value();
  const WorkPrecision precision = amplify ? WorkPrecision::F32 : default_precision_;

  if (state.show_ref) {
    const struct pl_frame *ref = frames.at(state.ref_id.value());
    RenderFrame(render_frame_, render_tex_, width, height, ref, state, state.ref_id.value(), precision);
    SetResult(render_frame_, render_tex_[0], precision);
    return;
  }

  dis_precision_ = precision;
  ref_precision_ = precision;
  const struct pl_frame *left = frames.at(state.slide.left_id);
  const struct pl_frame *right = frames.at(state.slide.right_id);
  RenderFrame(left_frame_, left_tex_, width, height, left, state, state.slide.left_id, precision);
  RenderFrame(right_frame_, right_tex_, width, height, right, state, state.slide.right_id, precision);
  SplitFrame(render_frame_, render_tex_, &left_frame_, &right_frame_, state.slide.split_pos, precision);

  if (amplify) {
    const struct pl_frame *ref = frames.at(state.ref_id.value());
    ref_precision_ = WorkPrecision::F32;
    RenderFrame(ref_frame_, ref_tex_, width, height, ref, state, state.ref_id.value(), WorkPrecision::F32);
    AmplifyFrame(amplify_frame_, amplify_tex_, &render_frame_, &ref_frame_, state, WorkPrecision::F32);
    // The split line belongs to the comparison image, not to the amplified
    // difference; draw it after amplification so it cannot be amplified itself.
    DrawSplitLine(line_frame_, line_tex_, &amplify_frame_, state.slide.split_pos, WorkPrecision::F32);
    SetResult(line_frame_, line_tex_[0], WorkPrecision::F32);
  } else {
    DrawSplitLine(line_frame_, line_tex_, &render_frame_, state.slide.split_pos, precision);
    SetResult(line_frame_, line_tex_[0], precision);
  }
}

void SlideRender::SplitFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *left,
                             const struct pl_frame *right, float split_position, WorkPrecision precision) {
  PrepareLinearFrame(dst, tex, left->planes[0].texture->params.w, left->planes[0].texture->params.h, precision);

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
          .data = &split_position,
      },
  };
  struct pl_custom_shader params = {
      .body = "vec2 tex_size = vec2(textureSize(left_tex, 0));\n"
              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
              "\n"
              "if (pos.x <= split_pos) {\n"
              "    color = texture(left_tex, pos);\n"
              "} else {\n"
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

void SlideRender::DrawSplitLine(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *src,
                                float split_position, WorkPrecision precision) {
  PrepareLinearFrame(dst, tex, src->planes[0].texture->params.w, src->planes[0].texture->params.h, precision);

  float line_width = kLineWidth * pixel_density_;
  float light = kComparisonLineLightNorm;
  pl_shader shader = pl_dispatch_begin(split_dp_);
  struct pl_shader_desc desc = {
      .desc =
          {
              .name = "src_tex",
              .type = PL_DESC_SAMPLED_TEX,
          },
      .binding =
          {
              .object = src->planes[0].texture,
              .sample_mode = PL_TEX_SAMPLE_NEAREST,
          },
  };
  struct pl_shader_var vars[] = {
      {.var = pl_var_float("split_pos"), .data = &split_position},
      {.var = pl_var_float("line_width"), .data = &line_width},
      {.var = pl_var_float("light"), .data = &light},
  };
  struct pl_custom_shader params = {
      .body = "vec2 tex_size = vec2(textureSize(src_tex, 0));\n"
              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
              "color = texture(src_tex, pos);\n"
              "float line_width_x = line_width / tex_size.x;\n"
              "if (abs(pos.x - split_pos) < (line_width_x / 2)) {\n"
              "    color = vec4(light, light, light, 1.0);\n"
              "}\n",
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = &desc,
      .num_descriptors = 1,
      .variables = vars,
      .num_variables = 3,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst.planes[0].texture};
  if (!pl_dispatch_finish(split_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}
