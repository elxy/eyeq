#include "window.hpp"

#include <cassert>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
extern "C" {
#include <libavutil/bprint.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/rational.h>
}
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/swapchain.h>

#include "platform_icc.h"

using namespace EYEQ;

extern const std::vector<const char *> optional_device_exts;

Window::Window(const std::string &title, int video_width, int video_height, LoggingLevel log_level,
               bool colorspace_hint, bool high_dpi, SDL_DisplayID display_id)
    : colorspace_hint_(colorspace_hint), high_dpi_(high_dpi) {

  display_id = display_id ? display_id : Window::CurrentDisplay();
  InitWindow(title, video_width, video_height, display_id);

  log_ = create_pl_log(log_level);
  CreateVulkanContext();

  main_id_ = 0;

  {
    int width, height;
    SDL_GetWindowSize(window_, &width, &height);
    ori_width_ = width;
    ori_height_ = height;
    ori_display_id_ = SDL_GetDisplayForWindow(window_);
    Logger->debug("window size: {}x{}", width, height);
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    Logger->debug("window size in pixels: {}x{}", width, height);
    Logger->debug("window display scale: {}", SDL_GetWindowDisplayScale(window_));
    Logger->debug("window pixel density: {}", SDL_GetWindowPixelDensity(window_));
  }
}

Window::~Window() {
  // ICC objects must be released before vulkan_ is destroyed (they hold GPU-related state)
  if (global_icc_object_) {
    pl_icc_close(&global_icc_object_);
  }
  for (auto &[id, obj] : per_video_icc_objects_) {
    if (obj) {
      pl_icc_close(&obj);
    }
  }
  per_video_icc_objects_.clear();

  // GPU resources must be released before vulkan_ is destroyed
  av_frames_.clear();
  osd_manager_.reset();
  display_render_.reset();

  if (vulkan_) {
    pl_swapchain_destroy(&swapchain_);
    pl_vulkan_destroy(&vulkan_);
  }

  if (vk_surface_) {
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR =
        (PFN_vkDestroySurfaceKHR)get_proc_addr_(vk_inst_, "vkDestroySurfaceKHR");
    vkDestroySurfaceKHR(vk_inst_, vk_surface_, nullptr);
    vk_surface_ = VK_NULL_HANDLE;
  }

  av_buffer_unref(&hw_device_ref_);
  pl_log_destroy(&log_);

  SDL_DestroyWindow(window_);
  window_ = nullptr;
}

SDL_DisplayID Window::CurrentDisplay() {
  // Get current mouse position
  float mouse_x, mouse_y;
  SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
  SDL_Point mouse_point = {(int)mouse_x, (int)mouse_y};
  SDL_DisplayID current_display = SDL_GetDisplayForPoint(&mouse_point);
  return current_display;
}

void Window::GetDisplayResolution(int &display_w, int &display_h, SDL_DisplayID display_id, bool high_dpi) {
  const SDL_DisplayMode *display = SDL_GetCurrentDisplayMode(display_id);
  if (high_dpi) {
    display_w = display->w * display->pixel_density;
    display_h = display->h * display->pixel_density;
  } else {
    display_w = display->w;
    display_h = display->h;
  }
}

float Window::GetDisplayPixelDensity(SDL_DisplayID display_id, bool high_dpi) {
  const SDL_DisplayMode *display = SDL_GetCurrentDisplayMode(display_id);
  if (high_dpi) {
    return display->pixel_density;
  } else {
    return 1.0;
  }
}

float Window::GetDisplayPixelDensity() { return SDL_GetWindowPixelDensity(window_); }

void Window::Reset() {
  Logger->trace("set window size to: {}x{}", ori_width_, ori_height_);
  if (!SDL_SetWindowSize(window_, ori_width_, ori_height_)) {
    throw std::runtime_error(fmt::format("Failed to resize window: {}", SDL_GetError()));
  }
  MoveWindowToCenter(ori_display_id_);
}

