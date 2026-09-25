#include "frame_renderer.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include <spdlog/fmt/fmt.h>

#include "log.hpp"
#include "platform_icc.h"
#include "slide_render.hpp"
#include "grid_render.hpp"
#include "utils.hpp"

using namespace EYEQ;

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

FrameRenderer::FrameRenderer(RenderContext &context, const Config &config, ColorspaceHintFunc colorspace_hint)
    : context_(&context), mode_(config.mode), colorspace_hint_enabled_(config.colorspace_hint),
      pixel_density_(config.pixel_density), colorspace_hint_(std::move(colorspace_hint)) {
  if (!context_->HasDevice()) {
    throw std::logic_error("FrameRenderer requires a RenderContext with a created device");
  }

  render_params_ = pl_render_default_params;
  render_params_.dither_params = nullptr;
  render_params_.peak_detect_params = nullptr;
  render_params_.background_color[0] = 0.2f;
  render_params_.background_color[1] = 0.2f;
  render_params_.background_color[2] = 0.2f;

  render_params_.upscaler = get_filter_config(config.scale_method);
  render_params_.plane_upscaler = get_filter_config(config.plane_scale_method);

  FillRender::ICCQueryFunc icc_query = [this](int video_id) -> pl_icc_object { return GetICCObjectForVideo(video_id); };

  pl_gpu gpu = context_->gpu();
  switch (mode_) {
  case DisplayMode::Fill:
    display_render_ = std::make_unique<FillRender>(context_->log(), gpu, &render_params_, pixel_density_, icc_query);
    break;
  case DisplayMode::Slide:
    display_render_ = std::make_unique<SlideRender>(context_->log(), gpu, &render_params_, pixel_density_, icc_query);
    break;
  case DisplayMode::Grid:
    display_render_ = std::make_unique<GridRender>(context_->log(), gpu, &render_params_, pixel_density_, icc_query);
    break;
  default:
    throw std::runtime_error("Unsupported display mode for FrameRenderer");
  }

  // The OSD is optional. Leaving it disabled keeps offscreen instances free of
  // SDL_ttf/font/SDL surface dependencies.
  if (config.enable_osd) {
    osd_manager_ = std::make_unique<OsdManager>(gpu, pixel_density_);
  }
}

FrameRenderer::~FrameRenderer() {
  // ICC objects hold GPU-related state and must be released before the
  // context's Vulkan device goes away.
  if (global_icc_object_) {
    pl_icc_close(&global_icc_object_);
  }
  for (auto &[id, obj] : per_video_icc_objects_) {
    if (obj) {
      pl_icc_close(&obj);
    }
  }
  per_video_icc_objects_.clear();

  // Frame mappings (pl_unmap_avframe, hwframe references) and the compositor's
  // GPU resources must be released before the context releases the FFmpeg
  // hw_device wrapper and the Vulkan device.
  pl_frames_.clear();
  av_frames_.clear();
  osd_manager_.reset();
  display_render_.reset();
}

void FrameRenderer::SetTargetDisplayNits(double nits, bool override_enabled) {
  target_display_nits_ = nits;
  target_display_override_enabled_ = override_enabled;
}

void FrameRenderer::SetICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id) {
  OpenICCProfile(std::move(data), source, video_id);
}

