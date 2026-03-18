#include "video_source.hpp"

#include <limits>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include "log.hpp"
#include "utils.hpp"

using namespace EYEQ;

VideoSource::VideoSource(std::string filename, std::string filter_graph, size_t max_cached_frames,
                         int decode_threads, HardwareDecoder hw_decoder)
    : filename_(std::move(filename)), hw_decoder_(hw_decoder), filter_graph_(std::move(filter_graph)),
      max_cached_frames_(max_cached_frames), decode_threads_(decode_threads) {
  fmt_ctx_ = nullptr;
  dec_ctx_ = nullptr;
  stream_index_ = -1;
  stream_ = nullptr;

  graph_ = nullptr;

  OpenStream();

  // FIXME: avg_frame_rate may be inaccurate for some video files
  frame_rate_ = av_q2d(stream_->avg_frame_rate);
  time_base_ = av_q2d(stream_->time_base);
  start_pts_ = ((float)fmt_ctx_->start_time / AV_TIME_BASE) / time_base_;
  end_pts_ = ((float)fmt_ctx_->duration / AV_TIME_BASE) / time_base_ + start_pts_;

  current_frame_index_ = -1;
  current_frame_pts_ = start_pts_;

  have_seeked_ = false;
  current_frame_serial_ = -1;
}

VideoSource::~VideoSource() {
  Stop();

  avfilter_graph_free(&graph_);
  avcodec_free_context(&dec_ctx_);
  avformat_close_input(&fmt_ctx_);
  av_buffer_unref(&hw_device_ctx_);
}

int VideoSource::Width() {
  std::unique_lock<std::mutex> lock(buffer_mutex_);

  AVFrame *frame = nullptr;

  if (current_frame_index_ >= 0 && current_frame_index_ < (int)frame_buffer_.size()) {
    frame = frame_buffer_[current_frame_index_].get();
  } else if (current_frame_index_ < 0 && frame_buffer_.size() > 0) {
    frame = frame_buffer_[0].get();
  }
  Logger->trace("Use frame {} to guess width...", fmt::ptr(frame));

  if (frame) {
    Logger->trace("frame resolution {}x{}", frame->width, frame->height);
    return frame->width;
  } else {
    return dec_ctx_->width;
  }
}

int VideoSource::Height() {
  std::unique_lock<std::mutex> lock(buffer_mutex_);

  AVFrame *frame = nullptr;

  if (current_frame_index_ >= 0 && current_frame_index_ < (int)frame_buffer_.size()) {
    frame = frame_buffer_[current_frame_index_].get();
  } else if (current_frame_index_ < 0 && frame_buffer_.size() > 0) {
    frame = frame_buffer_[0].get();
  }

  if (frame) {
    return frame->height;
  } else {
    return dec_ctx_->height;
  }
}

int VideoSource::GetCurrentFrameSerial() {
  if (!have_seeked_) {
    return current_frame_serial_;
  }

  std::unique_lock<std::mutex> lock(buffer_mutex_);
  int64_t pts = frame_buffer_[current_frame_index_]->pts;

  // Prefer index lookup (supports VFR)
  if (!pts_to_serial_.empty()) {
    auto it = pts_to_serial_.find(pts);
    if (it != pts_to_serial_.end()) {
      return it->second;
    }
  }

  // Fallback: estimate using pts/duration (CFR scenario)
  int64_t duration = frame_buffer_[current_frame_index_]->duration;
  Logger->debug("Use pts {} and duration {} to guess frame serial...", pts, duration);
  if (AV_NOPTS_VALUE != pts && duration > 0) {
    return pts / duration;
  } else {
    return -1;
  }
}

void VideoSource::SaveCurrentFrame(const std::filesystem::path &path) {
  std::unique_lock<std::mutex> lock(buffer_mutex_);
  buffer_cv_.wait(lock, [this]() { return (int)frame_buffer_.size() > current_frame_index_ || eof_; });
  if ((int)frame_buffer_.size() <= current_frame_index_) {
    Logger->error("Current frame index {} out of range, please check bugs!", current_frame_index_);
    return;
  }

  AVFrame *frame = frame_buffer_[current_frame_index_].get();

  std::string format;
  auto ext = path.extension();
  if (!ext.empty()) {
    format = ext.u8string();
    if (!format.empty() && format[0] == '.') {
      format.erase(0, 1);
    }
  }

  save_frame(frame, path, format);
}

