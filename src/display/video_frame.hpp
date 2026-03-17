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
   * @brief Converts AVFrame to pl_frame, retaining GPU textures for efficiency
   */
  VideoFrame(std::shared_ptr<AVFrame> frame, pl_gpu gpu, AVBufferRef *hw_device_ref);
  ~VideoFrame();

  void UpdateAVFrame(std::shared_ptr<AVFrame> frame, AVBufferRef *hw_device_ref);
  AVFrame *GetAVFrame() { return frame_.get(); }
  struct pl_frame *GetPLFrame() { return &pl_frame_; }

private:
  std::shared_ptr<AVFrame> frame_;

  pl_gpu gpu_;
  pl_tex tex_[PL_MAX_PLANES];

  AVBufferRef *hw_frame_ref_;
  AVHWFramesConstraints *constraints_;
  enum AVPixelFormat *transfer_formats_;

  struct pl_frame pl_frame_{};

  void GeneratePLFrame(AVBufferRef *hw_device_ref);

  int create_hw_frame(AVBufferRef *hw_device_ref, AVFrame *frame);
  int move_to_output_frame(AVFrame *frame, AVFrame *tmp_frame);
  int map_frame(AVFrame *frame, int use_hw_frame);
  int check_hw_transfer(AVFrame *frame);
  int transfer_frame(AVFrame *frame, int use_hw_frame);
};

}; // namespace EYEQ