void FrameRenderer::OpenICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id) {
  if (data.empty()) {
    Logger->warn("Empty ICC profile data from '{}'", source);
    return;
  }

  ICCProfileInfo profile_info;
  profile_info.data = std::move(data);
  profile_info.source = source;
  ParseICCProfileHeader(profile_info);

  Logger->debug("Loaded ICC profile from '{}'", source);
  Logger->debug("  Version: {}.{}, Color Space: {}, PCS: {}, Header Intent: {}, Size: {} bytes",
                profile_info.version_major, profile_info.version_minor, profile_info.color_space,
                profile_info.connection_space, profile_info.header_intent, profile_info.data.size());

  size_t data_size = profile_info.data.size();
  struct pl_icc_profile pl_profile{};

  if (video_id.has_value()) {
    int id = video_id.value();
    per_video_icc_data_[id] = std::move(profile_info.data);
    pl_profile.data = per_video_icc_data_[id].data();
    pl_profile.len = per_video_icc_data_[id].size();
    pl_icc_profile_compute_signature(&pl_profile);

    // Close the old ICC object (if any)
    if (per_video_icc_objects_.count(id) && per_video_icc_objects_[id]) {
      pl_icc_close(&per_video_icc_objects_[id]);
    }
    Logger->debug("Opening ICC profile for video {} ...", id);
    per_video_icc_objects_[id] = pl_icc_open(context_->log(), &pl_profile, nullptr);
    if (!per_video_icc_objects_[id]) {
      Logger->error("Failed to open ICC profile for video {}", id);
      return;
    }
    Logger->debug("ICC profile assigned to video {}: {} ({} bytes, {} → {}), gamma={:.3f}, primaries={}", id,
                  profile_info.name, data_size, profile_info.color_space, profile_info.connection_space,
                  per_video_icc_objects_[id]->gamma,
                  pl_color_primaries_name(per_video_icc_objects_[id]->containing_primaries));
  } else {
    global_icc_data_ = std::move(profile_info.data);
    pl_profile.data = global_icc_data_.data();
    pl_profile.len = global_icc_data_.size();
    pl_icc_profile_compute_signature(&pl_profile);

    // Close the old ICC object (if any)
    if (global_icc_object_) {
      pl_icc_close(&global_icc_object_);
    }
    Logger->debug("Opening ICC profile globally ...");
    global_icc_object_ = pl_icc_open(context_->log(), &pl_profile, nullptr);
    if (!global_icc_object_) {
      Logger->error("Failed to open ICC profile globally");
      return;
    }
    Logger->debug("ICC profile assigned globally: {} ({} bytes, {} → {}), gamma={:.3f}, primaries={}",
                  profile_info.name, global_icc_data_.size(), profile_info.color_space, profile_info.connection_space,
                  global_icc_object_->gamma, pl_color_primaries_name(global_icc_object_->containing_primaries));
  }
}

pl_icc_object FrameRenderer::GetICCObjectForVideo(int video_id) const {
  auto it = per_video_icc_objects_.find(video_id);
  if (it != per_video_icc_objects_.end())
    return it->second;
  if (global_icc_object_)
    return global_icc_object_;
  return nullptr;
}

bool FrameRenderer::GetICCTargetColorSpace(struct pl_color_space *out) const {
  if (!global_icc_object_) {
    return false;
  }
  *out = {};
  out->primaries = global_icc_object_->containing_primaries;
  if (global_icc_object_->csp.transfer) {
    out->transfer = global_icc_object_->csp.transfer;
  }
  return true;
}

const struct pl_frame *FrameRenderer::GetSourceFrame(int video_id) const {
  auto it = av_frames_.find(video_id);
  if (it == av_frames_.end()) {
    return nullptr;
  }
  return it->second.GetPLFrame();
}

