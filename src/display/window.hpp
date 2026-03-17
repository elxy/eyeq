#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#include <SDL3/SDL_vulkan.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/icc.h>

#include "log.hpp"
#include "render.hpp"
#include "fill_render.hpp"
#include "osd.hpp"
#include "video_frame.hpp"

namespace EYEQ {

enum class HighDpiMode { Auto, Yes, No };

class Window {
  /*
   * Renders frames according to DisplayMode, also supports:
   *   video frame rendering, panning, zooming, reference video switching, and text display
   */
public:
  Window(const std::string &title, int video_width, int video_height, LoggingLevel log_level,
         bool colorspace_hint_ = true, bool high_dpi = false, SDL_DisplayID display_id = 0);
  ~Window();

  static SDL_DisplayID CurrentDisplay();
  static void GetDisplayResolution(int &display_w, int &display_h, SDL_DisplayID display_id, bool high_dpi);
  static float GetDisplayPixelDensity(SDL_DisplayID display_id, bool high_dpi);
  float GetDisplayPixelDensity();

  void InitRender(DisplayMode display_mode, ScaleMethod &scale_method, ScaleMethod &plane_scale_method);

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

  void SetTitle(const std::string &title) { SDL_SetWindowTitle(window_, title.c_str()); }
  void Raise() { SDL_RaiseWindow(window_); }
  bool HasInputFocus() const { return SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS; }

  void SetMainSource(int id) { main_id_ = id; }

  /**
   * @brief Load ICC profile
   *
   * @param path ICC file path, or "auto" to auto-detect system display ICC
   * @param video_id Specific video ID; nullopt means apply globally
   */
  void LoadICCProfile(const std::string &path, std::optional<int> video_id = std::nullopt);

  /**
   * @brief Dynamically recalibrate ICC parameters based on target color space
   *
   * Similar to libplacebo plplay's approach: per frame, compute max_luma from
   * the swapchain's final target color space, then call pl_icc_update to recalibrate the LUT.
   * Processes both global and per-video ICC objects.
   */
  void RecalibrateICCParams(const struct pl_color_space *target_color);

  /**
   * @brief Get the ICC object for a specific video
   *
   * Returns per-video ICC first, then global ICC; returns nullptr if none
   */
  pl_icc_object GetICCObjectForVideo(int video_id) const;
  /**
   * @brief Feed frames to the renderer for rendering
   *
   * @param frames Video frames
   */
  void FeedFrames(std::map<int, std::shared_ptr<AVFrame>> &frames);
  void Render(const DisplayState &state);

  OsdManager &GetOsdManager() { return *osd_manager_; }

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

  void PlaceboLockQueue(uint32_t queue_family, uint32_t index) { vulkan_->lock_queue(vulkan_, queue_family, index); }
  void PlaceboUnlockQueue(uint32_t queue_family, uint32_t index) {
    vulkan_->unlock_queue(vulkan_, queue_family, index);
  }

protected:
  DisplayMode mode_;
  bool colorspace_hint_;
  bool high_dpi_;

  SDL_Window *window_;
  int ori_width_;
  int ori_height_;
  SDL_DisplayID ori_display_id_;

  AVBufferRef *hw_device_ref_;

  PFN_vkGetInstanceProcAddr get_proc_addr_;
  VkInstance vk_inst_;
  VkSurfaceKHR vk_surface_;

  pl_log log_;
  pl_vk_inst pl_inst_;
  pl_vulkan vulkan_;
  pl_swapchain swapchain_;

  struct pl_render_params render_params_;
  std::unique_ptr<FillRender> display_render_;
  std::unique_ptr<OsdManager> osd_manager_;

  int main_id_;
  std::map<int, VideoFrame> av_frames_;
  std::map<int, struct pl_frame *> pl_frames_;
  std::mutex render_mutex_;

  // ICC profile data storage
  std::vector<uint8_t> global_icc_data_;
  pl_icc_object global_icc_object_ = nullptr;
  std::map<int, std::vector<uint8_t>> per_video_icc_data_;
  std::map<int, pl_icc_object> per_video_icc_objects_;
  float last_icc_target_luma_ = -1.0f; // Last calibrated target_luma, used for deduplication

  void InitWindow(const std::string &title, int video_width, int video_height, SDL_DisplayID display_id);
  void MoveWindowToCenter(SDL_DisplayID display_id);
  void CreateVulkanContext();

  /**
   * @brief Inform the swapchain using the frame's color space information
   *
   * @param frame Frame
   */
  void ColorspaceHint(struct pl_color_space *colorspace);

private:
  static void HWCallbackLockQueue(struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
    Window *self = static_cast<Window *>(dev_ctx->user_opaque);
    self->PlaceboLockQueue(queue_family, index);
  }
  static void HWCallbackUnlockQueue(struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
    Window *self = static_cast<Window *>(dev_ctx->user_opaque);
    self->PlaceboUnlockQueue(queue_family, index);
  }
};

}; // namespace EYEQ
