#include "fill_render.hpp"

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/swapchain.h>

#include "log.hpp"
#include "render.hpp"

using namespace EYEQ;

FillRender::FillRender(pl_log log, pl_gpu gpu, struct pl_render_params *render_params, float pixel_density,
                       ICCQueryFunc icc_query)
    : gpu_(gpu), render_params_(render_params), pixel_density_(pixel_density), icc_query_(std::move(icc_query)) {
  renderer_ = pl_renderer_create(log, gpu_);
  if (!renderer_) {
    throw std::runtime_error("Failed to create renderer");
  }

  amp_dp_ = pl_dispatch_create(log, gpu_);
  osd_dp_ = pl_dispatch_create(log, gpu_);

  for (int i = 0; i < PL_MAX_PLANES; i++) {
    render_tex_[i] = nullptr;
    ref_tex_[i] = nullptr;
    amplify_tex_[i] = nullptr;
  }
}

FillRender::~FillRender() {
  for (int i = 0; i < PL_MAX_PLANES; i++) {
    if (render_tex_[i]) {
      pl_tex_destroy(gpu_, &(render_tex_[i]));
    }
    if (ref_tex_[i]) {
      pl_tex_destroy(gpu_, &(ref_tex_[i]));
    }
    if (amplify_tex_[i]) {
      pl_tex_destroy(gpu_, &(amplify_tex_[i]));
    }
  }

  if (amp_dp_) {
    pl_dispatch_destroy(&amp_dp_);
  }
  if (osd_dp_) {
    pl_dispatch_destroy(&osd_dp_);
  }

  if (renderer_) {
    pl_renderer_destroy(&renderer_);
  }
}

void FillRender::UpdateMainFrame(const struct pl_frame *main) {
  main_w_ = main->planes[0].texture->params.w;
  main_h_ = main->planes[0].texture->params.h;
}

void FillRender::Render(struct pl_frame &swap_frame, const std::map<int, struct pl_frame *> &frames,
                        const DisplayState &state) {
  struct pl_frame *frame;

  if (state.show_ref) {
    frame = frames.at(state.ref_id.value());
  } else {
    frame = frames.at(state.fill.id);
  }

  int current_id = state.show_ref ? state.ref_id.value() : state.fill.id;

  // First render the scaled frame in swap_frame's format
  RenderFrame(render_frame_, render_tex_, &swap_frame, frame, state, current_id);

  if (!state.show_ref && state.amplify.has_value() && state.ref_id.has_value()) {
    // Render reference frame
    frame = frames.at(state.ref_id.value());
    RenderFrame(ref_frame_, ref_tex_, &swap_frame, frame, state, state.ref_id.value());

    AmplifyFrame(amplify_frame_, amplify_tex_, &render_frame_, &ref_frame_, state);

    // Finally draw text and info overlays
    PlotOSD(swap_frame, &amplify_frame_);
  } else {
    // Finally draw text and info overlays
    PlotOSD(swap_frame, &render_frame_);
  }
}

void FillRender::RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *swap_frame,
                             const struct pl_frame *src, const DisplayState &state, int video_id) {
  pl_rect2df crop;
  crop.x0 = static_cast<float>(state.offset_x);
  crop.y0 = static_cast<float>(state.offset_y);
  crop.x1 = static_cast<float>(state.offset_x + main_w_ * state.scale);
  crop.y1 = static_cast<float>(state.offset_y + main_h_ * state.scale);
  RenderFrame(dst, tex, swap_frame, src, crop, video_id);
}