std::vector<int> FrameRenderer::FeedFrames(const std::map<int, std::shared_ptr<AVFrame>> &frames) {
  std::vector<int> failed_ids;
  for (auto [i, frame] : frames) {
    try {
      auto search = av_frames_.find(i);
      if (search != av_frames_.end()) {
        av_frames_.at(i).UpdateAVFrame(frame, context_->hw_device());
      } else {
        // Avoid creating a temporary Frame that gets immediately destroyed
        av_frames_.try_emplace(i, frame, context_->gpu(), context_->hw_device());
      }
      pl_frames_[i] = av_frames_.at(i).GetPLFrame();

      // Apply the target display peak to all HDR content (HLG / DPX read-back),
      // but only when the user explicitly passed --target-display-nits (opt-in override).
      struct pl_color_space *csp = &pl_frames_[i]->color;
      if (target_display_override_enabled_ && pl_color_space_is_hdr(csp)) {
        if (csp->hdr.max_luma > 0.0f && std::abs(csp->hdr.max_luma - static_cast<float>(target_display_nits_)) > 0.5f &&
            !target_display_override_logged_) {
          Logger->info("Video #{}: overriding HDR max_luma {:.0f} → target display {:.0f} nits", i, csp->hdr.max_luma,
                       target_display_nits_);
          target_display_override_logged_ = true;
        }
        csp->hdr.max_luma = static_cast<float>(target_display_nits_);
      }
    } catch (const texture_format_error &e) {
      Logger->warn("Video #{}: {}", i, e.what());
      failed_ids.push_back(i);
    }
  }
  if (!failed_ids.empty()) {
    return failed_ids;
  }

  struct pl_frame *main = pl_frames_.at(main_id_);
  // The colorspace hint touches the swapchain, which lives in Window; it is
  // invoked here while the caller still serializes Feed/Render.
  if (colorspace_hint_) {
    colorspace_hint_(colorspace_hint_enabled_ ? &main->color : nullptr);
  }

  // Override SDR white level when the main video is HDR
  if (sdr_white_on_hdr_ > 0 && pl_color_space_is_hdr(&main->color)) {
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

void FrameRenderer::Render(struct pl_frame &target, const DisplayState &state) {
  // Compose OSD overlay. The target provides the size; the OSD manager is only
  // present when the instance was created with OSD enabled.
  pl_tex osd_overlay = nullptr;
  if (state.show_osd && osd_manager_) {
    int win_w = static_cast<int>(target.planes[0].texture->params.w);
    int win_h = static_cast<int>(target.planes[0].texture->params.h);
    int margin = static_cast<int>(kOsdMargin * pixel_density_);

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
      float line_width = kLineWidth * pixel_density_;
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
      int bar_height = static_cast<int>(kProgressBarHeight * pixel_density_);
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
      int bar_height = static_cast<int>(kProgressBarHeight * pixel_density_);
      int x = (win_w - text_w) / 2;
      int y = win_h - text_h - bar_height - margin;
      if (x < 0)
        x = margin;
      entries.push_back({fps_text, x, y});
    }

    osd_overlay = osd_manager_->ComposeOverlay(win_w, win_h, entries, state.playback_progress);
  }
  display_render_->SetOsdOverlay(osd_overlay);

  display_render_->Render(target, pl_frames_, state);
}

void FrameRenderer::RecalibrateICCParams(const struct pl_color_space *target_color) {
  if (!global_icc_object_ && per_video_icc_objects_.empty())
    return;

  // Similar to plplay's render_frame approach:
  // Compute max_luma from the target's final color space, then use
  // pl_icc_update to recalibrate the ICC LUT (libplacebo skips if params unchanged)
  float target_luma = 0.0f;
  struct pl_nominal_luma_params luma_params{};
  luma_params.metadata = PL_HDR_METADATA_HDR10;
  luma_params.scaling = PL_HDR_NITS;
  luma_params.color = target_color;
  luma_params.out_max = &target_luma;
  pl_color_space_nominal_luma_ex(&luma_params);

  // Skip if params unchanged (libplacebo checks internally too, but early skip avoids redundant logging)
  if (target_luma == last_icc_target_luma_)
    return;

  Logger->debug("ICC recalibrating: target_luma {:.1f} → {:.1f} nits", last_icc_target_luma_, target_luma);
  last_icc_target_luma_ = target_luma;

  // Build update_params from each ICC object's parsed params, only updating max_luma.
  // Note: cannot use the original icc_params_.intent because PL_INTENT_AUTO (-1) is resolved
  // to the actual intent value from the ICC header inside pl_icc_open. Passing AUTO directly
  // would cause pl_icc_update to detect a param mismatch and trigger an unnecessary icc_reopen.
  if (global_icc_object_) {
    struct pl_icc_params update_params = global_icc_object_->params;
    update_params.max_luma = target_luma;
    if (!pl_icc_update(context_->log(), &global_icc_object_, nullptr, &update_params)) {
      Logger->error("Failed to recalibrate global ICC object");
    }
  }

  for (auto &[video_id, icc_obj] : per_video_icc_objects_) {
    if (icc_obj) {
      struct pl_icc_params update_params = icc_obj->params;
      update_params.max_luma = target_luma;
      if (!pl_icc_update(context_->log(), &icc_obj, nullptr, &update_params)) {
        Logger->error("Failed to recalibrate ICC object for video {}", video_id);
      }
    }
  }
}
