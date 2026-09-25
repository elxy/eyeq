#include "render.hpp"

#include <cstdio>
#include <cmath>
#include <vector>

extern "C" {
#include <libavutil/file.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/colorspace.h>
#include <spdlog/fmt/fmt.h>

#include "log.hpp"
#include "osd.hpp"
#include "utils.hpp"
#include "window.hpp"
#include "frame_renderer.hpp"

using namespace EYEQ;

void Window::InitRender(DisplayMode display_mode, ScaleMethod &scale_method, ScaleMethod &plane_scale_method) {
  FrameRenderer::Config config;
  config.mode = display_mode;
  config.scale_method = scale_method;
  config.plane_scale_method = plane_scale_method;
  config.pixel_density = SDL_GetWindowPixelDensity(window_);
  config.enable_osd = true;
  config.colorspace_hint = colorspace_hint_;

  frame_renderer_ = std::make_unique<FrameRenderer>(
      *context_, config, [this](const struct pl_color_space *colorspace) { ColorspaceHint(colorspace); });
}

std::vector<int> Window::FeedFrames(const std::map<int, std::shared_ptr<AVFrame>> &frames) {
  // render_mutex_ serializes FeedFrames (decode thread) against Render (main
  // thread) and covers the whole FrameRenderer state, including the colorspace
  // hint callback that touches the swapchain.
  std::unique_lock<std::mutex> lock(render_mutex_);
  return frame_renderer_->FeedFrames(frames);
}

void Window::Render(const DisplayState &state) {
  std::unique_lock<std::mutex> lock(render_mutex_);
  if (!frame_renderer_->HasFrames()) {
    Logger->debug("no frames to render");
    return;
  }

  struct pl_swapchain_frame swap_frame{};
  if (!pl_swapchain_start_frame(swapchain_, &swap_frame)) {
    throw std::runtime_error("start frame failed.");
  }
  struct pl_frame target{};
  pl_frame_from_swapchain(&target, &swap_frame);

  // Diagnostic: log the swapchain's final color space selection (logged only once)
  static bool swapchain_csp_logged = false;
  if (!swapchain_csp_logged) {
    Logger->debug("swapchain output: primaries={}, transfer={}, bits={}/{}",
                  pl_color_primaries_name(target.color.primaries), pl_color_transfer_name(target.color.transfer),
                  target.repr.bits.color_depth, target.repr.bits.sample_depth);
    Logger->debug("swapchain fbo format: {}, size={}x{}", target.planes[0].texture->params.format->name,
                  target.planes[0].texture->params.w, target.planes[0].texture->params.h);
    swapchain_csp_logged = true;
  }

  // Dynamically recalibrate ICC parameters based on the swapchain's final color space
  frame_renderer_->RecalibrateICCParams(&target.color);

  // Rendering pipeline for different DisplayModes:
  // 1. Use sample + rect to zoom/pan the video, outputting a frame the same size as swap_frame
  // 2. Blend according to the DisplayMode
  // 3. Draw text or status info (including OSD overlay)
  struct pl_frame render_dst = target;
  bool saving = pending_render_save_.has_value();
  if (saving) {
    // When saving, render into a host-readable RGBA32F texture instead of the
    // swapchain, then blit the result to the swapchain and download it.
    pl_gpu gpu = context_->gpu();
    pl_fmt save_fmt = pl_find_fmt(
        gpu, PL_FMT_FLOAT, 4, 32, 32,
        static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
    struct pl_tex_params save_params = {
        .w = target.planes[0].texture->params.w,
        .h = target.planes[0].texture->params.h,
        .format = save_fmt,
        .sampleable = true,
        .renderable = true,
        .host_readable = true,
    };
    if (!save_fmt || !pl_tex_recreate(gpu, &render_save_tex_, &save_params)) {
      Logger->error("Failed to create save texture; frame save aborted");
      pending_render_save_.reset();
      saving = false;
    } else {
      render_dst.planes[0].texture = render_save_tex_;
      render_dst.planes[0].components = 4;
      render_dst.planes[0].component_mapping[0] = 0;
      render_dst.planes[0].component_mapping[1] = 1;
      render_dst.planes[0].component_mapping[2] = 2;
      render_dst.planes[0].component_mapping[3] = 3;
    }
  }

  frame_renderer_->Render(render_dst, state);

  if (saving) {
    pl_gpu gpu = context_->gpu();
    // Blit the intermediate texture to the swapchain (pure format conversion)
    if (!render_save_dp_) {
      render_save_dp_ = pl_dispatch_create(context_->log(), gpu);
    }
    if (render_save_dp_) {
      pl_shader shader = pl_dispatch_begin(render_save_dp_);
      struct pl_shader_desc desc = {
          .desc =
              {
                  .name = "src_tex",
                  .type = PL_DESC_SAMPLED_TEX,
              },
          .binding =
              {
                  .object = render_save_tex_,
                  .sample_mode = PL_TEX_SAMPLE_NEAREST,
              },
      };
      struct pl_custom_shader params = {
          .body = "vec2 tex_size = vec2(textureSize(src_tex, 0));\n"
                  "vec2 pos = gl_FragCoord.xy / tex_size;\n"
                  "color = texture(src_tex, pos);\n",
          .output = PL_SHADER_SIG_COLOR,
          .descriptors = &desc,
          .num_descriptors = 1,
      };
      pl_shader_custom(shader, &params);
      struct pl_dispatch_params dp_params = {.shader = &shader, .target = target.planes[0].texture};
      if (!pl_dispatch_finish(render_save_dp_, &dp_params)) {
        Logger->error("Failed to blit save texture to swapchain");
      }
    } else {
      Logger->error("Failed to create save dispatch; frame save aborted");
    }

    // Download the rendered frame
    const size_t w = render_save_tex_->params.w;
    const size_t h = render_save_tex_->params.h;
    std::vector<float> pixels(w * h * 4);
    struct pl_tex_transfer_params transfer_params = {
        .tex = render_save_tex_,
        .row_pitch = w * 4 * sizeof(float),
        .ptr = pixels.data(),
    };
    if (!pl_tex_download(gpu, &transfer_params)) {
      Logger->error("Failed to download save texture");
    } else {
      // Peak luminance of the rendered output. --target-display-nits, when passed, already
      // overrode the frame's max_luma; otherwise this keeps the source's mastering display peak.
      float peak = target.color.hdr.max_luma;
      int ret = save_render_frame(pixels.data(), static_cast<int>(w), static_cast<int>(h), target.color.transfer,
                                  target.color.primaries, peak,
                                  target.planes[0].texture->params.format->component_depth[0], *pending_render_save_);
      if (ret < 0) {
        Logger->error("Failed to save rendered frame to {}", pending_render_save_->string());
      } else {
        Logger->info("{} saved", pending_render_save_->string());
      }
    }
    pending_render_save_.reset();
  }

  if (!pl_swapchain_submit_frame(swapchain_)) {
    throw std::runtime_error("pl_swapchain_submit_frame failed.");
  }
  pl_swapchain_swap_buffers(swapchain_);
}
