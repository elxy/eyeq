#include "video_frame.hpp"

#include <cassert>

#include <spdlog/spdlog.h>
extern "C" {
#include <libavutil/bprint.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/swapchain.h>

#include "log.hpp"
#include "utils.hpp"

using namespace EYEQ;

VideoFrame::VideoFrame(std::shared_ptr<AVFrame> frame, pl_gpu gpu, AVBufferRef *hw_device_ref) {
  frame_ = frame;

  gpu_ = gpu;
  hw_frame_ref_ = nullptr;

  constraints_ = nullptr;
  transfer_formats_ = nullptr;

  for (int i = 0; i < PL_MAX_PLANES; i++) {
    tex_[i] = {};
  }

  GeneratePLFrame(hw_device_ref);
}

VideoFrame::~VideoFrame() {

  av_freep(&transfer_formats_);
  av_hwframe_constraints_free(&constraints_);
  av_buffer_unref(&hw_frame_ref_);

  for (int i = 0; i < PL_MAX_PLANES; i++)
    pl_tex_destroy(gpu_, &(tex_[i]));

  if (pl_frame_.user_data) {
    pl_unmap_avframe(gpu_, &pl_frame_);
  }

  frame_.reset();
}

void VideoFrame::UpdateAVFrame(std::shared_ptr<AVFrame> frame, AVBufferRef *hw_device_ref) {
  // Calling av_frame_unref(pl_frame_.user_data.avframe): hardware frame textures are destroyed, software frame textures are not
  pl_unmap_avframe(gpu_, &pl_frame_);

  frame_ = frame;
  GeneratePLFrame(hw_device_ref);
}

void VideoFrame::GeneratePLFrame(AVBufferRef *hw_device_ref) {
  int use_hw;
  int ret;

  if (!frame_->hw_frames_ctx) {
    SPDLOG_LOGGER_TRACE(Logger, "frame {} (pts {}) is not hardware frame", fmt::ptr(frame_.get()), frame_->pts);
    goto convert;
  }
  SPDLOG_LOGGER_TRACE(Logger, "frame {} (pts {}) is hardware frame", fmt::ptr(frame_.get()), frame_->pts);

  if (frame_->format == AV_PIX_FMT_VULKAN) {
    SPDLOG_LOGGER_TRACE(Logger, "frame {} (pts {}) is vulkan frame", fmt::ptr(frame_.get()), frame_->pts);
    goto convert;
  }

  // Need to transfer non-Vulkan hardware frames to Vulkan or CPU
  if (!hw_device_ref) {
    use_hw = 0;
  } else {
    use_hw = 1;
    ret = create_hw_frame(hw_device_ref, frame_.get());
    if (ret < 0) {
      throw std::runtime_error(fmt::format("Failed to create hwframe, {}: {}", ret, ffmpeg_error_string(ret)));
    }
  }

  for (; use_hw >= 0; use_hw--) {
    ret = map_frame(frame_.get(), use_hw);
    if (!ret)
      goto convert;

    ret = transfer_frame(frame_.get(), use_hw);
    if (!ret)
      goto convert;
  }

convert:
  struct pl_avframe_params params = {.frame = frame_.get(), .tex = tex_, .map_dovi = true};
  // pl_map_avframe_ex clones the frame via av_frame_clone. tex stores textures for non-hardware frames
  if (!pl_map_avframe_ex(gpu_, &pl_frame_, &params)) {
    throw std::runtime_error("pl_map_avframe_ex() failed");
  }
  Logger->trace("convert avframe from format {} to pl_frame texture {}",
                av_get_pix_fmt_name(static_cast<enum AVPixelFormat>(frame_->format)), tex_[0]->params.format->name);
  AVStream * stream = (AVStream *)frame_->opaque;
  pl_frame_copy_stream_props(&pl_frame_, stream);
}

int VideoFrame::create_hw_frame(AVBufferRef *hw_device_ref, AVFrame *frame) {
  AVHWFramesContext *src_hw_frame = (AVHWFramesContext *)frame->hw_frames_ctx->data;
  AVHWFramesContext *hw_frame;
  int ret;

  if (hw_frame_ref_) {
    hw_frame = (AVHWFramesContext *)hw_frame_ref_->data;

    if (hw_frame->width == frame->width && hw_frame->height == frame->height &&
        hw_frame->sw_format == src_hw_frame->sw_format)
      return 0;

    av_buffer_unref(&hw_frame_ref_);
  }

  if (!constraints_) {
    constraints_ = av_hwdevice_get_hwframe_constraints(hw_device_ref, nullptr);
    if (!constraints_)
      return AVERROR(ENOMEM);
  }

  // Check constraints and skip create hwframe. Don't take it as error since
  // we can fallback to memory copy from GPU to CPU.
  if ((constraints_->max_width && constraints_->max_width < frame->width) ||
      (constraints_->max_height && constraints_->max_height < frame->height) ||
      (constraints_->min_width && constraints_->min_width > frame->width) ||
      (constraints_->min_height && constraints_->min_height > frame->height))
    return 0;

  if (constraints_->valid_sw_formats) {
    enum AVPixelFormat *sw_formats = constraints_->valid_sw_formats;
    while (*sw_formats != AV_PIX_FMT_NONE) {
      if (*sw_formats == src_hw_frame->sw_format)
        break;
      sw_formats++;
    }
    if (*sw_formats == AV_PIX_FMT_NONE)
      return 0;
  }

  hw_frame_ref_ = av_hwframe_ctx_alloc(hw_device_ref);
  if (!hw_frame_ref_)
    return AVERROR(ENOMEM);

  hw_frame = (AVHWFramesContext *)hw_frame_ref_->data;
  hw_frame->format = AV_PIX_FMT_VULKAN;
  hw_frame->sw_format = src_hw_frame->sw_format;
  hw_frame->width = frame->width;
  hw_frame->height = frame->height;

  if (frame->format == AV_PIX_FMT_CUDA) {
    AVVulkanFramesContext *vk_frame_ctx = (AVVulkanFramesContext *)hw_frame->hwctx;
    vk_frame_ctx->flags = AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
  }

  ret = av_hwframe_ctx_init(hw_frame_ref_);
  if (ret < 0) {
    Logger->error("Failed to create hwframe context: {}", ffmpeg_error_string(ret));
    return ret;
  }

  av_hwframe_transfer_get_formats(hw_frame_ref_, AV_HWFRAME_TRANSFER_DIRECTION_TO, &transfer_formats_, 0);
  return 0;
}

int VideoFrame::move_to_output_frame(AVFrame *frame, AVFrame *tmp_frame) {
  int ret = av_frame_copy_props(tmp_frame, frame);
  if (ret < 0)
    return ret;
  av_frame_unref(frame);
  av_frame_move_ref(frame, tmp_frame);
  return 0;
}

int VideoFrame::map_frame(AVFrame *frame, int use_hw_frame) {
  int ret;

  if (use_hw_frame && !hw_frame_ref_)
    return AVERROR(ENOSYS);

  AVFrame *vk_frame = av_frame_alloc();
  // Try map data first
  if (use_hw_frame) {
    vk_frame->hw_frames_ctx = av_buffer_ref(hw_frame_ref_);
    vk_frame->format = AV_PIX_FMT_VULKAN;
  }
  ret = av_hwframe_map(vk_frame, frame, 0);
  if (!ret) {
    ret = move_to_output_frame(frame, vk_frame);
    goto exit;
  }

  if (ret != AVERROR(ENOSYS)) {
    std::string msg = fmt::format("Map frame failed: {}", ffmpeg_error_string(ret));
    throw std::runtime_error(msg);
  }

exit:
  av_frame_free(&vk_frame);
  return ret;
}

int VideoFrame::check_hw_transfer(AVFrame *frame) {
  if (!hw_frame_ref_ || !transfer_formats_)
    return 0;

  for (int i = 0; transfer_formats_[i] != AV_PIX_FMT_NONE; i++)
    if (transfer_formats_[i] == frame->format)
      return 1;

  return 0;
}

int VideoFrame::transfer_frame(AVFrame *frame, int use_hw_frame) {
  int ret;

  if (use_hw_frame && !check_hw_transfer(frame))
    return AVERROR(ENOSYS);

  AVFrame *vk_frame = av_frame_alloc();
  if (use_hw_frame)
    av_hwframe_get_buffer(hw_frame_ref_, vk_frame, 0);
  ret = av_hwframe_transfer_data(vk_frame, frame, 1);
  if (!ret) {
    ret = move_to_output_frame(frame, vk_frame);
    goto exit;
  }

  if (ret != AVERROR(ENOSYS)) {
    std::string msg = fmt::format("Transfer frame failed: {}", ffmpeg_error_string(ret));
    throw std::runtime_error(msg);
  }

exit:
  av_frame_free(&vk_frame);
  return ret;
}