void VideoSource::Start(size_t start_frame) {
  if (one_thread_.joinable()) {
    return;
  }

  start_frame_serial_ = start_frame;
  one_thread_ = std::thread(&VideoSource::AllInOneThread, this);
}

void VideoSource::Stop() {
  decoding_active_ = false;

  if (one_thread_.joinable()) {
    one_thread_.join();
  }
}

std::shared_ptr<AVFrame> VideoSource::GetNextFrame(int timeout_ms) {
  // Buffer must have enough frames, unless EOF has been reached
  std::unique_lock<std::mutex> lock(buffer_mutex_);

  std::chrono::milliseconds timeout = get_timeout(timeout_ms);
  buffer_cv_.wait_for(lock, timeout, [this]() { return (int)frame_buffer_.size() > current_frame_index_ + 1 || eof_; });

  if ((int)frame_buffer_.size() <= current_frame_index_ + 1) {
    SPDLOG_LOGGER_TRACE(Logger, "frame_buffer_.size() {}, current_frame_index_ {}, eof_ {}", frame_buffer_.size(),
                        current_frame_index_, eof_.load());
    if (eof_) {
      throw std::out_of_range("VideoSource meet EOF, cannot get next frame");
    } else {
      throw timeout_error("VideoSource cannot get next frame, please wait for a moment");
    }
  }

  // Move to next frame and return
  current_frame_index_++;

  // Get current frame
  auto frame = frame_buffer_[current_frame_index_];
  current_frame_pts_ = frame->pts;
  current_frame_serial_ += 1;

  CleanupOldFrames();

  SPDLOG_LOGGER_TRACE(Logger, "GetNextFrame frame {}, pts {}. frame_buffer_.size() {}, current_frame_index_ {}",
                      fmt::ptr(frame.get()), frame->pts, frame_buffer_.size(), current_frame_index_);
  return frame;
}

std::shared_ptr<AVFrame> VideoSource::GetPrevFrame(int timeout_ms) {
  std::shared_ptr<AVFrame> frame;

  if (current_frame_index_ < 0) {
    throw timeout_error("Get previous frame before any frame decoded, please wait for a moment");
  } else if (current_frame_index_ > 0) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // Normal case: move back one frame
    current_frame_index_--;
    frame = frame_buffer_[current_frame_index_];
    current_frame_pts_ = frame->pts;
    current_frame_serial_ -= 1;
    SPDLOG_LOGGER_TRACE(Logger, "GetPrevFrame frame {}, pts {}. frame_buffer_.size() {}, current_frame_index_ {}",
                        fmt::ptr(frame.get()), frame->pts, frame_buffer_.size(), current_frame_index_);
    return frame;
  }

  // If already at the first frame, cannot step back further; need to decode earlier frames
  // Check if we can re-decode earlier frames
  int64_t target_pts = current_frame_pts_ - 1;
  int64_t keyframe_pts = FindNearestKeyframePts(target_pts);
  if (keyframe_pts < 0) {
    throw std::out_of_range(fmt::format("No keyframe earlier than (pts {}, {:.3f}s) found", current_frame_pts_,
                                        current_frame_pts_ * time_base_));
  }
  SPDLOG_LOGGER_TRACE(Logger, "Get previous keyframe pts {} ({:.3f} s) for target pts {}", keyframe_pts,
                      keyframe_pts * time_base_, target_pts);
  // Check if frame cache is sufficient
  {
    int64_t diff_pts = current_frame_pts_ - keyframe_pts;
    int64_t interval_pts = static_cast<int64_t>(1. / (time_base_ * frame_rate_));
    if ((diff_pts / interval_pts) > (int)max_cached_frames_) {
      Logger->debug("Frame cache size {} not meet requirement {} = ({} / {})", max_cached_frames_,
                    diff_pts / interval_pts, diff_pts, interval_pts);
      // TODO: support --frame-cache option
      Logger->warn("Due to limited frame buffer size ({}), step back will skip frames and may affect synchronization."
                   " Consider setting --frame-cache to {} for a larger cache,"
                   " or set --frame-cache to -1 for dynamic cache sizing.",
                   max_cached_frames_, std::ceil(diff_pts / interval_pts));
    }
  }

  { // Request the decode thread to re-decode from the keyframe
    // Notify the decode thread to seek
    decode_start_pts_ = keyframe_pts;
    decode_need_reset_ = true;

    // Wait for the decode thread to finish seeking
    std::unique_lock<std::mutex> lock(seek_mutex_);
    seek_cv_.wait(lock);
  }

  // Wait for the decode thread to fill the buffer
  std::unique_lock<std::mutex> lock(buffer_mutex_);
  std::chrono::milliseconds timeout = get_timeout(timeout_ms);
  bool frame_found = buffer_cv_.wait_for(lock, timeout, [this, target_pts]() {
    if (frame_buffer_.empty()) {
      return false;
    } else if (frame_buffer_.size() >= max_cached_frames_) {
      // If buffer is full, waiting further won't yield target_pts
      return true;
    }
    // Check if buffer has been decoded up to target_pts
    auto frame = frame_buffer_.back();
    return frame->pts >= target_pts;
  });

  if (!frame_found) {
    throw timeout_error("VideoSource::GetPrevFrame() timed out");
  }

  // Find the frame closest to but not exceeding target_pts
  size_t target_index = 0;
  for (int i = (int)frame_buffer_.size() - 1; i >= 0; i--) {
    if (frame_buffer_[i]->pts <= target_pts) {
      target_index = i;
      break;
    }
  }
  Logger->debug("Get previous frame with pts {} ({:.3f} s)", frame_buffer_[target_index]->pts,
                frame_buffer_[target_index]->pts * time_base_);

  // Set current index
  current_frame_index_ = target_index;
  // Get current frame
  frame = frame_buffer_[current_frame_index_];
  current_frame_pts_ = frame->pts;

  SPDLOG_LOGGER_TRACE(Logger, "GetPrevFrame frame {}, pts {}. frame_buffer_.size() {}, current_frame_index_ {}",
                      fmt::ptr(frame.get()), frame->pts, frame_buffer_.size(), current_frame_index_);
  return frame;
}

