#include "video_frame.hpp"

#include <cassert>
#include <stdexcept>

#include <spdlog/spdlog.h>
extern "C" {
#include <libavutil/bprint.h>
#include <libavutil/dovi_meta.h>
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

  mapped_frame_ = av_frame_alloc();
  if (!mapped_frame_) {
    throw std::runtime_error("Failed to allocate private AVFrame header");
  }

  // A throwing constructor does not run the destructor, so release anything
  // GeneratePLFrame() may already have acquired before rethrowing.
  try {
    GeneratePLFrame(hw_device_ref);
  } catch (...) {
    ReleaseResources();
    throw;
  }
}

VideoFrame::~VideoFrame() { ReleaseResources(); }

void VideoFrame::ReleaseResources() {
  // Release the mapping before the hwframe context it may reference.
  if (pl_frame_.user_data) {
    pl_unmap_avframe(gpu_, &pl_frame_);
  }
  for (int i = 0; i < PL_MAX_PLANES; i++) {
    if (tex_[i]) {
      pl_tex_destroy(gpu_, &(tex_[i]));
    }
  }

  av_frame_free(&mapped_frame_);
  av_freep(&transfer_formats_);
  av_hwframe_constraints_free(&constraints_);
  av_buffer_unref(&hw_frame_ref_);

  frame_.reset();
}

void VideoFrame::UpdateAVFrame(std::shared_ptr<AVFrame> frame, AVBufferRef *hw_device_ref) {
  // GeneratePLFrame() stages the new mapping and only replaces the previous one
  // after it succeeded, so a failed update keeps the previous frame valid
  // instead of leaving a dangling pl_frame.
  frame_ = frame;
  GeneratePLFrame(hw_device_ref);
}

