#include "render_context.hpp"

#include <stdexcept>
#include <vector>

#include <spdlog/fmt/fmt.h>

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}

#include "log.hpp"

using namespace EYEQ;

// Kept in sync with optional_device_exts inside FFmpeg's hwcontext_vulkan.c.
extern const std::vector<const char *> optional_device_exts;

RenderContext::RenderContext(LoggingLevel log_level, PFN_vkGetInstanceProcAddr get_proc_addr,
                             const char *const *instance_extensions, unsigned num_instance_extensions)
    : get_proc_addr_(get_proc_addr) {
  log_ = create_pl_log(log_level);
  if (!log_) {
    throw std::runtime_error("Failed to create placebo log");
  }

  struct pl_vk_inst_params inst_params = {
      .debug = log_->params.log_level >= PL_LOG_DEBUG,
      .get_proc_addr = get_proc_addr_,
      .extensions = instance_extensions,
      .num_extensions = static_cast<int>(num_instance_extensions),
  };
  pl_inst_ = pl_vk_inst_create(log_, &inst_params);
  if (!pl_inst_) {
    // The constructor threw, so the destructor will not run; release the log here.
    pl_log_destroy(&log_);
    throw std::runtime_error("Failed to create placebo instance");
  }
  vk_inst_ = pl_inst_->instance;
}

RenderContext::~RenderContext() {
  // The FFmpeg hw_device wrapper is released before the Vulkan device. Every
  // frame or hwframe that referenced it must already be gone, otherwise the
  // device could be destroyed while those references are still live.
  av_buffer_unref(&hw_device_ref_);

  if (vulkan_) {
    pl_vulkan_destroy(&vulkan_);
  }
  if (pl_inst_) {
    pl_vk_inst_destroy(&pl_inst_);
  }
  vk_inst_ = VK_NULL_HANDLE;

  if (log_) {
    pl_log_destroy(&log_);
  }
}

void RenderContext::CreatePresentDevice(VkSurfaceKHR surface) {
  if (surface == VK_NULL_HANDLE) {
    throw std::invalid_argument("CreatePresentDevice requires a valid VkSurfaceKHR");
  }
  CreateDevice(surface);
}

void RenderContext::CreateHeadlessDevice() { CreateDevice(VK_NULL_HANDLE); }

void RenderContext::CreateDevice(VkSurfaceKHR surface) {
  if (device_attempted_) {
    throw std::logic_error("RenderContext device creation was already attempted");
  }
  device_attempted_ = true;
  if (!log_ || !pl_inst_) {
    throw std::logic_error("RenderContext device creation before instance creation");
  }

  struct pl_vulkan_params vk_params = {
      .instance = vk_inst_,
      .get_proc_addr = get_proc_addr_,
      .surface = surface,
      .allow_software = false,
      .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
      .opt_extensions = optional_device_exts.data(),
      .num_opt_extensions = static_cast<int>(optional_device_exts.size()),
  };
  vulkan_ = pl_vulkan_create(log_, &vk_params);
  if (!vulkan_) {
    throw std::runtime_error("Failed to create libplacebo vulkan device");
  }

  CreateHWDevice();
}

void RenderContext::CreateHWDevice() {
  hw_device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if (!hw_device_ref_) {
    // libavutil was built without Vulkan support; the GPU path still works, but
    // hardware frames cannot be transferred through FFmpeg.
    Logger->debug("linked libavutil was not built with vulkan support");
    return;
  }

  AVHWDeviceContext *device_ctx = (AVHWDeviceContext *)hw_device_ref_->data;
  device_ctx->user_opaque = this;

  AVVulkanDeviceContext *vk_dev_ctx = (AVVulkanDeviceContext *)device_ctx->hwctx;
  // FFmpeg and libplacebo submit to the same queues, so FFmpeg must share
  // libplacebo's queue lock. On libavutil < 62 these deprecated callbacks are
  // the only way to do that; the replacement,
  // VK_KHR_internally_synchronized_queues, is not exposed by libplacebo 7.x or
  // MoltenVK. Revisit when libplacebo supports it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  vk_dev_ctx->lock_queue = RenderContext::HWCallbackLockQueue;
  vk_dev_ctx->unlock_queue = RenderContext::HWCallbackUnlockQueue;
#pragma GCC diagnostic pop

  vk_dev_ctx->get_proc_addr = pl_inst_->get_proc_addr;
  vk_dev_ctx->inst = pl_inst_->instance;
  vk_dev_ctx->phys_dev = vulkan_->phys_device;
  vk_dev_ctx->act_dev = vulkan_->device;

  vk_dev_ctx->device_features = *(vulkan_->features);

  vk_dev_ctx->enabled_inst_extensions = pl_inst_->extensions;
  vk_dev_ctx->nb_enabled_inst_extensions = pl_inst_->num_extensions;

  vk_dev_ctx->enabled_dev_extensions = vulkan_->extensions;
  vk_dev_ctx->nb_enabled_dev_extensions = vulkan_->num_extensions;

  // FFmpeg only uses the queue families listed here; with none listed, its
  // frames init dereferences a missing transfer queue and crashes. Expose the
  // queues libplacebo created (same layout as fftools/ffplay_renderer.c).
  int nb_qf = 0;
  auto add_qf = [&](uint32_t idx, uint32_t num, VkQueueFlagBits flags) {
    vk_dev_ctx->qf[nb_qf++] = AVVulkanDeviceQueueFamily{static_cast<int>(idx), static_cast<int>(num), flags};
  };
  add_qf(vulkan_->queue_graphics.index, vulkan_->queue_graphics.count, VK_QUEUE_GRAPHICS_BIT);
  add_qf(vulkan_->queue_transfer.index, vulkan_->queue_transfer.count, VK_QUEUE_TRANSFER_BIT);
  add_qf(vulkan_->queue_compute.index, vulkan_->queue_compute.count, VK_QUEUE_COMPUTE_BIT);
  // libplacebo created the decode queue via extra_queues when the device has
  // one (MoltenVK has none). FFmpeg indexes per-family arrays with idx, so only
  // list it when it exists.
  auto get_qf_props = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      pl_inst_->get_proc_addr(pl_inst_->instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  if (!get_qf_props) {
    throw std::runtime_error("vkGetPhysicalDeviceQueueFamilyProperties not available");
  }
  uint32_t num_props = 0;
  get_qf_props(vulkan_->phys_device, &num_props, nullptr);
  std::vector<VkQueueFamilyProperties> props(num_props);
  get_qf_props(vulkan_->phys_device, &num_props, props.data());
  for (uint32_t i = 0; i < num_props; i++) {
    if (props[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
      add_qf(i, props[i].queueCount, VK_QUEUE_VIDEO_DECODE_BIT_KHR);
      break;
    }
  }
  vk_dev_ctx->nb_qf = nb_qf;

  if (av_hwdevice_ctx_init(hw_device_ref_) < 0) {
    av_buffer_unref(&hw_device_ref_);
    throw std::runtime_error("Failed to initialize hwdevice context");
  }
}

void RenderContext::LockQueue(uint32_t queue_family, uint32_t index) {
  if (!vulkan_) {
    return;
  }
  vulkan_->lock_queue(vulkan_, queue_family, index);
}

void RenderContext::UnlockQueue(uint32_t queue_family, uint32_t index) {
  if (!vulkan_) {
    return;
  }
  vulkan_->unlock_queue(vulkan_, queue_family, index);
}