int VideoSource::SeekTo(float time_s, int timeout_ms) {
  if (pts_to_serial_.empty()) {
    BuildKeyFrameIndex();
  }

  float start_time = StartTime();
  float end_time = EndTime();
  if (!std::isnan(start_time)) {
    if (time_s < start_time) {
      time_s = start_time;
    }
  }
  if (!std::isnan(end_time)) {
    if (end_time < time_s) {
      time_s = end_time;
    }
  }

  // Notify the decode thread to seek
  int64_t target_pts = time_s / time_base_;
  decode_start_pts_ = target_pts;
  decode_need_reset_ = true;

  // Actual seek finds the nearest keyframe; use its serial as the target frame serial
  int64_t keyframe_pts = FindNearestKeyframePts(target_pts);
  int serial = -1;
  if (keyframe_pts >= 0) {
    auto it = pts_to_serial_.find(keyframe_pts);
    if (it != pts_to_serial_.end()) {
      serial = it->second;
    }
  }

  // Wait for the decode thread to finish seeking
  std::unique_lock<std::mutex> lock(seek_mutex_);
  std::chrono::milliseconds timeout = get_timeout(timeout_ms);
  seek_cv_.wait_for(lock, timeout);
  return serial;
}

void VideoSource::SeekToFrameSerial(int target_serial, int timeout_ms) {
  if (pts_to_serial_.empty()) {
    BuildKeyFrameIndex();
  }

  // Clamp target frame serial to valid range
  if (target_serial < 0) {
    target_serial = 0;
  } else if (target_serial >= total_frames_) {
    target_serial = total_frames_ - 1;
  }

  // 1. Find the nearest keyframe before the target frame serial
  auto it = keyframe_serial_index_.upper_bound(target_serial);
  int keyframe_serial;
  int64_t keyframe_pts;
  if (it == keyframe_serial_index_.begin()) {
    keyframe_serial = it->first;
    keyframe_pts = it->second;
  } else {
    --it;
    keyframe_serial = it->first;
    keyframe_pts = it->second;
  }

  int frames_to_skip = target_serial - keyframe_serial;

  Logger->debug("SeekToFrameSerial: target_serial={}, keyframe_serial={}, keyframe_pts={}, frames_to_skip={}",
                target_serial, keyframe_serial, keyframe_pts, frames_to_skip);

  // 2. Notify the decode thread to seek to the keyframe and skip frames_to_skip frames
  //    Reuse start_frame_serial_ mechanism: decode thread skips the specified number of frames (no filter, no buffer)
  decode_start_pts_ = keyframe_pts;
  start_frame_serial_ = frames_to_skip;
  decode_need_reset_ = true;

  // Wait for the decode thread to finish seeking
  std::unique_lock<std::mutex> lock(seek_mutex_);
  std::chrono::milliseconds timeout = get_timeout(timeout_ms);
  seek_cv_.wait_for(lock, timeout);
}

