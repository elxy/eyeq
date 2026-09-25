#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_vulkan.h>
}
#include <SDL3/SDL_vulkan.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/icc.h>

#include "display_coordinator.hpp"
#include "log.hpp"
#include "render.hpp"
#include "render_context.hpp"

namespace EYEQ {

enum class HighDpiMode { Auto, Yes, No };

class Window {
  /*
   * Renders frames according to DisplayMode, also supports:
   *   video frame rendering, panning, zooming, reference video switching, and text display
   *
   * The window owns the SDL window, its Vulkan surface and the swapchain and
   * drives presentation; the shared GPU context (RenderContext) and the
   * target-related rendering resources (FrameRenderer) are delegated. It also
   * keeps the desktop-only concerns: swapchain colorspace hints, ICC
   * auto-discovery from the system display and the Ctrl+E screenshot path.
   */
public:
  Window(const std::string &title, int video_width, int video_height, LoggingLevel log_level,
         bool colorspace_hint_ = true, bool high_dpi = false, SDL_DisplayID display_id = 0, bool full_screen = false);
  ~Window();

  static SDL_DisplayID CurrentDisplay();
  static void GetDisplayResolution(int &display_w, int &display_h, SDL_DisplayID display_id, bool high_dpi);
  static float GetDisplayPixelDensity(SDL_DisplayID display_id, bool high_dpi);
  float GetDisplayPixelDensity();

  // Installs the coordination layer that owns the single FrameRenderer. The
  // window becomes a presenter: it never consumes AVFrame and never runs the
  // source mapping or the scene comparison itself.
  void SetCoordinator(DisplayCoordinator *coordinator) { coordinator_ = coordinator; }
  RenderContext &context() { return *context_; }
  // Presents a color space hint to the swapchain on behalf of the coordinator
  // (the coordinator resolves the ICC policy before calling this).
  void PresenterColorspaceHint(const struct pl_color_space *csp) { ColorspaceHint(csp); }

  int Width() const {
    int width, height;
    SDL_GetWindowSize(window_, &width, &height);
    return width;
  }
  int Height() const {
    int width, height;
    SDL_GetWindowSize(window_, &width, &height);
    return height;
  }
  int PixelWidth() const {
    int width, height;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return width;
  }
  int PixelHeight() const {
    int width, height;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return height;
  }
  void Reset();
  bool OnResized();

  bool IsFullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }
  /**
   * @brief Enter or leave fullscreen at runtime
   *
   * Leaving fullscreen restores the original windowed size and centers the window on the
   * display it was initially created on.
   */
  void SetFullscreen(bool enabled);

  void SetTitle(const std::string &title) { SDL_SetWindowTitle(window_, title.c_str()); }
  void Raise() { SDL_RaiseWindow(window_); }
  bool HasInputFocus() const { return SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS; }

  /**
   * @brief Request saving the next rendered frame (final display image) to the given path
   *
   * The actual readback and encoding happens inside Render(); the request is consumed by the
   * next Render() call.
   */
  void RequestSaveRenderedFrame(const std::filesystem::path &path) { pending_render_save_ = path; }

  /**
   * @brief Load ICC profile
   *
   * @param path ICC file path, or "auto" to auto-detect system display ICC
   * @param video_id Specific video ID; nullopt means apply globally
   */
  void LoadICCProfile(const std::string &path, std::optional<int> video_id = std::nullopt);

  // Present the common result owned by the coordinator into the swapchain.
  void Render(const DisplayState &state);

  /**
   * @brief Calculate scaled dimensions while maintaining image aspect ratio within the display area
   *
   * @param resize_width Resulting scaled width
   * @param resize_height Resulting scaled height
   * @param pic_ratio Image aspect ratio
   * @param display_width Display area width
   * @param display_height Display area height
   */
  static void FitResolutionWithAspectRatio(int &resize_width, int &resize_height, float pic_ratio, int display_width,
                                           int display_height);

protected:
  bool colorspace_hint_;
  bool high_dpi_;
  bool full_screen_;
  LoggingLevel log_level_;

  SDL_Window *window_ = nullptr;
  int ori_width_ = 0;
  int ori_height_ = 0;
  SDL_DisplayID ori_display_id_ = 0;

  VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
  pl_swapchain swapchain_ = nullptr;

  // Shared GPU/context lifetime. The coordinator (owned by main and destroyed
  // before this window) holds the single FrameRenderer.
  std::unique_ptr<RenderContext> context_;
  DisplayCoordinator *coordinator_ = nullptr;

  // Rendering result save (rendered frame -> DPX). Desktop-only debug path; the
  // actual frame is rendered by frame_renderer_ into this intermediate target.
  std::optional<std::filesystem::path> pending_render_save_;
  pl_tex render_save_tex_ = nullptr;     // host-readable RGBA32F intermediate target
  pl_dispatch render_save_dp_ = nullptr; // pass-through shader to blit intermediate -> swapchain

  // Serializes the swapchain start/present/submit against the colorspace-hint
  // callback. The coordinator has its own mutex for FeedFrames/RenderScene.
  std::mutex render_mutex_;

  void InitWindow(const std::string &title, int video_width, int video_height, SDL_DisplayID display_id);
  void MoveWindowToCenter(SDL_DisplayID display_id);
  void CreateVulkanContext();
  void DestroyGraphics();

  /**
   * @brief Inform the swapchain using the frame's color space information
   *
   * @param frame Frame
   */
  void ColorspaceHint(const struct pl_color_space *colorspace);
};

}; // namespace EYEQ
