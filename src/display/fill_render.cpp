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
                       ICCQueryFunc icc_query, HdrVividQueryFunc hdrvivid_query)
    : gpu_(gpu), render_params_(render_params), pixel_density_(pixel_density), icc_query_(std::move(icc_query)),
      hdrvivid_query_(std::move(hdrvivid_query)) {
  renderer_ = pl_renderer_create(log, gpu_);
  if (!renderer_) {
    throw std::runtime_error("Failed to create renderer");
  }

  amp_dp_ = pl_dispatch_create(log, gpu_);
  osd_dp_ = pl_dispatch_create(log, gpu_);

  present_color_map_ = pl_color_map_default_params;
  present_color_map_.tone_mapping_function = &pl_tone_map_clip;
  present_color_map_.gamut_mapping = &pl_gamut_map_clip;
  present_params_ = *render_params_;
  present_params_.color_map_params = &present_color_map_;
  // The final desktop hop dithers to the swapchain depth (closure b); the
  // compose and explicit-encode stages stay dither-free.
  present_params_.dither_params = pl_render_default_params.dither_params;
  present_params_.peak_detect_params = nullptr;

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

  if (osd_staging_) {
    pl_tex_destroy(gpu_, &osd_staging_);
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

void FillRender::SetMappingTarget(const MappingTarget &target, WorkPrecision precision) {
  ValidateMappingTarget(target);
  mapping_target_ = target;
  default_precision_ = precision;
  mapping_target_set_ = true;
}

void FillRender::UpdateMainFrame(const struct pl_frame *main) {
  main_w_ = main->planes[0].texture->params.w;
  main_h_ = main->planes[0].texture->params.h;
}

const struct pl_frame &FillRender::LinearResult() const {
  if (!result_frame_) {
    throw std::logic_error("FillRender::LinearResult called before RenderLinear");
  }
  return *result_frame_;
}

pl_tex FillRender::LinearResultTexture() const {
  if (!result_texture_) {
    throw std::logic_error("FillRender::LinearResultTexture called before RenderLinear");
  }
  return result_texture_;
}

void FillRender::SetResult(struct pl_frame &frame, pl_tex texture, WorkPrecision precision) {
  result_frame_ = &frame;
  result_texture_ = texture;
  result_precision_ = precision;
}

void FillRender::PrepareLinearFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height,
                                    WorkPrecision precision) {
  if (!mapping_target_set_) {
    throw std::logic_error("FillRender: mapping target not set before rendering");
  }
  pl_fmt format = FindLinearWorkFormat(gpu_, precision);
  struct pl_tex_params tex_params = {
      .w = width,
      .h = height,
      .format = format,
      .sampleable = true,
      .renderable = true,
      .blit_dst = true,
      // Host-readable so tests/diagnostics can measure the display-linear
      // error budget (16F vs 32F) without a separate readback path.
      .host_readable = true,
  };
  if (!pl_tex_recreate(gpu_, &(tex[0]), &tex_params)) {
    throw std::runtime_error("Failed to create display-linear work texture");
  }

  dst = {};
  dst.num_planes = 1;
  dst.planes[0].texture = tex[0];
  dst.planes[0].components = 4;
  dst.planes[0].component_mapping[0] = 0;
  dst.planes[0].component_mapping[1] = 1;
  dst.planes[0].component_mapping[2] = 2;
  dst.planes[0].component_mapping[3] = 3;
  // The work texture is always full-range RGB; its transfer is the display
  // mapping target (LINEAR), independent of the final swapchain/encode format.
  dst.repr = pl_color_repr_rgb;
  dst.repr.sys = PL_COLOR_SYSTEM_RGB;
  dst.repr.levels = PL_COLOR_LEVELS_FULL;
  dst.color = mapping_target_.DisplayLinearColorSpace();
}

void FillRender::RenderLinear(int width, int height, const std::map<int, struct pl_frame *> &frames,
                              const DisplayState &state) {
  const bool amplify = !state.show_ref && state.amplify.has_value() && state.ref_id.has_value();
  const WorkPrecision precision = amplify ? WorkPrecision::F32 : default_precision_;

  const int current_id = state.show_ref ? state.ref_id.value() : state.fill.id;
  const struct pl_frame *frame = frames.at(current_id);

  // Map the source to the display-linear work texture.
  dis_precision_ = precision;
  ref_precision_ = precision;
  RenderFrame(render_frame_, render_tex_, width, height, frame, state, current_id, precision);

  if (amplify) {
    const struct pl_frame *ref = frames.at(state.ref_id.value());
    // The reference and the amplification result are F32 as well: the whole
    // subtraction path must stay high precision, not just the last texture.
    ref_precision_ = WorkPrecision::F32;
    RenderFrame(ref_frame_, ref_tex_, width, height, ref, state, state.ref_id.value(), WorkPrecision::F32);
    AmplifyFrame(amplify_frame_, amplify_tex_, &render_frame_, &ref_frame_, state, WorkPrecision::F32);
    SetResult(amplify_frame_, amplify_tex_[0], WorkPrecision::F32);
  } else {
    SetResult(render_frame_, render_tex_[0], precision);
  }
}

void FillRender::RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height,
                             const struct pl_frame *src, const DisplayState &state, int video_id,
                             WorkPrecision precision) {
  pl_rect2df crop;
  crop.x0 = static_cast<float>(state.offset_x);
  crop.y0 = static_cast<float>(state.offset_y);
  crop.x1 = static_cast<float>(state.offset_x + main_w_ * state.scale);
  crop.y1 = static_cast<float>(state.offset_y + main_h_ * state.scale);
  RenderFrame(dst, tex, width, height, src, crop, video_id, precision);
}