void FillRender::RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *swap_frame,
                             const struct pl_frame *src, const pl_rect2df &dst_crop, int video_id) {
  // Render the original frame through the normal pipeline
  struct pl_tex_params tex_params = {
      .w = swap_frame->planes[0].texture->params.w,
      .h = swap_frame->planes[0].texture->params.h,
      .format = swap_frame->planes[0].texture->params.format,
      .sampleable = true,
      .renderable = true,
      .blit_dst = true,
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
      // pl_render_image() maps src color space to dst color space
      .repr = swap_frame->repr,
      .color = swap_frame->color,
  };
  dst.crop = dst_crop;
  Logger->trace("dst.crop is ({}, {}, {}, {})", dst_crop.x0, dst_crop.y0, dst_crop.x1, dst_crop.y1);

  // Apply ICC profile to target frame
  if (icc_query_) {
    pl_icc_object icc = icc_query_(video_id);
    if (icc)
      dst.icc = icc;
  }

  if (!pl_render_image(renderer_, src, &dst, render_params_)) {
    throw std::runtime_error("pl_render_image failed.");
  }
}

void FillRender::AmplifyFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *dis,
                              const struct pl_frame *ref, const DisplayState &state) {
  struct pl_tex_params tex_params = {
      .w = dis->planes[0].texture->params.w,
      .h = dis->planes[0].texture->params.h,
      .format = dis->planes[0].texture->params.format,
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
      .repr = dis->repr,
      .color = dis->color,
  };

  pl_shader shader = pl_dispatch_begin(amp_dp_);
  struct pl_shader_desc desc[] = {
      {
          .desc =
              {
                  .name = "dis_tex",
                  .type = PL_DESC_SAMPLED_TEX,
              },
          .binding =
              {
                  .object = dis->planes[0].texture,
                  .sample_mode = PL_TEX_SAMPLE_NEAREST,
              },
      },
      {
          .desc =
              {
                  .name = "ref_tex",
                  .type = PL_DESC_SAMPLED_TEX,
              },
          .binding =
              {
                  .object = ref->planes[0].texture,
                  .sample_mode = PL_TEX_SAMPLE_NEAREST,
              },
      },
  };
  struct pl_shader_var vars[] = {
      {
          .var = pl_var_float("amplify"),
          .data = &state.amplify,
      },
  };
  struct pl_custom_shader params = {
      .body = "vec2 tex_size = vec2(textureSize(dis_tex, 0));\n"
              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
              "\n"
              "vec4 dis_color = texture(dis_tex, pos);\n"
              "vec4 ref_color = texture(ref_tex, pos);\n"
              "color = ref_color + (dis_color - ref_color) * amplify;\n"
              "color = clamp(color, 0.0, 1.0);\n"
              "color.a = 1.0;\n",
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = 2,
      .variables = vars,
      .num_variables = 1,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst.planes[0].texture};
  if (!pl_dispatch_finish(amp_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}

void FillRender::PlotOSD(struct pl_frame &swap_frame, const struct pl_frame *src) {
  pl_shader shader = pl_dispatch_begin(osd_dp_);

  bool has_osd = (osd_overlay_ != nullptr);

  struct pl_shader_desc desc[2];
  int num_desc = 1;
  desc[0] = {
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

  float ref_white = reference_white(swap_frame.color.transfer);
  struct pl_shader_var vars[] = {
      {
          .var = pl_var_float("ref_white"),
          .data = &ref_white,
      },
  };

  const char *body_no_osd = "vec2 tex_size = vec2(textureSize(src_tex, 0));\n"
                            "vec2 pos = gl_FragCoord.xy / tex_size;\n"
                            "color = texture(src_tex, pos);\n";

  const char *body_with_osd = "vec2 tex_size = vec2(textureSize(src_tex, 0));\n"
                              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
                              "color = texture(src_tex, pos);\n"
                              "vec4 osd_color = texture(osd_tex, pos);\n"
                              "osd_color.rgb *= ref_white;\n"
                              "color.rgb = mix(color.rgb, osd_color.rgb, osd_color.a);\n";

  struct pl_custom_shader params = {
      .body = has_osd ? body_with_osd : body_no_osd,
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = num_desc,
      .variables = vars,
      .num_variables = 1,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = swap_frame.planes[0].texture};
  if (!pl_dispatch_finish(osd_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}
