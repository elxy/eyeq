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

#include "platform_display.h"
#include "platform_icc.h"

using namespace EYEQ;

Window::Window(const std::string &title, int video_width, int video_height, LoggingLevel log_level,
               bool colorspace_hint, bool high_dpi, SDL_DisplayID display_id, bool full_screen)
    : colorspace_hint_(colorspace_hint), high_dpi_(high_dpi), full_screen_(full_screen), log_level_(log_level) {
  try {
    display_id = display_id ? display_id : Window::CurrentDisplay();
    InitWindow(title, video_width, video_height, display_id);
    CreateVulkanContext();

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
  } catch (...) {
    // The destructor is not called for a throwing constructor; release the
    // partially created graphics objects and the SDL window here.
    DestroyGraphics();
    if (window_) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    throw;
  }
}

Window::~Window() {
  DestroyGraphics();
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

void Window::DestroyGraphics() {
  // Target/mapping resources and other GPU objects first, so that the Vulkan
  // device (owned by the context) is destroyed only after them. The coordinator
  // that owns the FrameRenderer is destroyed by the caller before this window.
  if (render_save_tex_) {
    if (context_) {
      pl_tex_destroy(context_->gpu(), &render_save_tex_);
    } else {
      render_save_tex_ = nullptr;
    }
  }
  if (render_save_dp_) {
    pl_dispatch_destroy(&render_save_dp_);
  }

  // The swapchain must go before the surface and the device.
  if (swapchain_) {
    pl_swapchain_destroy(&swapchain_);
  }

  // The surface must go before the instance, which the context destroys.
  if (vk_surface_ && context_) {
    VkInstance instance = context_->instance();
    PFN_vkGetInstanceProcAddr get_proc_addr = context_->get_proc_addr();
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR =
        get_proc_addr ? (PFN_vkDestroySurfaceKHR)get_proc_addr(instance, "vkDestroySurfaceKHR") : nullptr;
    if (vkDestroySurfaceKHR) {
      vkDestroySurfaceKHR(instance, vk_surface_, nullptr);
    }
    vk_surface_ = VK_NULL_HANDLE;
  }

  // Releases the FFmpeg hw_device wrapper, the Vulkan device, the instance and
  // the log in that order.
  context_.reset();
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

void Window::Shutdown() {
  DestroyGraphics();
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

void Window::SetFullscreen(bool enabled) {
  if (IsFullscreen() == enabled) {
    return;
  }
  if (!SDL_SetWindowFullscreen(window_, enabled)) {
    Logger->error("Failed to {} fullscreen: {}", enabled ? "enter" : "leave", SDL_GetError());
    return;
  }
  if (!enabled) {
    // Wait for the fullscreen transition to settle, then restore the original windowed size
    if (!SDL_SyncWindow(window_)) {
      Logger->warn("Failed to sync window after leaving fullscreen: {}", SDL_GetError());
    }
    Reset();
  }
  Logger->debug("Fullscreen {}", enabled ? "enabled" : "disabled");
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
  if (full_screen_)
    flags |= SDL_WINDOW_FULLSCREEN;

  // Adjust display area based on the display device
  const SDL_DisplayMode *display = SDL_GetCurrentDisplayMode(display_id);
  SDL_PropertiesID prop_id = SDL_GetDisplayProperties(display_id);
  bool hdr = SDL_GetBooleanProperty(prop_id, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
  Logger->debug("display {}, resolution {}x{}, pixel density {}, refresh rate {}, HDR {}",
                SDL_GetDisplayName(display_id), display->w, display->h, display->pixel_density, display->refresh_rate,
                hdr ? "enabled" : "disabled");

  int window_width = video_width;
  int window_height = video_height;
  if (full_screen_) {
    // Fullscreen runs at the desktop resolution; SDL ignores the requested size
    window_width = display->w;
    window_height = display->h;
  } else {
    if (high_dpi_) {
      window_width = video_width / display->pixel_density;
      window_height = video_height / display->pixel_density;
    }
    if (window_width > display->w || window_height > display->h) {
      Logger->warn("Window size is too large, scaling down to fit display area");
      Window::FitResolutionWithAspectRatio(window_width, window_height, static_cast<float>(video_width) / video_height,
                                           display->w, display->h);
    }
  }

  window_ = SDL_CreateWindow(title.c_str(), window_width, window_height, flags);
  if (!window_) {
    throw std::runtime_error(fmt::format("Failed to create window: {}", SDL_GetError()));
  }

  if (full_screen_) {
    Logger->info("Starting in fullscreen mode on display {}", SDL_GetDisplayName(display_id));
  } else {
    MoveWindowToCenter(display_id);
  }
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
  // The SDL window system integration requires its instance extensions to be
  // enabled; the surface can only be created once the instance exists, and the
  // device must be created after the surface so it can present to it.
  unsigned num_ext = 0;
  char const *const *ext = SDL_Vulkan_GetInstanceExtensions(&num_ext);
  if (nullptr == ext) {
    throw std::runtime_error(fmt::format("Failed to get vulkan extensions: {}", SDL_GetError()));
  }
  PFN_vkGetInstanceProcAddr get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();

  context_ = std::make_unique<RenderContext>(log_level_, get_proc_addr, ext, num_ext);

  if (!SDL_Vulkan_CreateSurface(window_, context_->instance(), NULL, &vk_surface_)) {
    throw std::runtime_error("Failed to create vulkan surface");
  }

  context_->CreatePresentDevice(vk_surface_);

  struct pl_vulkan_swapchain_params swapchain_params = {
      .surface = vk_surface_,
      .present_mode = VK_PRESENT_MODE_FIFO_KHR,
      .allow_suboptimal = false,
  };
  swapchain_ = pl_vulkan_create_swapchain(context_->vulkan(), &swapchain_params);
  if (!swapchain_) {
    throw std::runtime_error("Failed to create swapchain");
  }
}

void Window::ColorspaceHint(const struct pl_color_space *colorspace) {
  // The coordinator has already resolved the ICC policy: in monitoring mode it
  // never hands over an ICC target, and in the plain desktop it hands over the
  // ICC target when a global profile is active. This function only applies the
  // hint, the HDR-on-SDR warning and the EDR reference-mode report.
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
    if (is_hdr && support_hdr && display_id != edr_checked_display_) {
      CheckDisplayEDR();
      edr_checked_display_ = display_id;
    }
  }

  static bool hint_logged = false;
  if (!hint_logged) {
    if (colorspace) {
      Logger->debug("colorspace hint: primaries={}, transfer={}", pl_color_primaries_name(colorspace->primaries),
                    pl_color_transfer_name(colorspace->transfer));
    } else {
      Logger->debug("colorspace hint: nullptr (no hint)");
    }
    hint_logged = true;
  }
  pl_swapchain_colorspace_hint(swapchain_, colorspace);
}

void Window::CheckDisplayEDR() {
  DisplayEDRInfo info = QueryDisplayEDR(window_);
  if (!info.valid) {
    Logger->debug("display EDR query unavailable: {}", info.error);
    return;
  }
  Logger->debug("display {} EDR headroom: potential {:.1f}, reference {:.1f} (0 = not a reference mode)",
                info.display_name, info.potential_headroom, info.reference_headroom);
  if (info.preset_valid) {
    Logger->debug("display {} preset: {}, HDR peak {:.0f} nits, SDR white {:.0f} nits (private MonitorPanel)",
                  info.display_name, info.preset_name, info.preset_peak_nits, info.preset_sdr_white_nits);
  } else {
    Logger->debug("display {} preset unavailable: {}", info.display_name, info.preset_error);
  }

  // Outside a reference mode macOS scales HDR output to the current SDR brightness
  // and tone-maps whatever exceeds the current headroom, so PQ code values are not
  // shown at their absolute luminance.
  if (info.reference_headroom <= 0.0f) {
    Logger->warn("Display {} is not in a reference mode: macOS shows HDR relative to the SDR brightness and may "
                 "tone-map highlights, so PQ is not at absolute luminance. Select a reference preset such as "
                 "'HDR Video (P3-ST 2084)' for absolute PQ",
                 info.display_name);
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
    Logger->debug("Loaded auto ICC profile: '{}' from {}", profile_info.name, profile_info.source);
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
  }

  // The window only resolves the raw profile bytes (including "auto"); the
  // coordinator owns the ICC policy (monitoring mode disables all application
  // ICC) and the renderer owns the ICC objects.
  if (!coordinator_) {
    throw std::logic_error("Window::LoadICCProfile called before SetCoordinator");
  }
  coordinator_->LoadICCProfile(std::move(profile_info.data), profile_info.source, video_id);
}
