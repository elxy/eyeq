#include "render.hpp"

#include <cstdio>

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
#include "fill_render.hpp"
#include "slide_render.hpp"
#include "grid_render.hpp"

using namespace EYEQ;

std::string OsdInfo::Format() const {
  int hours = static_cast<int>(current_time_s) / 3600;
  int minutes = (static_cast<int>(current_time_s) % 3600) / 60;
  int seconds = static_cast<int>(current_time_s) % 60;
  char time_buf[16];
  std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", hours, minutes, seconds);

  // Build suffix for per-video offset info
  std::string suffix;
  if (frame_offset != 0) {
    suffix += fmt::format(" [{:+d}f]", frame_offset);
  }
  if (individual_paused) {
    suffix += " ||";
  }

  char buf[512];
  std::snprintf(buf, sizeof(buf), "#%d: %s %c %s [%d/%d]%s", video_id, filename.c_str(), pict_type, time_buf,
                frame_serial, total_frames, suffix.c_str());
  return std::string(buf);
}

static const struct pl_filter_config *get_filter_config(ScaleMethod method) {
  switch (method) {
  case ScaleMethod::Nearest:
    return &pl_filter_nearest;
  case ScaleMethod::Bilinear:
    return &pl_filter_bilinear;
  case ScaleMethod::Bicubic:
    return &pl_filter_bicubic;
  case ScaleMethod::Lanczos:
    return &pl_filter_lanczos;
  case ScaleMethod::EWALanczos:
    return &pl_filter_ewa_lanczos;
  case ScaleMethod::EWALanczosSharp:
    return &pl_filter_ewa_lanczossharp;
  case ScaleMethod::Mitchell:
    return &pl_filter_mitchell;
  case ScaleMethod::CatmullRom:
    return &pl_filter_catmull_rom;
  case ScaleMethod::Spline36:
    return &pl_filter_spline36;
  case ScaleMethod::Spline64:
    return &pl_filter_spline64;
  default:
    return nullptr;
  }
}

void Window::InitRender(DisplayMode display_mode, ScaleMethod &scale_method, ScaleMethod &plane_scale_method) {
  mode_ = display_mode;

  render_params_ = pl_render_default_params;
  render_params_.dither_params = nullptr;
  render_params_.peak_detect_params = nullptr;
  render_params_.background_color[0] = 0.2f;
  render_params_.background_color[1] = 0.2f;
  render_params_.background_color[2] = 0.2f;

  render_params_.upscaler = get_filter_config(scale_method);
  render_params_.plane_upscaler = get_filter_config(plane_scale_method);

  FillRender::ICCQueryFunc icc_query = [this](int video_id) -> pl_icc_object { return GetICCObjectForVideo(video_id); };

  float pixel_density = SDL_GetWindowPixelDensity(window_);
  switch (display_mode) {
  case DisplayMode::Fill:
    display_render_ =
        std::make_unique<FillRender>(swapchain_->log, swapchain_->gpu, &render_params_, pixel_density, icc_query);
    break;
  case DisplayMode::Slide:
    display_render_ =
        std::make_unique<SlideRender>(swapchain_->log, swapchain_->gpu, &render_params_, pixel_density, icc_query);
    break;
  case DisplayMode::Grid:
    display_render_ =
        std::make_unique<GridRender>(swapchain_->log, swapchain_->gpu, &render_params_, pixel_density, icc_query);
    break;
  default:
    throw std::runtime_error("not implemented");
  }

  osd_manager_ = std::make_unique<OsdManager>(swapchain_->gpu, pixel_density);
}

std::vector<int> Window::FeedFrames(std::map<int, std::shared_ptr<AVFrame>> &frames) {
  std::vector<int> failed_ids;
  std::unique_lock<std::mutex> lock(render_mutex_);
  for (auto [i, frame] : frames) {
    try {
      auto search = av_frames_.find(i);
      if (search != av_frames_.end()) {
        av_frames_.at(i).UpdateAVFrame(frame, hw_device_ref_);
      } else {
        // Avoid creating a temporary Frame that gets immediately destroyed
        av_frames_.try_emplace(i, frame, swapchain_->gpu, hw_device_ref_);
      }
      pl_frames_[i] = av_frames_.at(i).GetPLFrame();
    } catch (const texture_format_error &e) {
      Logger->warn("Video #{}: {}", i, e.what());
      failed_ids.push_back(i);
    }
  }
  if (!failed_ids.empty()) {
    return failed_ids;
  }
  struct pl_frame *main = pl_frames_.at(main_id_);
  struct pl_color_space *color = &main->color;
  ColorspaceHint(colorspace_hint_ ? color : nullptr);

  // Override SDR white level when the main video is HDR
  if (sdr_white_on_hdr_ > 0 && pl_color_space_is_hdr(color)) {
    static bool sdr_white_logged = false;
    for (auto &[id, frame] : pl_frames_) {
      if (!pl_color_space_is_hdr(&frame->color)) {
        frame->color.hdr.max_luma = sdr_white_on_hdr_;
        if (!sdr_white_logged) {
          Logger->info("Video #{}: SDR white on HDR set to {:.0f} nits", id, sdr_white_on_hdr_);
        }
      }
    }
    sdr_white_logged = true;
  }

  display_render_->UpdateMainFrame(main);
  return failed_ids;
}

// Format seconds as HH:MM:SS
static std::string format_time(float seconds) {
  int s = static_cast<int>(seconds);
  int h = s / 3600;
  int m = (s % 3600) / 60;
  int sec = s % 60;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, sec);
  return buf;
}