void VideoSource::WaitForFrameAvailable(int timeout_ms) {
  std::unique_lock<std::mutex> lock(buffer_mutex_);

  std::chrono::milliseconds timeout = get_timeout(timeout_ms);
  buffer_cv_.wait_for(lock, timeout, [this]() { return !frame_buffer_.empty(); });

  if ((int)frame_buffer_.size() <= current_frame_index_) {
    SPDLOG_LOGGER_TRACE(Logger, "frame_buffer_.size() {}, current_frame_index_ {}, eof_ {}", frame_buffer_.size(),
                        current_frame_index_, eof_.load());
    if (eof_) {
      throw std::out_of_range("video source meet EOF");
    } else {
      throw timeout_error("timeout");
    }
  }
}

void VideoSource::OpenStream() {
  int ret;
  /* open input file, and allocate format context */
  ret = avformat_open_input(&fmt_ctx_, filename_.c_str(), nullptr, nullptr);
  if (ret < 0) {
    throw std::runtime_error(fmt::format("Could not open file {}, {}: {}", filename_, ret, ffmpeg_error_string(ret)));
  }

  /* retrieve stream information */
  ret = avformat_find_stream_info(fmt_ctx_, nullptr);
  if (ret < 0) {
    throw std::runtime_error(fmt::format("Could not find stream information, {}: {}", ret, ffmpeg_error_string(ret)));
  }

  /* dump input information to stderr */
  av_dump_format(fmt_ctx_, stream_index_, filename_.c_str(), 0);

  ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ret < 0) {
    throw std::runtime_error(
        fmt::format("Could not find video stream in input file, {}: {}", ret, ffmpeg_error_string(ret)));
  }
  stream_index_ = ret;
  stream_ = fmt_ctx_->streams[stream_index_];

  /* find decoder for the stream */
  const AVCodec *dec = avcodec_find_decoder(stream_->codecpar->codec_id);
  if (!dec) {
    throw std::runtime_error(fmt::format("Failed to find decoder {}", avcodec_get_name(stream_->codecpar->codec_id)));
  }

  /* Allocate a codec context for the decoder */
  dec_ctx_ = avcodec_alloc_context3(dec);
  if (!dec_ctx_) {
    throw std::runtime_error(fmt::format("Failed to allocate the codec context"));
  }

  /* Copy codec parameters from input stream to output codec context */
  ret = avcodec_parameters_to_context(dec_ctx_, stream_->codecpar);
  if (ret < 0) {
    throw std::runtime_error(
        fmt::format("Failed to copy codec parameters to decoder context, {}: {}", ret, ffmpeg_error_string(ret)));
  }

  /* Hardware decoder initialization */
  if (hw_decoder_ != HardwareDecoder::None) {
    // Build the list of hardware device types to try
    std::vector<AVHWDeviceType> hw_types_to_try;
    if (hw_decoder_ == HardwareDecoder::Auto) {
#ifdef __APPLE__
      hw_types_to_try.push_back(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(__linux__)
      hw_types_to_try.push_back(AV_HWDEVICE_TYPE_VAAPI);
      hw_types_to_try.push_back(AV_HWDEVICE_TYPE_CUDA);
#elif defined(_WIN32)
      hw_types_to_try.push_back(AV_HWDEVICE_TYPE_D3D12VA);
      hw_types_to_try.push_back(AV_HWDEVICE_TYPE_D3D11VA);
      hw_types_to_try.push_back(AV_HWDEVICE_TYPE_DXVA2);
#endif
    } else {
      AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
      if (hw_decoder_ == HardwareDecoder::VideoToolbox) {
        hw_type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
      } else if (hw_decoder_ == HardwareDecoder::VAAPI) {
        hw_type = AV_HWDEVICE_TYPE_VAAPI;
      } else if (hw_decoder_ == HardwareDecoder::CUDA) {
        hw_type = AV_HWDEVICE_TYPE_CUDA;
      } else if (hw_decoder_ == HardwareDecoder::D3D12VA) {
        hw_type = AV_HWDEVICE_TYPE_D3D12VA;
      } else if (hw_decoder_ == HardwareDecoder::D3D11VA) {
        hw_type = AV_HWDEVICE_TYPE_D3D11VA;
      } else if (hw_decoder_ == HardwareDecoder::DXVA2) {
        hw_type = AV_HWDEVICE_TYPE_DXVA2;
      }
      if (hw_type != AV_HWDEVICE_TYPE_NONE) {
        hw_types_to_try.push_back(hw_type);
      }
    }

    // Try each hardware device type in order until one succeeds
    for (AVHWDeviceType hw_type : hw_types_to_try) {
      ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_type, nullptr, nullptr, 0);
      if (ret < 0) {
        Logger->warn("Failed to create hw device {}: {}",
                     av_hwdevice_get_type_name(hw_type), ffmpeg_error_string(ret));
        hw_device_ctx_ = nullptr;
        continue;
      }
      Logger->info("Using hardware decoder: {}", av_hwdevice_get_type_name(hw_type));
      dec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
      break;
    }
    if (!hw_device_ctx_) {
      Logger->warn("All hardware decoders failed, falling back to software decoding");
    }
  }

  /* Enable multi-threaded decoding */
  dec_ctx_->thread_count = decode_threads_; // 0 means auto-detect
  dec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

  /* Set extra_hw_frames to avoid "Static surface pool size exceeded" errors.
   * The hardware surface pool must be large enough to hold:
   *   - reference frames used by the codec (up to 16 for H.264)
   *   - frames held by each decode thread
   *   - frames cached in the application-level frame_buffer_ (max_cached_frames_)
   *   - a safety margin for the filter graph and display pipeline
   */
  if (hw_device_ctx_) {
    int thread_count = decode_threads_ > 0 ? decode_threads_ : std::thread::hardware_concurrency();
    int extra = 16 /* max ref frames */ + thread_count + static_cast<int>(max_cached_frames_) + 8 /* safety margin */;
    dec_ctx_->extra_hw_frames = extra;
    Logger->info("Setting extra_hw_frames to {} (ref=16, threads={}, cache={}, margin=8)",
                 extra, thread_count, max_cached_frames_);
  }

  /* Init the decoders */
  ret = avcodec_open2(dec_ctx_, dec, nullptr);
  if (ret < 0) {
    throw std::runtime_error(fmt::format("Failed to open codec, {}: {}", ret, ffmpeg_error_string(ret)));
  }
}