void FillRender::RenderFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], int width, int height,
                             const struct pl_frame *src, const struct pl_rect2df &dst_crop, int video_id,
                             WorkPrecision precision, const struct pl_render_params *params) {
  PrepareLinearFrame(dst, tex, width, height, precision);
  dst.crop = dst_crop;
  Logger->trace("dst.crop is ({}, {}, {}, {})", dst_crop.x0, dst_crop.y0, dst_crop.x1, dst_crop.y1);

  // Per-video ICC is intentionally kept on the source mapping in V2-1: the
  // application ICC is a desktop presentation concern, and moving it after the
  // shared comparison would lose the per-video profile selection (V2.1 4.3).
  // In monitoring mode every application ICC is disabled (V2-2), which is what
  // makes the public result ICC-free.
  if (icc_query_) {
    pl_icc_object icc = icc_query_(video_id);
    if (icc) {
      dst.icc = icc;
    }
  }

  MapSource(src, &dst, params ? params : render_params_, video_id);
}

void FillRender::MapSource(const struct pl_frame *src, const struct pl_frame *dst,
                           const struct pl_render_params *params, int video_id) {
  // pl_render_image() performs the source interpretation and the HDR/gamut
  // mapping to the display-linear target. This is the "mapping completed"
  // point of the pipeline.
  //
  // Dynamic HDR (HDR Vivid) is applied by the same call through a render hook:
  // the CUVA curve belongs to the source-mapping stage, so it is attached here,
  // per source, and never copied into a second renderer. The shared render
  // params are not mutated because Grid composes several videos into one pass
  // and each cell needs its own hook.
  ++mapping_pass_count_[video_id];
  struct pl_render_params local_params;
  const struct pl_hook *hooks[1] = {nullptr};
  struct pl_frame local_src;
  const struct pl_frame *effective_src = src;
  const struct pl_render_params *effective_params = params;
  const struct pl_hook *vivid_hook = hdrvivid_query_ ? hdrvivid_query_(video_id) : nullptr;
  if (vivid_hook != nullptr) {
    // The CUVA curve has already mapped this source to the common target peak,
    // so the generic mapper must not tone-map it a second time. Declare the
    // post-curve peak on a copy: the stored source metadata stays untouched and
    // the public result stays free of the source's dynamic/mastering values.
    local_src = *src;
    local_src.color.hdr = mapping_target_.DisplayLinearColorSpace().hdr;
    hooks[0] = vivid_hook;
    effective_src = &local_src;
    ++hdrvivid_apply_count_;
  } else if (clip_ && pl_color_space_is_hdr(&src->color)) {
    // --tone-mapping clip / clip-rgb: declare the target range on a copy of the
    // source so the generic mapper does no tone mapping at all, then clip.
    local_src = *src;
    local_src.color.hdr = mapping_target_.DisplayLinearColorSpace().hdr;
    effective_src = &local_src;
    hooks[0] = clip_hook_;
  }
  if (hooks[0] != nullptr) {
    local_params = *effective_params;
    local_params.hooks = hooks;
    local_params.num_hooks = 1;
    effective_params = &local_params;
  }

  if (!pl_render_image(renderer_, effective_src, dst, effective_params)) {
    throw std::runtime_error("pl_render_image failed.");
  }
}