bool Window::OnResized() {
  int width, height;
  if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
    Logger->error("Could not get window size in pixels: {}", SDL_GetError());
    return false;
  }
  Logger->debug("window size in pixels: {}x{}", width, height);

  // FIXME: After resize, ensure full swapchain recreation
  // ref: https://vulkan-tutorial.com/Drawing_a_triangle/Swap_chain_recreation
  if (!pl_swapchain_resize(swapchain_, &width, &height)) {
    Logger->error("Failed to resize swapchain");
    return false;
  } else {
    bool suboptimal = pl_vulkan_swapchain_suboptimal(swapchain_);
    Logger->debug("swapchain size: {}x{}, suboptimal {}", width, height, suboptimal);
    return true;
  }
}

void Window::FitResolutionWithAspectRatio(int &resize_width, int &resize_height, float pic_aspect, int display_width,
                                          int display_height) {
  float display_aspect = static_cast<float>(display_width) / display_height;
  if (display_aspect > pic_aspect) {
    // If the display area aspect ratio is larger than the image, the display is too wide
    resize_width = display_height * pic_aspect;
    resize_height = display_height;
  } else {
    // If the display area aspect ratio is smaller than the image, the display is too tall
    resize_width = display_width;
    resize_height = display_width / pic_aspect;
  }
}