void VideoSource::ParseMediaProps() {}

/**
 * @brief Build keyframe index
 */
void VideoSource::BuildKeyFrameIndex() {
  keyframe_index_.clear();
  pts_to_serial_.clear();
  keyframe_serial_index_.clear();

  // Use a separate AVFormatContext for scanning to avoid contention with the decode thread
  AVFormatContext *scan_ctx = nullptr;
  int ret = avformat_open_input(&scan_ctx, filename_.c_str(), nullptr, nullptr);
  if (ret < 0) {
    Logger->error("BuildKeyFrameIndex: could not open file {}: {}", filename_, ffmpeg_error_string(ret));
    return;
  }
  ret = avformat_find_stream_info(scan_ctx, nullptr);
  if (ret < 0) {
    Logger->error("BuildKeyFrameIndex: could not find stream info: {}", ffmpeg_error_string(ret));
    avformat_close_input(&scan_ctx);
    return;
  }

  AVPacket *packet = av_packet_alloc();

  int64_t start_pts = std::numeric_limits<int64_t>::max();
  int64_t end_pts = std::numeric_limits<int64_t>::min();
  int serial = 0;

  // Scan the entire video to build keyframe index and frame serial mapping
  while (av_read_frame(scan_ctx, packet) >= 0) {
    if (packet->stream_index == stream_index_) {
      if (packet->pts != AV_NOPTS_VALUE) {
        pts_to_serial_[packet->pts] = serial;

        if (packet->flags & AV_PKT_FLAG_KEY) {
          // NOTE: The interval between keyframes may exceed the cache queue size
          // Record keyframe PTS and file position
          keyframe_index_[packet->pts] = packet->pos;
          keyframe_serial_index_[serial] = packet->pts;
        }
        start_pts = std::min(start_pts, packet->pts);
        end_pts = std::max(end_pts, packet->pts);
        serial++;
      }
    }
    av_packet_unref(packet);
  }

  av_packet_free(&packet);
  avformat_close_input(&scan_ctx);

  total_frames_ = serial;

  Logger->info("Built keyframe index: {} keyframes, {} total frames", keyframe_serial_index_.size(), total_frames_);

  start_pts_ = start_pts;
  end_pts_ = end_pts;
}