void VideoFrame::GeneratePLFrame(AVBufferRef *hw_device_ref) {
  int use_hw;
  int ret;

  // Refresh the private header so the map/transfer helpers below never mutate
  // the caller's AVFrame. Pixel buffers stay shared through refcounting.
  av_frame_unref(mapped_frame_);
  ret = av_frame_ref(mapped_frame_, frame_.get());
  if (ret < 0) {
    throw std::runtime_error(fmt::format("Failed to reference AVFrame: {}", ffmpeg_error_string(ret)));
  }

  if (!mapped_frame_->hw_frames_ctx) {
    SPDLOG_LOGGER_TRACE(Logger, "frame {} (pts {}) is not hardware frame", fmt::ptr(mapped_frame_), mapped_frame_->pts);
    goto convert;
  }
  SPDLOG_LOGGER_TRACE(Logger, "frame {} (pts {}) is hardware frame", fmt::ptr(mapped_frame_), mapped_frame_->pts);

  if (mapped_frame_->format == AV_PIX_FMT_VULKAN) {
    SPDLOG_LOGGER_TRACE(Logger, "frame {} (pts {}) is vulkan frame", fmt::ptr(mapped_frame_), mapped_frame_->pts);
    goto convert;
  }

  // Need to transfer non-Vulkan hardware frames to Vulkan or CPU
  if (!hw_device_ref) {
    use_hw = 0;
  } else {
    use_hw = 1;
    ret = create_hw_frame(hw_device_ref, mapped_frame_);
    if (ret < 0) {
      throw std::runtime_error(fmt::format("Failed to create hwframe, {}: {}", ret, ffmpeg_error_string(ret)));
    }
  }

  for (; use_hw >= 0; use_hw--) {
    ret = map_frame(mapped_frame_, use_hw);
    if (!ret)
      goto convert;

    ret = transfer_frame(mapped_frame_, use_hw);
    if (!ret)
      goto convert;
  }

convert:
  // Stage the mapping into fresh textures. pl_map_avframe_ex clears the target
  // frame on failure but leaves any textures it created for earlier planes to
  // the caller, so destroy them explicitly before reporting the failure.
  struct pl_frame new_frame{};
  pl_tex new_tex[PL_MAX_PLANES] = {};
  struct pl_avframe_params params = {.frame = mapped_frame_, .tex = new_tex, .map_dovi = true};
  // pl_map_avframe_ex clones the frame via av_frame_clone. tex stores textures
  // for non-hardware frames. It may fail for two reasons:
  //   1. The pixel format has incompatible component strides (e.g. y210le), rejected silently
  //      by libplacebo's pl_plane_data_from_pixfmt() before any GPU texture lookup.
  //   2. The pixel format passes validation but has no matching GPU texture format (e.g. bgr8),
  //      which triggers libplacebo's PL_ERR("Failed picking any compatible texture format").
  // In both cases we throw texture_format_error, which is caught per-video in
  // FrameRenderer::FeedFrames(). The main.cpp frame callback then triggers an automatic
  // fallback: appending format=rgb48le to the FFmpeg filter graph and re-decoding.
  if (!pl_map_avframe_ex(gpu_, &new_frame, &params)) {
    for (int i = 0; i < PL_MAX_PLANES; i++) {
      if (new_tex[i]) {
        pl_tex_destroy(gpu_, &new_tex[i]);
      }
    }
    throw texture_format_error(fmt::format("pl_map_avframe_ex() failed for pixel format {}",
                                           av_get_pix_fmt_name(static_cast<AVPixelFormat>(mapped_frame_->format))));
  }

  // Commit: replace the previous mapping only now that the new one is valid.
  if (pl_frame_.user_data) {
    pl_unmap_avframe(gpu_, &pl_frame_);
  }
  for (int i = 0; i < PL_MAX_PLANES; i++) {
    if (tex_[i]) {
      pl_tex_destroy(gpu_, &(tex_[i]));
    }
  }
  pl_frame_ = new_frame;
  for (int i = 0; i < PL_MAX_PLANES; i++) {
    tex_[i] = new_tex[i];
  }

  Logger->trace("convert avframe from format {} to pl_frame texture {}",
                av_get_pix_fmt_name(static_cast<enum AVPixelFormat>(mapped_frame_->format)),
                tex_[0]->params.format->name);
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

namespace {

// Short names for the interpretation log; libplacebo's own names are long
const char *SystemShort(enum pl_color_system sys) {
  switch (sys) {
  case PL_COLOR_SYSTEM_BT_601:
    return "bt601";
  case PL_COLOR_SYSTEM_BT_709:
    return "bt709";
  case PL_COLOR_SYSTEM_SMPTE_240M:
    return "smpte240m";
  case PL_COLOR_SYSTEM_BT_2020_NC:
    return "bt2020nc";
  case PL_COLOR_SYSTEM_BT_2020_C:
    return "bt2020c";
  case PL_COLOR_SYSTEM_BT_2100_PQ:
    return "ictcp-pq";
  case PL_COLOR_SYSTEM_BT_2100_HLG:
    return "ictcp-hlg";
  case PL_COLOR_SYSTEM_DOLBYVISION:
    return "dolbyvision";
  case PL_COLOR_SYSTEM_YCGCO:
    return "ycgco";
  case PL_COLOR_SYSTEM_YCGCO_RE:
    return "ycgco-re";
  case PL_COLOR_SYSTEM_YCGCO_RO:
    return "ycgco-ro";
  default:
    return "unknown";
  }
}

// FFmpeg-style names for the libplacebo primaries/transfer values that have an
// FFmpeg equivalent, libplacebo's own name otherwise
std::string PrimariesShort(enum pl_color_primaries prim) {
  const char *name = av_color_primaries_name(pl_primaries_to_av(prim));
  return (prim != PL_COLOR_PRIM_UNKNOWN && pl_primaries_to_av(prim) != AVCOL_PRI_UNSPECIFIED && name)
             ? name
             : pl_color_primaries_name(prim);
}

std::string TransferShort(enum pl_color_transfer trc) {
  if (trc == PL_COLOR_TRC_BT_1886) {
    return "bt1886"; // pl_transfer_to_av() maps it back to bt709, which names the OETF
  }
  const char *name = av_color_transfer_name(pl_transfer_to_av(trc));
  return (trc != PL_COLOR_TRC_UNKNOWN && pl_transfer_to_av(trc) != AVCOL_TRC_UNSPECIFIED && name)
             ? name
             : pl_color_transfer_name(trc);
}

} // namespace

std::string EYEQ::DescribeInterpretation(const AVFrame *frame) {
  struct pl_frame out;
  pl_frame_from_avframe(&out, frame);

  std::string desc;
  AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
  if (frame->hw_frames_ctx) {
    format = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data)->sw_format;
  }
  const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(format);
  const bool gray = pix_desc && pix_desc->nb_components < 3;
  // Representation: matrix and levels
  switch (gray ? PL_COLOR_SYSTEM_COUNT : out.repr.sys) {
  case PL_COLOR_SYSTEM_COUNT:
    desc = "gray"; // libplacebo decodes it as the luma of a YCbCr system; the matrix is irrelevant
    break;
  case PL_COLOR_SYSTEM_RGB:
    desc = "RGB";
    break;
  case PL_COLOR_SYSTEM_XYZ:
    desc = "XYZ (as DCI-P3, st428)";
    break;
  default: {
    const AVFrameSideData *dovi = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    desc = fmt::format("YCbCr {}", SystemShort(out.repr.sys));
    if (dovi && !av_dovi_get_header(reinterpret_cast<const AVDOVIMetadata *>(dovi->data))->disable_residual_flag) {
      // pl_map_avframe_ex() only maps RPUs that do not need an enhancement layer
      desc += " (Dolby Vision RPU needs an enhancement layer, showing the base layer)";
    } else if (dovi) {
      desc += " (Dolby Vision RPU)";
    }
    break;
  }
  }
  const enum pl_color_levels levels = pl_color_levels_guess(&out.repr);
  desc += levels == PL_COLOR_LEVELS_LIMITED ? ", limited" : ", full";
  if (out.repr.sys == PL_COLOR_SYSTEM_RGB && frame->color_range == AVCOL_RANGE_MPEG) {
    desc += " (tv tag ignored for RGB)";
  } else if (out.repr.levels == PL_COLOR_LEVELS_UNKNOWN) {
    desc += " (default)";
  }

  // Color space: an embedded ICC profile replaces primaries and transfer in
  // the renderer (pass_fix_frames), so the tags are not used at all
  if (out.profile.len) {
    desc += fmt::format(", embedded ICC profile ({} bytes) overrides primaries/transfer", out.profile.len);
    return desc;
  }
  if (out.repr.sys == PL_COLOR_SYSTEM_XYZ) {
    return desc;
  }
  if (out.color.primaries != PL_COLOR_PRIM_UNKNOWN) {
    desc += ", " + PrimariesShort(out.color.primaries);
  } else {
    // Renderer default (fix_frame): guessed from the frame size
    desc += ", " + PrimariesShort(pl_color_primaries_guess(frame->width, frame->height)) + " (default)";
  }
  if (out.color.transfer != PL_COLOR_TRC_UNKNOWN) {
    // SDR camera OETF tags (bt709, bt2020-10, ...) deliberately become the
    // BT.1886 display EOTF, so only other substitutions are worth a note
    desc += ", " + TransferShort(out.color.transfer);
    if (out.color.transfer != PL_COLOR_TRC_BT_1886 && frame->color_trc != AVCOL_TRC_UNSPECIFIED &&
        pl_transfer_to_av(out.color.transfer) != frame->color_trc) {
      desc += fmt::format(" (from {})", av_color_transfer_name(frame->color_trc));
    }
  } else {
    // Renderer default (pl_color_space_infer)
    desc += ", " + TransferShort(PL_COLOR_TRC_BT_1886) + " (default)";
    if (frame->color_trc != AVCOL_TRC_UNSPECIFIED) {
      desc += fmt::format(", {} not supported", av_color_transfer_name(frame->color_trc));
    }
  }
  return desc;
}
