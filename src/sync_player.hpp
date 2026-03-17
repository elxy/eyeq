#pragma once

#include <functional>
#include <map>
#include <memory>

#include <spdlog/fmt/fmt.h>
extern "C" {
#include <libavutil/frame.h>
}

#include "log.hpp"
#include "video_source.hpp"

namespace EYEQ {

/**
 * @class SyncVideoPlayer
 * @brief Multi-video synchronized player, coordinates all video decoders with the master clock
 *        The first added video stream is used as the reference stream; other streams fetch frames relative to it. Assumes all video streams share the same frame rate
 */
class SyncPlayer {
public:
  SyncPlayer();
  ~SyncPlayer();

  enum class PlayState {
    kStopped,
    kPlaying,
    kPaused,
  };

  /**
   * @brief Add a video source
   * @param source Video source instance
   * @param id Video ID; negative value means auto-assign
   * @return The assigned video ID
   */
  int AddVideoSource(std::unique_ptr<VideoSource> &&source, int id);

  void SetMainId(int main_id) {
    if (sources_.find(main_id) == sources_.end()) {
      throw std::runtime_error(fmt::format("main_id {} not found", main_id));
    }
    main_id_ = main_id;
  }
  int GetMainId() const { return main_id_; }
  bool HasVideoSource(int id) const { return sources_.find(id) != sources_.end(); }

  int Width(int id = -1);
  int Height(int id = -1);
  float FrameRate(int id = -1);
  float StartTime(int id = -1);
  float EndTime(int id = -1);

  /**
   * @brief Save current frames of all videos
   *        Named as "{frame_serial}.{id}.{filename}.{ext}"
   *
   * @param dirname Output directory; if empty, saves to the source video's directory
   * @param ext File extension
   */
  void SaveCurrentFrames(bool save_in_source, const std::string &ext);

  /**
   * @brief Start playback from the specified time/frame
   */
  void StartSources(float seek_to = -1., size_t seek_frames = -1);

  /**
   * @brief Set callback for when new frames are available
   */
  void SetFrameUpdateCallback(std::function<void(std::map<int, std::shared_ptr<AVFrame>> &)> callback);
  /**
   * @brief Set flicker callback
   */
  void SetFlickerCallback(float interval_s, std::function<void()> callback);
  void SetResolutionChangedCallback(std::function<void()> callback);

  /**
   * @brief Start playback
   *
   * @param pause Whether to remain paused
   */
  void Start(bool pause);

  /**
   * @brief Stop all video playback
   */
  void Stop();

  /**
   * @brief Resume playback
   */
  void Play();

  /**
   * @brief Pause all videos
   */
  void Pause();

  /**
   * @brief Toggle pause/resume
   *
   */
  void InvertPause();

  std::map<int, std::shared_ptr<AVFrame>> GetFrames(bool forward);

  void SeekFrames(int nb_frames);

  /**
   * @brief Seek to specified time
   * @param time_s Target time (seconds)
   */
  void SeekTo(float time_s);

  /**
   * @brief Seek by offset
   * @param offset_s Offset relative to current time (seconds)
   */
  void SeekOffset(float offset_s);

  void StepNextFrame();
  void StepPrevFrame();

  /**
   * @brief Get current playback time
   * @return Current playback time (seconds)
   */
  float CurrentTime() const;

  int GetCurrentFrameSerial() const;

  int GetTotalFrames(int id = -1) const;

  /**
   * @brief Check for video streams where hardware decoding was requested but not used
   * @return List of video stream IDs not using hardware decoding
   */
  std::vector<int> CheckUnusedHardwareDecoders() const;

  /**
   * @brief Get current playback FPS (sliding average)
   * @return Playback FPS; returns 0 when paused
   */
  float GetPlaybackFps() const;

protected:
  int ResolveId(int id) const { return id < 0 ? main_id_ : id; }

  std::map<int, std::unique_ptr<VideoSource>> sources_;
  int main_id_;

  std::function<void(std::map<int, std::shared_ptr<AVFrame>> &)> frame_callback_;
  float flicker_interval_s_ = 0.;
  std::function<void()> flicker_callback_;
  std::function<void()> resolution_changed_callback_;

  std::atomic<PlayState> state_;
  // Current playback position (seconds)
  std::atomic<float> current_position_s_;
  // Timestamp of last current_position_s_ update; saves current position on pause
  std::chrono::time_point<std::chrono::steady_clock> last_update_time_;
  // Timestamp of the last displayed frame
  float last_frame_time_s_ = -1.;
  // Timestamp of the last flicker
  float last_flicker_time_s_ = -1.;

  int last_width_ = -1, last_height_ = -1;

  int waitint_time_ms_ = kTooLongIntervalMS;

  std::atomic<bool> start_on_pause_{false};   // Indicates whether PlayerLoop should pause after submitting the first frame
  std::atomic<bool> frames_outdated_{true};    // Indicates whether PlayerLoop needs to fetch the next frame
  std::atomic<bool> step_forward_{true};      // Indicates the direction for PlayerLoop to fetch the next frame
  std::atomic<bool> position_updated_{false}; // Indicates whether PlayerLoop needs to update playback position from the fetched frame's timestamp

  // Playback FPS calculation (fixed time window statistics)
  static constexpr float kFpsWindowSeconds = 1.0f; // Statistics window length (seconds)
  std::atomic<float> playback_fps_{0.0f};
  std::chrono::time_point<std::chrono::steady_clock> fps_window_start_{};
  int fps_window_frame_count_ = 0; // Frame count in current window
  bool fps_warmup_ = true;         // Discard the first window's statistics after start/seek

  // Playback thread: periodically fetches next frame and calls frame_callback_ to display
  std::thread loop_thread_;
  void PlayerLoop();
};

} // namespace EYEQ
