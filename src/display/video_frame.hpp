#pragma once

#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

namespace EYEQ {

class VideoFrame {
public:
  /**
   * @brief Converts AVFrame to pl_frame
   *
   * Uploads the frame to GPU textures. Note that each successful update
   * re-creates the backing textures for the mapped planes; the previous mapping
   * is kept alive until the new one is committed, so a failed update leaves the
   * old frame renderable.
   */
  VideoFrame(std::shared_ptr<AVFrame> frame, pl_gpu gpu, AVBufferRef *hw_device_ref);
  ~VideoFrame();

  void UpdateAVFrame(std::shared_ptr<AVFrame> frame, AVBufferRef *hw_device_ref);
  struct pl_frame *GetPLFrame() { return &pl_frame_; }
  const struct pl_frame *GetPLFrame() const { return &pl_frame_; }

private:
  // Caller-owned source frame. It keeps the pixel buffers reachable, but the
  // clone below means the buffers are in fact shared by reference count.
  std::shared_ptr<AVFrame> frame_;

  // Private frame header that map/transfer operate on. The FFmpeg helpers move
  // their result into the frame they are handed, so mutating the caller's
  // AVFrame would corrupt the input; this clone absorbs those mutations.
  AVFrame *mapped_frame_ = nullptr;

  pl_gpu gpu_;
  pl_tex tex_[PL_MAX_PLANES];

  AVBufferRef *hw_frame_ref_;
  AVHWFramesConstraints *constraints_;
  enum AVPixelFormat *transfer_formats_;

  struct pl_frame pl_frame_{};

  // Releases every resource owned by this instance. Idempotent, so it can be
  // used both by the destructor and by the constructor's failure path (a
  // throwing constructor does not run the destructor).
  void ReleaseResources();

  void GeneratePLFrame(AVBufferRef *hw_device_ref);

  int create_hw_frame(AVBufferRef *hw_device_ref, AVFrame *frame);
  int move_to_output_frame(AVFrame *frame, AVFrame *tmp_frame);
  int map_frame(AVFrame *frame, int use_hw_frame);
  int check_hw_transfer(AVFrame *frame);
  int transfer_frame(AVFrame *frame, int use_hw_frame);
};

}; // namespace EYEQ
