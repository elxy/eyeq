#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "utils.hpp"

namespace EYEQ {

enum class HardwareDecoder {
  None,
  Auto,
  VideoToolbox, // macOS
  VAAPI,        // Linux
  CUDA,         // NVIDIA (cross-platform)
  D3D12VA,      // Windows (Direct3D 12)
  D3D11VA,      // Windows (Direct3D 11)
  DXVA2,        // Windows (legacy)
};

struct AVFrameDeleter {
  void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};

class VideoSource {
public:
  explicit VideoSource(std::string filename, std::string filter_graph, int decode_threads = 0,
                       HardwareDecoder hw_decoder = HardwareDecoder::None);
  ~VideoSource();

  const std::string &Filename() const { return filename_; }
  int StreamIndex() const { return stream_index_; }
  int Width();
  int Height();

  /**
   * @brief Get codec-level width (available immediately after construction, no decoding needed)
   */
  int CodecWidth() const { return dec_ctx_->width; }

  /**
   * @brief Get codec-level height (available immediately after construction, no decoding needed)
   */
  int CodecHeight() const { return dec_ctx_->height; }

  /**
   * @brief Set the maximum number of cached frames (must be called before Start())
   */
  void SetMaxCachedFrames(size_t max_cached_frames);
  const char *CodecName() const { return avcodec_get_name(dec_ctx_->codec_id); }
  const char *PixelFormat() const { return av_get_pix_fmt_name(dec_ctx_->pix_fmt); }
  const char *ColorSpace() const { return av_color_space_name(dec_ctx_->colorspace); }
  const char *ColorRange() const { return av_color_range_name(dec_ctx_->color_range); }
  const char *ColorPrimaries() const { return av_color_primaries_name(dec_ctx_->color_primaries); }
  const char *ColorTransfer() const { return av_color_transfer_name(dec_ctx_->color_trc); }
  float Bitrate() const { return (float)fmt_ctx_->bit_rate / 1000; }
  float FrameRate() const { return frame_rate_; }
  float TimeBase() const { return time_base_; }

  std::string CodecString() {
    char buf[256];
    avcodec_string(buf, sizeof(buf), dec_ctx_, 1);
    return std::string(buf);
  }

  /**
   * @brief Get video start time, calculated from packet->pts, in seconds
   */
  float StartTime() const {
    if (AV_NOPTS_VALUE == start_pts_) {
      return std::numeric_limits<float>::quiet_NaN();
    } else {
      return time_base_ * start_pts_;
    }
  }
  /**
   * @brief Get video end time, calculated from packet->pts, in seconds
   */
  float EndTime() const {
    if (AV_NOPTS_VALUE == start_pts_) {
      return std::numeric_limits<float>::quiet_NaN();
    } else {
      return time_base_ * end_pts_;
    }
  }
  /**
   * @brief Get video duration, calculated from packet->pts, in seconds
   */
  float Duration() const {
    if (AV_NOPTS_VALUE == start_pts_ || AV_NOPTS_VALUE == end_pts_) {
      return std::numeric_limits<float>::quiet_NaN();
    } else {
      return time_base_ * (end_pts_ - start_pts_);
    }
  }

  int GetCurrentFrameSerial();
  int GetTotalFrames() const { return total_frames_; }

  /**
   * @brief Check if hardware decoding was requested
   */
  bool IsHardwareDecoderRequested() const { return hw_decoder_ != HardwareDecoder::None; }

  /**
   * @brief Check if hardware decoding is actually used (call after decoding at least one frame)
   */
  bool IsHardwareDecoderUsed() const { return hw_decode_enabled_; }

  /**
   * @brief Save current video frame
   *
   * @param path Output path
   */
  void SaveCurrentFrame(const std::filesystem::path &path);

  void Start(size_t start_frame = 0);
  void Stop();

  /**
   * @brief Get next frame
   * @param timeout_ms Timeout in ms; if <=0, waits indefinitely until a frame is returned
   * @return Frame pointer; memory is managed by shared_ptr, caller need not handle deallocation
   */
  std::shared_ptr<AVFrame> GetNextFrame(int timeout_ms = kWaitIntervalMS);
  std::shared_ptr<AVFrame> GetPrevFrame(int timeout_ms = kTooLongIntervalMS);

  /**
   * @brief Seek to specified time
   *        Automatically calls BuildKeyFrameIndex() if frame serial index is not yet built
   *
   * @param time_s Target time in seconds
   * @param timeout_ms Timeout
   * @return Frame serial of the target frame; returns -1 if undetermined
   */
  int SeekTo(float time_s, int timeout_ms = -1);

  /**
   * @brief Seek to and precisely position at the specified frame serial
   *        Finds the nearest keyframe before the target frame serial via keyframe index,
   *        seeks to that keyframe and decodes forward to the target frame.
   *        Only positions; does not return frames. Frame retrieval is done by the caller via GetNextFrame()
   * @param target_serial Target frame serial
   * @param timeout_ms Timeout
   */
  void SeekToFrameSerial(int target_serial, int timeout_ms = -1);