void VideoSource::AllInOneThread() {
  // Set thread name for debugging
#ifdef __APPLE__
  pthread_setname_np("VideoSource::AllInOneThread()");
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), "VideoSource::AllInOneThread()");
#endif

  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *filt_frame = av_frame_alloc();
  AVFrame *tmp_frame = av_frame_alloc();
  bool packet_pending = false;
  int ret;
  if (!packet || !frame || !filt_frame) {
    throw std::runtime_error("Could not allocate frame or packet");
  }

  decoding_active_ = true;
  while (decoding_active_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kUpdateIntervalMS));
    if (decode_need_reset_) {
      // Reset decoder state
      av_seek_frame(fmt_ctx_, stream_index_, decode_start_pts_.load(), AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(dec_ctx_);
      packet_pending = false;
      eof_ = false;
      decode_need_reset_ = false;

      // Reset filter graph
      avfilter_graph_free(&graph_);

      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        frame_buffer_.clear();
        current_frame_index_ = -1;
        current_frame_pts_ = 0;
        have_seeked_ = true;
      }

      {
        std::lock_guard<std::mutex> lock(seek_mutex_);
        seek_cv_.notify_all();
      }
    }

    if (eof_) {
      continue;
    }

    // Check if buffer size is sufficient
    size_t buffer_size;
    {
      std::lock_guard<std::mutex> lock(buffer_mutex_);
      buffer_size = frame_buffer_.size();
    }

    // If buffer is full, wait
    if (buffer_size >= max_cached_frames_) {
      continue;
    }

    ret = DecodeNextFrame(frame, packet, packet_pending);
    if (AVERROR_EOF == ret) {
    } else if (ret < 0) {
      throw std::runtime_error(fmt::format("Error in decoding, {}: {}", ret, ffmpeg_error_string(ret)));
    }

    // Skip filter processing to speed up frame seeking
    if (start_frame_serial_ > 0) {
      if (AVERROR_EOF != ret) {
        av_frame_unref(tmp_frame);
        av_frame_move_ref(tmp_frame, frame);
        current_frame_serial_++;
        start_frame_serial_--;
        continue;
      } else {
        // If EOF is reached, stop skipping and keep the last frame for display
        ret = 0;
        av_frame_move_ref(frame, tmp_frame);
        start_frame_serial_ = 0;
      }
    }

    if (!graph_) {
      CreateFilterGraph(frame);
    }
    ret = FilterNextFrame(filt_frame, AVERROR_EOF == ret ? nullptr : frame);
    if (AVERROR_EOF == ret) {
      Logger->trace("av_buffersink_get_frame() returned EOF");
      eof_ = true;
    } else if (ret < 0) {
      throw std::runtime_error(fmt::format("Error in filtering, {}: {}", ret, ffmpeg_error_string(ret)));
    } else if (0 <= ret) {
      std::lock_guard<std::mutex> lock(buffer_mutex_);
      // Frame may be shared across threads; use shared_ptr for management
      frame_buffer_.push_back(std::shared_ptr<AVFrame>(filt_frame, AVFrameDeleter()));
      filt_frame->opaque = stream_;
      SPDLOG_LOGGER_TRACE(Logger, "pushed frame {} pts {}", fmt::ptr(filt_frame), filt_frame->pts);
      buffer_cv_.notify_all(); // Notify waiting threads
    }
    filt_frame = av_frame_alloc();
  }

  av_frame_free(&frame);
  av_frame_free(&filt_frame);
  av_frame_free(&tmp_frame);
  av_packet_free(&packet);
}