uint64_t FillRender::MappingPassCount(int video_id) const {
  auto it = mapping_pass_count_.find(video_id);
  return it == mapping_pass_count_.end() ? 0 : it->second;
}

void FillRender::ResetMappingCounts() {
  mapping_pass_count_.clear();
  hdrvivid_apply_count_ = 0;
}

void FillRender::AmplifyFrame(struct pl_frame &dst, pl_tex tex[PL_MAX_PLANES], const struct pl_frame *dis,
                              const struct pl_frame *ref, const DisplayState &state, WorkPrecision precision) {
  PrepareLinearFrame(dst, tex, dis->planes[0].texture->params.w, dis->planes[0].texture->params.h, precision);

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
  // Pass a real float: pl_var_float reads its data as float, so pointing it at
  // a std::optional<float> would depend on that type's internal layout.
  float amplify = state.amplify.value();
  // Hard clip bound of the mapped display range, in PL_HDR_NORM units. It is
  // computed from the physical target peak, not fixed at 1.0.
  float peak_norm = mapping_target_.PeakNorm();
  struct pl_shader_var vars[] = {
      {
          .var = pl_var_float("amplify"),
          .data = &amplify,
      },
      {
          .var = pl_var_float("peak_norm"),
          .data = &peak_norm,
      },
  };
  // reference + gain * (test - reference) in the display-linear domain, then a
  // per-channel hard clip to the target range. There is deliberately no tone
  // mapping / gamut compression of the difference.
  struct pl_custom_shader params = {
      .body = "vec2 tex_size = vec2(textureSize(dis_tex, 0));\n"
              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
              "\n"
              "vec3 dis_color = texture(dis_tex, pos).rgb;\n"
              "vec3 ref_color = texture(ref_tex, pos).rgb;\n"
              "vec3 amplified = ref_color + (dis_color - ref_color) * amplify;\n"
              "color = vec4(clamp(amplified, vec3(0.0), vec3(peak_norm)), 1.0);\n",
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = 2,
      .variables = vars,
      .num_variables = 2,
  };
  pl_shader_custom(shader, &params);

  struct pl_dispatch_params dp_params = {.shader = &shader, .target = dst.planes[0].texture};
  if (!pl_dispatch_finish(amp_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed.");
  }
}

pl_tex FillRender::PrepareOsdStaging(pl_tex target) {
  if (!target) {
    throw std::invalid_argument("FillRender::PrepareOsdStaging: null target");
  }
  // Same size and format as the presentation target, but sampleable so the OSD
  // blend can read the encoded result. The target is usually the swapchain
  // image, which libplacebo wraps without VK_IMAGE_USAGE_SAMPLED_BIT. It stays
  // blittable like the swapchain image because PresentTo clears the letterbox
  // with pl_tex_clear (SDR presentation hop).
  struct pl_tex_params staging_params = {
      .w = target->params.w,
      .h = target->params.h,
      .format = target->params.format,
      .sampleable = true,
      .renderable = true,
      .blit_dst = target->params.blit_dst,
  };
  if (!pl_tex_recreate(gpu_, &osd_staging_, &staging_params)) {
    throw std::runtime_error("Failed to create the OSD staging texture");
  }
  return osd_staging_;
}

void FillRender::PlotOsdText(pl_tex target, pl_tex encoded, enum pl_color_transfer target_trc) {
  if (!osd_overlay_ || !target || !encoded) {
    return;
  }
  if (!target->params.renderable) {
    throw std::invalid_argument("FillRender::PlotOsdText: target is not renderable");
  }

  // The text is blended in the *encoded* target domain (it is a desktop-only
  // overlay drawn after the encode), so its white level uses the wire transfer
  // reference white.
  float ref_white = reference_white(target_trc);
  struct pl_shader_var vars[] = {
      {
          .var = pl_var_float("ref_white"),
          .data = &ref_white,
      },
  };

  // The encoded content is read from the staging copy, never from the target:
  // the blend both samples and writes the target size, and the target may not
  // even be sampleable (swapchain).
  pl_shader shader = pl_dispatch_begin(osd_dp_);
  struct pl_shader_desc desc[2];
  desc[0] = {
      .desc = {.name = "src_tex", .type = PL_DESC_SAMPLED_TEX},
      .binding = {.object = encoded, .sample_mode = PL_TEX_SAMPLE_NEAREST},
  };
  desc[1] = {
      .desc = {.name = "osd_tex", .type = PL_DESC_SAMPLED_TEX},
      .binding = {.object = osd_overlay_, .sample_mode = PL_TEX_SAMPLE_NEAREST},
  };
  struct pl_custom_shader params = {
      .body = "vec2 tex_size = vec2(textureSize(src_tex, 0));\n"
              "vec2 pos = gl_FragCoord.xy / tex_size;\n"
              "color = texture(src_tex, pos);\n"
              "vec4 osd_color = texture(osd_tex, pos);\n"
              "osd_color.rgb *= ref_white;\n"
              "color.rgb = mix(color.rgb, osd_color.rgb, osd_color.a);\n",
      .output = PL_SHADER_SIG_COLOR,
      .descriptors = desc,
      .num_descriptors = 2,
      .variables = vars,
      .num_variables = 1,
  };
  pl_shader_custom(shader, &params);
  struct pl_dispatch_params dp_params = {.shader = &shader, .target = target};
  if (!pl_dispatch_finish(osd_dp_, &dp_params)) {
    throw std::runtime_error("pl_dispatch_finish failed (OSD text blend).");
  }
}

void FillRender::SetPresentDitherEnabled(bool enabled) {
  // pl_render_default_params.dither_params is libplacebo's default dither; the
  // hop only dithers to the output depth, it never re-maps the display-linear
  // values.
  present_params_.dither_params = enabled ? pl_render_default_params.dither_params : nullptr;
}

void FillRender::PresentTo(struct pl_frame &target, const struct pl_frame *src, const EncodeRect *preview) {
  struct pl_frame dst = target;
  struct pl_frame s = *src;
  s.crop = {}; // always present the full canvas
  if (preview != nullptr) {
    // Letterboxed desktop preview: clear the target first because the area
    // outside the destination crop is left untouched by pl_render_image.
    float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    pl_tex_clear(gpu_, target.planes[0].texture, black);
    const float tw = static_cast<float>(target.planes[0].texture->params.w);
    const float th = static_cast<float>(target.planes[0].texture->params.h);
    struct pl_rect2df crop{};
    crop.x0 = preview->x * tw;
    crop.y0 = preview->y * th;
    crop.x1 = (preview->x + preview->w) * tw;
    crop.y1 = (preview->y + preview->h) * th;
    dst.crop = crop;
  } else {
    dst.crop = {};
  }
  if (!pl_render_image(renderer_, &s, &dst, &present_params_)) {
    throw std::runtime_error("pl_render_image failed (present).");
  }
}
