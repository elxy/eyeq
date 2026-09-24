#pragma once

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>

#include "log.hpp"

namespace EYEQ {

/*
 * Owns the GPU lifetime that is shared by the window present path and by
 * offscreen render targets: the placebo log, the Vulkan instance, the
 * libplacebo Vulkan device and the FFmpeg Vulkan hw_device. It does not own
 * any SDL window, surface, swapchain or render target.
 *
 * Initialization is explicitly two-stage so that the window can create its
 * surface between the instance and the device:
 *   1. Construction creates the log and the Vulkan instance.
 *   2. CreatePresentDevice(surface) or CreateHeadlessDevice() creates the
 *      device and the FFmpeg hw_device. Exactly one of them must be called
 *      once; calling either again throws.
 *
 * The hw_device queue-lock callbacks point at this object rather than at the
 * window. The object therefore has to stay at a stable address for its whole
 * lifetime, which the deleted copy/move operations enforce.
 *
 * Destruction order: every pl_tex/pl_dispatch/renderer and every FFmpeg frame
 * or hwframe that references this context must be released first. The owner
 * (Window or a test) must also destroy its swapchain and surface before
 * destroying the context, because the Vulkan instance is destroyed here and
 * the surface has to go before the instance. The context itself then releases
 * the FFmpeg hw_device wrapper, the Vulkan device, the Vulkan instance and the
 * log, in that order.
 */
class RenderContext {
public:
  RenderContext(LoggingLevel log_level, PFN_vkGetInstanceProcAddr get_proc_addr, const char *const *instance_extensions,
                unsigned num_instance_extensions);
  ~RenderContext();

  RenderContext(const RenderContext &) = delete;
  RenderContext &operator=(const RenderContext &) = delete;
  RenderContext(RenderContext &&) = delete;
  RenderContext &operator=(RenderContext &&) = delete;

  /*
   * Second initialization stage for the window present path. @p surface must
   * belong to this context's VkInstance. The device and its queues are chosen
   * so that presenting to @p surface is supported.
   */
  void CreatePresentDevice(VkSurfaceKHR surface);
  /*
   * Second initialization stage for offscreen targets. Uses the same instance
   * and device setup as the present path, only without a surface.
   */
  void CreateHeadlessDevice();

  bool HasDevice() const { return vulkan_ != nullptr; }

  pl_log log() const { return log_; }
  pl_vulkan vulkan() const { return vulkan_; }
  pl_gpu gpu() const { return vulkan_ ? vulkan_->gpu : nullptr; }
  AVBufferRef *hw_device() const { return hw_device_ref_; }
  VkInstance instance() const { return vk_inst_; }
  PFN_vkGetInstanceProcAddr get_proc_addr() const { return get_proc_addr_; }

  void LockQueue(uint32_t queue_family, uint32_t index);
  void UnlockQueue(uint32_t queue_family, uint32_t index);

private:
  void CreateDevice(VkSurfaceKHR surface);
  void CreateHWDevice();

  static void HWCallbackLockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
    static_cast<RenderContext *>(dev_ctx->user_opaque)->LockQueue(queue_family, index);
  }
  static void HWCallbackUnlockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
    static_cast<RenderContext *>(dev_ctx->user_opaque)->UnlockQueue(queue_family, index);
  }

  pl_log log_ = nullptr;
  PFN_vkGetInstanceProcAddr get_proc_addr_ = nullptr;
  pl_vk_inst pl_inst_ = nullptr;
  VkInstance vk_inst_ = VK_NULL_HANDLE;
  pl_vulkan vulkan_ = nullptr;
  AVBufferRef *hw_device_ref_ = nullptr;

  // Set as soon as device creation is attempted, successfully or not, so a
  // second stage-two call cannot silently rebuild the device while resources
  // from the first attempt are still alive.
  bool device_attempted_ = false;
};

}; // namespace EYEQ