int VideoSource::DecodeNextFrame(AVFrame *out, AVPacket *packet, bool &packet_pending) {
  int ret;

  while (true) {
    if (packet_pending) {
      packet_pending = false;
    } else {
      ret = av_read_frame(fmt_ctx_, packet);
      if (AVERROR_EOF == ret) {
        // Logger->debug("av_read_frame() returned EOF");
      } else if (ret < 0) {
        Logger->error("Error while reading from file, {}: {}", ret, ffmpeg_error_string(ret));
        return ret;
      }

      if (packet->stream_index != stream_index_) {
        av_packet_unref(packet);
        continue;
      }
      // SPDLOG_LOGGER_TRACE(Logger, "read packet pts {}", packet->pts);
    }

    ret = avcodec_send_packet(dec_ctx_, packet);
    if (AVERROR(EAGAIN) == ret) {
      SPDLOG_LOGGER_TRACE(Logger, "send packet get EAGAIN, need to receive frame first");
      packet_pending = true;
    } else if (AVERROR_EOF == ret) {
      SPDLOG_LOGGER_TRACE(Logger, "send packet get EOF");
    } else if (ret < 0) {
      Logger->error("Error while sending a packet to the decoder: {}", ffmpeg_error_string(ret));
      return ret;
    } else {
      SPDLOG_LOGGER_TRACE(Logger, "send packet pts {}", packet->pts);
    }
    if (!packet_pending) {
      av_packet_unref(packet);
    }

    ret = avcodec_receive_frame(dec_ctx_, out);
    if (0 == ret) {
      // NOTE: Need to support raw streams
      out->pts = out->best_effort_timestamp;
      SPDLOG_LOGGER_TRACE(Logger, "decoded frame with pts {}", out->pts);
    } else if (AVERROR(EAGAIN) == ret) {
      SPDLOG_LOGGER_TRACE(Logger, "receive frame get EAGAIN, need to send packet again");
      if (packet_pending) {
        SPDLOG_LOGGER_TRACE(Logger, "avcodec_receive_frame() and avcodec_send_packet() both returned EAGAIN,"
                                    " which is not an API violation. we need to receive more frames.");
      }
      continue;
    } else if (AVERROR_EOF == ret) {
      Logger->trace("avcodec_receive_frame() returned EOF");
    } else {
      Logger->error("Error while receiving a frame from the decoder, {}: {}", ret, ffmpeg_error_string(ret));
    }
    return ret;
  }

  throw std::runtime_error("Unreachable code");
}

int VideoSource::FilterNextFrame(AVFrame *out, AVFrame *in) {
  int ret;
  ret = av_buffersrc_add_frame_flags(filt_in_, in, AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    av_frame_unref(in);
    Logger->error("Error while feeding the filtergraph, {}: {}", ret, ffmpeg_error_string(ret));
    return ret;
  }

  return av_buffersink_get_frame(filt_out_, out);
}