  /**
   * @brief Convert time to frame serial without seeking
   *        Returns the serial of the frame at or just before the given time.
   *        Automatically calls BuildKeyFrameIndex() if not yet built.
   *
   * @param time_s Target time in seconds
   * @return Frame serial; returns -1 if undetermined
   */
  int TimeToFrameSerial(float time_s);

  /**
   * @brief Wait for an available frame
   *
   * @param timeout_ms Timeout in ms; if <=0, waits indefinitely until a frame is available
   */
  void WaitForFrameAvailable(int timeout_ms = -1);

  /**
   * @brief Whether decoding has reached end of file. SeekTo() and GetPrevFrame() reset this value
   *
   * @return true if end of file has been reached
   */
  bool IsEOF() const { return eof_.load(); }

  /**
   * @brief Request the decode thread to append format=rgb48le on next filter graph rebuild
   * @return true if the request was accepted; false if user already specified format or fallback was already attempted
   */
  bool RequestFormatFallback();

private:
  std::string filename_;

  float frame_rate_ = std::numeric_limits<float>::quiet_NaN(); // fps
  float time_base_ = std::numeric_limits<float>::quiet_NaN();  // seconds
  int64_t start_pts_ = AV_NOPTS_VALUE;
  int64_t end_pts_ = AV_NOPTS_VALUE;

  // Decoding parameters
  AVFormatContext *fmt_ctx_;
  AVCodecContext *dec_ctx_;
  int stream_index_;
  AVStream *stream_;

  // Hardware decoding
  HardwareDecoder hw_decoder_;
  AVBufferRef *hw_device_ctx_ = nullptr;
  bool hw_decode_enabled_ = false; // Whether hardware decoding is actually used (determined on first frame)

  std::string filter_graph_;
  AVFilterGraph *graph_;
  AVFilterContext *filt_in_, *filt_out_;
  std::atomic<bool> need_format_fallback_{false}; // Signal decode thread to append format filter
  bool format_fallback_applied_{false};           // Whether format fallback has been applied (decode thread only)

  // Decoding state
  std::thread one_thread_;
  std::atomic<bool> decoding_active_{false};   // Controls whether decoding is active
  std::atomic<bool> decode_need_reset_{false}; // Whether decoding position needs to be reset
  std::atomic<int64_t> decode_start_pts_{0};   // Decoding start timestamp

  std::atomic<bool> eof_{false};
  std::map<int64_t, int64_t> keyframe_index_;    // Keyframe PTS -> file position mapping (in bytes)
  std::map<int64_t, int> pts_to_serial_;         // PTS -> frame serial (all frames)
  std::map<int, int64_t> keyframe_serial_index_; // Keyframe serial -> PTS
  int total_frames_ = 0;                         // Total frame count

  // Cached frames; the last element is the newest, index 0 is the oldest
  // The decoding thread only appends new frames at the end; it does not access the front or current_frame_index_
  static constexpr size_t kMaxCachedFrames = 16;
  size_t max_cached_frames_;
  int decode_threads_;
  std::deque<std::shared_ptr<AVFrame>> frame_buffer_;
  // Current frame index
  int current_frame_index_;
  int64_t current_frame_pts_;
  std::mutex buffer_mutex_;
  std::condition_variable buffer_cv_;

  std::mutex seek_mutex_;
  std::condition_variable seek_cv_;

  bool have_seeked_{false};       // True after any seek; switches serial source to pts_to_serial_
  int current_frame_serial_ = -1; // Incremental counter, used only before first seek
  size_t start_frame_serial_ = 0;

  /**
   * @brief Open video stream
   */
  void OpenStream();
  /**
   * @brief Parse media file properties
   */
  void ParseMediaProps();
  /**
   * @brief Build keyframe index
   */
  void BuildKeyFrameIndex();

  /**
   * @brief Decoding thread
   */
  void AllInOneThread();
  /**
   * @brief Read packet and decode next frame
   *
   * @param out Output frame
   * @param packet Input packet
   * @param pending_packet Records whether the packet has been processed
   * @return >=0 on success, AVERROR_EOF on end, other values indicate failure
   */
  int DecodeNextFrame(AVFrame *out, AVPacket *packet, bool &packet_pending);
  /**
   * @brief Feed video frame into filter and get the output frame
   *
   * @param out Output frame
   * @param in Input frame
   * @return >=0 on success, AVERROR_EOF on end, AVERROR(EAGAIN) means retry needed, other values indicate failure
   */
  int FilterNextFrame(AVFrame *out, AVFrame *in);

  /**
   * @brief Create filter graph for producing render-ready frames
   *
   * @param frame Decoded frame, used to obtain parameters
   */
  void CreateFilterGraph(const AVFrame *frame);

  /**
   * @brief Clean up old frames to save memory
   */
  void CleanupOldFrames();

  /**
   * @brief Find the nearest keyframe PTS that does not exceed the target PTS
   * @param target_pts Target timestamp
   * @return PTS of the nearest keyframe; returns -1 on failure
   */
  int64_t FindNearestKeyframePts(int64_t target_pts);
};

}; // namespace EYEQ