void Window::InitWindow(const std::string &title, int video_width, int video_height, SDL_DisplayID display_id) {
  // High-DPI: https://wiki.libsdl.org/SDL3/README/highdpi
  uint32_t flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
  if (high_dpi_)
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

  // Adjust display area based on the display device
  const SDL_DisplayMode *display = SDL_GetCurrentDisplayMode(display_id);
  SDL_PropertiesID prop_id = SDL_GetDisplayProperties(display_id);
  bool hdr = SDL_GetBooleanProperty(prop_id, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
  Logger->debug("display {}, resolution {}x{}, pixel density {}, refresh rate {}, HDR {}",
                SDL_GetDisplayName(display_id), display->w, display->h, display->pixel_density, display->refresh_rate,
                hdr ? "enabled" : "disabled");

  int window_width = video_width;
  int window_height = video_height;
  if (high_dpi_) {
    window_width = video_width / display->pixel_density;
    window_height = video_height / display->pixel_density;
  }
  if (window_width > display->w || window_height > display->h) {
    Logger->warn("Window size is too large, scaling down to fit display area");
    Window::FitResolutionWithAspectRatio(window_width, window_height, static_cast<float>(video_width) / video_height,
                                         display->w, display->h);
  }

  window_ = SDL_CreateWindow(title.c_str(), window_width, window_height, flags);
  if (!window_) {
    throw std::runtime_error(fmt::format("Failed to create window: {}", SDL_GetError()));
  }

  MoveWindowToCenter(display_id);
  SDL_RaiseWindow(window_);
}

void Window::MoveWindowToCenter(SDL_DisplayID display_id) {
  SDL_Rect displayBounds;
  if (!SDL_GetDisplayBounds(display_id, &displayBounds)) {
    Logger->error("Error getting display bounds: %s", SDL_GetError());
    return;
  }
  int window_width, window_height;
  SDL_GetWindowSize(window_, &window_width, &window_height);
  int x = displayBounds.x + (displayBounds.w - window_width) / 2;
  int y = displayBounds.y + (displayBounds.h - window_height) / 2;
  SDL_SetWindowPosition(window_, x, y);
}

void Window::CreateVulkanContext() {
  AVHWDeviceContext *device_ctx;
  AVVulkanDeviceContext *vk_dev_ctx;

  unsigned num_ext = 0;
  char const *const *ext;
  ext = SDL_Vulkan_GetInstanceExtensions(&num_ext);
  if (nullptr == ext) {
    std::string msg = fmt::format("Failed to get vulkan extensions: {}", SDL_GetError());
    throw std::runtime_error(msg);
  }
  get_proc_addr_ = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();

  struct pl_vk_inst_params inst_params = {
      .debug = log_->params.log_level >= PL_LOG_DEBUG,
      .get_proc_addr = get_proc_addr_,
      .extensions = ext,
      .num_extensions = static_cast<int>(num_ext),
  };
  pl_inst_ = pl_vk_inst_create(log_, &inst_params);
  if (!pl_inst_) {
    throw std::runtime_error("Failed to create placebo instance");
  }
  vk_inst_ = pl_inst_->instance;

  if (!SDL_Vulkan_CreateSurface(window_, vk_inst_, NULL, &vk_surface_)) {
    throw std::runtime_error("Failed to create vulkan surface");
  }

  struct pl_vulkan_params vk_params = {
      .instance = vk_inst_,
      .get_proc_addr = get_proc_addr_,
      .surface = vk_surface_,
      .allow_software = false,
      .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
      .opt_extensions = optional_device_exts.data(),
      .num_opt_extensions = static_cast<int>(optional_device_exts.size()),
  };
  vulkan_ = pl_vulkan_create(log_, &vk_params);
  if (!vulkan_) {
    throw std::runtime_error("Failed to create libplacebo vulkan device");
  }

  hw_device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if (!hw_device_ref_) {
    Logger->debug("linked libavutil was not built with vulkan support");
    goto exit;
  }

  device_ctx = (AVHWDeviceContext *)hw_device_ref_->data;
  device_ctx->user_opaque = this;

  vk_dev_ctx = (AVVulkanDeviceContext *)device_ctx->hwctx;
  vk_dev_ctx->lock_queue = Window::HWCallbackLockQueue;
  vk_dev_ctx->unlock_queue = Window::HWCallbackUnlockQueue;

  vk_dev_ctx->get_proc_addr = pl_inst_->get_proc_addr;

  vk_dev_ctx->inst = pl_inst_->instance;
  vk_dev_ctx->phys_dev = vulkan_->phys_device;
  vk_dev_ctx->act_dev = vulkan_->device;

  vk_dev_ctx->device_features = *(vulkan_->features);

  vk_dev_ctx->enabled_inst_extensions = pl_inst_->extensions;
  vk_dev_ctx->nb_enabled_inst_extensions = pl_inst_->num_extensions;

  vk_dev_ctx->enabled_dev_extensions = vulkan_->extensions;
  vk_dev_ctx->nb_enabled_dev_extensions = vulkan_->num_extensions;

  if (av_hwdevice_ctx_init(hw_device_ref_) < 0) {
    throw std::runtime_error("Failed to initialize hwdevice context");
  }

exit:
  struct pl_vulkan_swapchain_params swapchain_params = {
      .surface = vk_surface_,
      .present_mode = VK_PRESENT_MODE_FIFO_KHR,
      .allow_suboptimal = false,
  };
  swapchain_ = pl_vulkan_create_swapchain(vulkan_, &swapchain_params);
  if (!swapchain_) {
    throw std::runtime_error("Failed to create swapchain");
  }
}

void Window::ColorspaceHint(struct pl_color_space *colorspace) {
  bool has_icc = (global_icc_object_ != nullptr) || (!per_video_icc_objects_.empty());

  if (has_icc && global_icc_object_) {
    // When ICC is applied, the hint should match the ICC target color space;
    // otherwise macOS compositor will apply a secondary correction based on an incorrect color space declaration
    struct pl_color_space icc_target{};
    icc_target.primaries = global_icc_object_->containing_primaries;
    if (global_icc_object_->csp.transfer)
      icc_target.transfer = global_icc_object_->csp.transfer;

    static bool hint_logged = false;
    if (!hint_logged) {
      Logger->debug("ICC active, colorspace hint overridden to ICC target: primaries={}, transfer={}",
                    pl_color_primaries_name(icc_target.primaries), pl_color_transfer_name(icc_target.transfer));
      hint_logged = true;
    }
    pl_swapchain_colorspace_hint(swapchain_, &icc_target);
  } else {
    // Normal handling without ICC: use the source video color space
    if (colorspace) {
      SDL_DisplayID display_id = SDL_GetDisplayForWindow(window_);
      SDL_PropertiesID prop_id = SDL_GetDisplayProperties(display_id);
      bool support_hdr = SDL_GetBooleanProperty(prop_id, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);

      bool is_hdr = pl_color_space_is_hdr(colorspace);
      static bool warned = false;
      if (is_hdr && !support_hdr) {
        if (!warned) {
          Logger->warn("HDR content on non-HDR display detected. Use HDR display or add --no-colorspace-hint to "
                       "enable tone mapping");
          warned = true;
        }
      } else {
        warned = false;
      }
    }

    static bool hint_logged_no_icc = false;
    if (!hint_logged_no_icc) {
      if (colorspace) {
        Logger->debug("colorspace hint (no ICC): primaries={}, transfer={}",
                      pl_color_primaries_name(colorspace->primaries), pl_color_transfer_name(colorspace->transfer));
      } else {
        Logger->debug("colorspace hint: nullptr (no hint)");
      }
      hint_logged_no_icc = true;
    }
    pl_swapchain_colorspace_hint(swapchain_, colorspace);
  }
}

void Window::RecalibrateICCParams(const struct pl_color_space *target_color) {
  if (!global_icc_object_ && per_video_icc_objects_.empty())
    return;

  // Similar to plplay's render_frame approach:
  // Compute max_luma from the swapchain's final target color space,
  // then use pl_icc_update to recalibrate the ICC LUT (libplacebo skips if params unchanged)
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
    if (!pl_icc_update(log_, &global_icc_object_, nullptr, &update_params)) {
      Logger->error("Failed to recalibrate global ICC object");
    }
  }

  for (auto &[video_id, icc_obj] : per_video_icc_objects_) {
    if (icc_obj) {
      struct pl_icc_params update_params = icc_obj->params;
      update_params.max_luma = target_luma;
      if (!pl_icc_update(log_, &icc_obj, nullptr, &update_params)) {
        Logger->error("Failed to recalibrate ICC object for video {}", video_id);
      }
    }
  }
}