void Window::Render(const DisplayState &state) {
  std::unique_lock<std::mutex> lock(render_mutex_);
  if (av_frames_.empty()) {
    Logger->debug("no frames to render");
    return;
  }

  // Compose OSD overlay
  pl_tex osd_overlay = nullptr;
  if (state.show_osd && osd_manager_) {
    int win_w = PixelWidth();
    int win_h = PixelHeight();
    float pixel_density = GetDisplayPixelDensity();
    int margin = static_cast<int>(kOsdMargin * pixel_density);

    std::vector<OsdEntry> entries;

    switch (mode_) {
    case DisplayMode::Fill: {
      int id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.fill.id;
      auto it = state.osd_infos.find(id);
      if (it != state.osd_infos.end()) {
        entries.push_back({it->second.Format(), margin, margin});
      }
    } break;
    case DisplayMode::Slide: {
      int left_id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.slide.left_id;
      int right_id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.slide.right_id;
      auto it_left = state.osd_infos.find(left_id);
      if (it_left != state.osd_infos.end()) {
        entries.push_back({it_left->second.Format(), margin, margin});
      }
      auto it_right = state.osd_infos.find(right_id);
      if (it_right != state.osd_infos.end()) {
        // Top-right corner: use TTF to get actual text width for precise positioning
        std::string text = it_right->second.Format();
        int text_w = osd_manager_->GetTextWidth(text);
        int x = win_w - text_w - margin;
        if (x < 0)
          x = margin;
        entries.push_back({text, x, margin});
      }
    } break;
    case DisplayMode::Grid: {
      float line_width = kLineWidth * pixel_density;
      float cell_width = (win_w - (state.grid.cols - 1) * line_width) / state.grid.cols;
      float cell_height = (win_h - (state.grid.rows - 1) * line_width) / state.grid.rows;
      for (int row = 0; row < state.grid.rows; row++) {
        for (int col = 0; col < state.grid.cols; col++) {
          int index = row * state.grid.cols + col;
          if (index >= static_cast<int>(state.ids.size()))
            continue;
          int id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.ids[index];
          auto it = state.osd_infos.find(id);
          if (it != state.osd_infos.end()) {
            int x = static_cast<int>(col * (cell_width + line_width) + margin);
            int y = static_cast<int>(row * (cell_height + line_width) + margin);
            entries.push_back({it->second.Format(), x, y});
          }
        }
      }
    } break;
    }

    // Bottom-right corner: time at mouse position / total duration
    if (state.mouse_time_s >= 0 && state.total_duration_s > 0) {
      std::string time_text = format_time(state.mouse_time_s) + " / " + format_time(state.total_duration_s);
      int text_w = osd_manager_->GetTextWidth(time_text);
      int text_h = osd_manager_->GetTextHeight();
      int bar_height = static_cast<int>(kProgressBarHeight * pixel_density);
      int x = win_w - text_w - margin;
      int y = win_h - text_h - bar_height - margin;
      if (x < 0)
        x = margin;
      entries.push_back({time_text, x, y});
    }

    // Bottom-center: playback FPS
    {
      std::string fps_text = fmt::format("Playback FPS: {:.1f} / Main: {:.1f}", state.playback_fps, state.main_fps);
      int text_w = osd_manager_->GetTextWidth(fps_text);
      int text_h = osd_manager_->GetTextHeight();
      int bar_height = static_cast<int>(kProgressBarHeight * pixel_density);
      int x = (win_w - text_w) / 2;
      int y = win_h - text_h - bar_height - margin;
      if (x < 0)
        x = margin;
      entries.push_back({fps_text, x, y});
    }

    osd_overlay = osd_manager_->ComposeOverlay(win_w, win_h, entries, state.playback_progress);
  }
  display_render_->SetOsdOverlay(osd_overlay);

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
  RecalibrateICCParams(&target.color);

  // Rendering pipeline for different DisplayModes:
  // 1. Use sample + rect to zoom/pan the video, outputting a frame the same size as swap_frame
  // 2. Blend according to the DisplayMode
  // 3. Draw text or status info (including OSD overlay)
  display_render_->Render(target, pl_frames_, state);

  if (!pl_swapchain_submit_frame(swapchain_)) {
    throw std::runtime_error("pl_swapchain_submit_frame failed.");
  }
  pl_swapchain_swap_buffers(swapchain_);
}

float EYEQ::reference_white(enum pl_color_transfer trc) {
  switch (trc) {
  case PL_COLOR_TRC_BT_1886:
  case PL_COLOR_TRC_SRGB:
  case PL_COLOR_TRC_LINEAR:
  case PL_COLOR_TRC_GAMMA18:
  case PL_COLOR_TRC_GAMMA20:
  case PL_COLOR_TRC_GAMMA22:
  case PL_COLOR_TRC_GAMMA24:
  case PL_COLOR_TRC_GAMMA26:
  case PL_COLOR_TRC_GAMMA28:
  case PL_COLOR_TRC_PRO_PHOTO:
  case PL_COLOR_TRC_ST428:
    return 1.0;
  case PL_COLOR_TRC_PQ:
    return 0.580689;
  case PL_COLOR_TRC_HLG:
    return 0.749877;
  case PL_COLOR_TRC_V_LOG:
  case PL_COLOR_TRC_S_LOG1:
  case PL_COLOR_TRC_S_LOG2:
  default:
    return 0.5;
  }
}