void VideoSource::CreateFilterGraph(const AVFrame *frame) {
  // Print actual decoded frame format to confirm hardware decoding status
  bool is_hardware = frame->hw_frames_ctx != nullptr;
  hw_decode_enabled_ = is_hardware;  // Record whether hardware decoding is actually used
  Logger->debug("{}: decoded frame format: {}{}", filename_,
               av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)),
               is_hardware ? " (hardware)" : " (software)");

  AVFilterGraph *graph = nullptr;

  graph = avfilter_graph_alloc();
  if (!graph) {
    throw std::runtime_error("Could not allocate filter graph");
  }

  char sws_flags[] = "flags=spline+full_chroma_int+accurate_rnd+full_chroma_inp:";
  graph->scale_sws_opts = av_strdup(sws_flags);

  AVCodecParameters *codecpar = stream_->codecpar;

  // create source filter
  AVFilterContext *filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "buffer");
  if (!filt_src) {
    throw std::runtime_error("Could not allocate buffer source");
  }
  AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
  AVRational fr = av_guess_frame_rate(fmt_ctx_, stream_, nullptr);
  par->format = frame->format;
  par->time_base = stream_->time_base;
  par->width = frame->width;
  par->height = frame->height;
  par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
  par->color_space = frame->colorspace;
  par->color_range = frame->color_range;
  par->frame_rate = fr;
  par->hw_frames_ctx = frame->hw_frames_ctx;
  if (av_buffersrc_parameters_set(filt_src, par) < 0) {
    throw std::runtime_error("Could not set buffer source parameters");
  }
  if (avfilter_init_dict(filt_src, nullptr) < 0) {
    throw std::runtime_error("Could not init source filter");
  }

  // create sink filter
  AVFilterContext *filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"), "buffersink");
  if (!filt_out) {
    throw std::runtime_error("Could not allocate buffer sink");
  }
  if (avfilter_init_dict(filt_out, nullptr) < 0) {
    throw std::runtime_error("Could not init sink filter");
  }

  // Auto-insert hwdownload before user filter when frame is a hardware frame
  std::string effective_filter = filter_graph_;
  if (frame->hw_frames_ctx && !filter_graph_.empty()) {
    AVHWFramesContext *hw_frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    const char *sw_fmt_name = av_get_pix_fmt_name(hw_frames->sw_format);
    effective_filter = fmt::format("hwdownload,format={},{}", sw_fmt_name, filter_graph_);
    Logger->info("Auto-inserted hwdownload for hardware frames, effective filter: {}", effective_filter);
  }

  // Auto-append format=rgb48le when texture format is incompatible with libplacebo
  if (need_format_fallback_.load()) {
    if (effective_filter.empty()) {
      effective_filter = "format=rgb48le";
    } else {
      effective_filter += ",format=rgb48le";
    }
    format_fallback_applied_ = true;
    need_format_fallback_.store(false);
    Logger->warn("{}: auto-appending format=rgb48le filter to handle incompatible texture format", filename_);
  }

  int nb_filters = graph->nb_filters;
  AVFilterInOut *outputs = nullptr, *inputs = nullptr;
  if (!effective_filter.empty()) {
    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
      throw std::runtime_error("Could not allocate filter i/o context");
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = filt_src;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = filt_out;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    if (avfilter_graph_parse_ptr(graph, effective_filter.c_str(), &inputs, &outputs, nullptr) < 0) {
      throw std::runtime_error("Could not parse filter graph");
    }
  } else {
    if (avfilter_link(filt_src, 0, filt_out, 0) < 0) {
      throw std::runtime_error("Could not link filters");
    }
  }

  /* Reorder the filters to ensure that inputs of the custom filters are merged first */
  for (unsigned i = 0; i < graph->nb_filters - nb_filters; i++) {
    FFSWAP(AVFilterContext *, graph->filters[i], graph->filters[i + nb_filters]);
  }

  if (avfilter_graph_config(graph, nullptr) < 0) {
    throw std::runtime_error("Could not configure filter graph");
  }

  graph_ = graph;
  filt_in_ = filt_src;
  filt_out_ = filt_out;

  avfilter_inout_free(&outputs);
  avfilter_inout_free(&inputs);
  av_freep(&par);
}

bool VideoSource::RequestFormatFallback() {
  // User filter already contains format=, do not override
  if (filter_graph_.find("format=") != std::string::npos) {
    Logger->warn("{}: user filter already contains format specifier, skipping auto format fallback", filename_);
    return false;
  }
  // Already attempted fallback, do not retry
  if (need_format_fallback_.load() || format_fallback_applied_) {
    Logger->error("{}: format fallback already attempted, giving up", filename_);
    return false;
  }
  need_format_fallback_.store(true);
  decode_need_reset_.store(true);
  return true;
}

void VideoSource::CleanupOldFrames() {
  assert(current_frame_index_ >= 0);

  // If current frame index exceeds queue midpoint, clean up front portion to save memory
  if (current_frame_index_ > (int)max_cached_frames_ / 2) {
    size_t frames_to_remove = current_frame_index_ - max_cached_frames_ / 4;
    if (frames_to_remove > 0) {
      frame_buffer_.erase(frame_buffer_.begin(), frame_buffer_.begin() + frames_to_remove);
      current_frame_index_ -= frames_to_remove;
    }
    SPDLOG_LOGGER_TRACE(Logger, "After cleanup, frame_buffer_.size() {}, current_frame_index_ {}", frame_buffer_.size(),
                        current_frame_index_);
  }
}

int64_t VideoSource::FindNearestKeyframePts(int64_t target_pts) {
  if (keyframe_index_.empty()) {
    BuildKeyFrameIndex();
  }

  // Find the first keyframe with PTS greater than target
  auto it = keyframe_index_.upper_bound(target_pts);

  // If not found, or it is the first keyframe
  if (it == keyframe_index_.begin()) {
    // No earlier keyframe
    if (keyframe_index_.begin()->first > target_pts) {
      return -1;
    }
    return keyframe_index_.begin()->first;
  }

  // Move backward to get the previous keyframe (PTS <= target)
  --it;
  return it->first;
}