void Window::LoadICCProfile(const std::string &path, std::optional<int> video_id) {
  ICCProfileInfo profile_info;

  if (path == "auto") {
    profile_info = GetSystemDisplayICCProfile(window_);
    if (profile_info.data.empty()) {
      Logger->warn("Failed to get system display ICC profile: {}", profile_info.source);
      return;
    }
    // Parse ICC header
    ParseICCProfileHeader(profile_info);
    Logger->debug("Loaded auto ICC profile: '{}' from {}", profile_info.name, profile_info.source);
    Logger->debug("  Version: {}.{}, Color Space: {}, PCS: {}, Header Intent: {}, Size: {} bytes",
                  profile_info.version_major, profile_info.version_minor, profile_info.color_space,
                  profile_info.connection_space, profile_info.header_intent, profile_info.data.size());
  } else {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      throw std::runtime_error(fmt::format("Failed to open ICC profile: {}", path));
    }
    auto size = file.tellg();
    if (size <= 0) {
      throw std::runtime_error(fmt::format("ICC profile is empty: {}", path));
    }
    profile_info.data.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(profile_info.data.data()), size);
    profile_info.source = path;

    // Parse ICC header info
    ParseICCProfileHeader(profile_info);

    Logger->debug("Loaded ICC profile from '{}'", path);
    Logger->debug("  Version: {}.{}, Color Space: {}, PCS: {}, Header Intent: {}, Size: {} bytes",
                  profile_info.version_major, profile_info.version_minor, profile_info.color_space,
                  profile_info.connection_space, profile_info.header_intent, profile_info.data.size());
  }

  // Build pl_icc_profile for pl_icc_open
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
    per_video_icc_objects_[id] = pl_icc_open(log_, &pl_profile, nullptr);
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
    global_icc_object_ = pl_icc_open(log_, &pl_profile, nullptr);
    if (!global_icc_object_) {
      Logger->error("Failed to open ICC profile globally");
      return;
    }
    Logger->debug("ICC profile assigned globally: {} ({} bytes, {} → {}), gamma={:.3f}, primaries={}",
                  profile_info.name, global_icc_data_.size(), profile_info.color_space, profile_info.connection_space,
                  global_icc_object_->gamma, pl_color_primaries_name(global_icc_object_->containing_primaries));
  }
}

pl_icc_object Window::GetICCObjectForVideo(int video_id) const {
  auto it = per_video_icc_objects_.find(video_id);
  if (it != per_video_icc_objects_.end())
    return it->second;
  if (global_icc_object_)
    return global_icc_object_;
  return nullptr;
}
