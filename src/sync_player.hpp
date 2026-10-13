#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/fmt/fmt.h>
extern "C" {
#include <libavutil/frame.h>
}

#include "log.hpp"
#include "video_source.hpp"

namespace EYEQ {

/**
 * @brief Presentation data captured at the instant a frame is fetched.
 *
 * Metadata is bound to the AVFrame here, before any later seek/reset can move
 * the source cursor, so a published snapshot can never label an old frame with
 * a newer serial.
 */
struct FetchedFrame {
  std::shared_ptr<AVFrame> frame;
  int serial = -1;
  int total_frames = -1;
  char pict_type = '?';
  float pts_s = 0.0f;
  int64_t pts = 0;
};

using FrameSet = std::map<int, FetchedFrame>;

/**
 * @brief Presentation data published to the UI thread (see FrameMetadata/FrameSet).
 */
struct FrameMetadata {
  int video_id = -1;
  char pict_type = '?';
  int frame_serial = -1;
  int total_frames = -1;
  float pts_s = 0.0f;
};

/**
 * @brief One complete frame publication.
 *
 * `frames` holds shared_ptr<AVFrame> references, so consuming the snapshot keeps
 * the frame data alive independent of the decode cursor.
 */
struct FrameSnapshot {
  uint64_t generation = 0;
  int main_id = -1;
  std::map<int, std::shared_ptr<AVFrame>> frames;
  std::map<int, FrameMetadata> metadata;
  float current_time_s = 0.0f;
  float start_time_s = std::numeric_limits<float>::quiet_NaN();
  float end_time_s = std::numeric_limits<float>::quiet_NaN();
  float playback_fps = 0.0f;
  float main_fps = 0.0f;
};

// Cursor operations are queued and executed by the PlayerLoop thread, which is
// the only owner of the frame consumption cursor. A command's outcome is
// reported only at its real completion point (frames fetched and a consistent
// snapshot published, or a definite failure/boundary).
enum class ControlKind {
  Pause,
  Play,
  TogglePause,
  SeekTo,
  SeekOffset,
  StepNext,
  StepPrev,
  StepNextSingle,
  StepPrevSingle,
  SeekOffsetSingle,
  ResetVideoOffset,
  ResetAllVideoOffsets,
  RequestFormatFallback,
};

enum class ControlStatus {
  Ok,        // completed successfully
  Boundary,  // reached start/end of a stream (not an error)
  Failed,    // execution failed; see message
  Cancelled, // the player was stopped before completion
  Rejected,  // never accepted; the bounded queue was full
};

struct ControlCommand {
  uint64_t request_id = 0;
  ControlKind kind = ControlKind::Pause;
  float offset_s = 0.0f;
  int video_id = -1;
  std::vector<int> video_ids; // RequestFormatFallback
};

struct ControlOutcome {
  uint64_t request_id = 0;
  ControlKind kind = ControlKind::Pause;
  ControlStatus status = ControlStatus::Ok;
  std::string message;
  // Generation of the snapshot this command published (0 when it published
  // nothing). Lets the UI correlate frames with the command that produced them.
  uint64_t snapshot_generation = 0;
};

/**
 * @class SyncVideoPlayer
 * @brief Multi-video synchronized player, coordinates all video decoders with the master clock
 *        The first added video stream is used as the reference stream; other streams fetch frames relative to it.
 * Assumes all video streams share the same frame rate
 *
 * Threading contract: PlayerLoop() is the single owner of the frame consumption
 * cursor (current_frames_/video_offsets_) and is the only caller of the
 * VideoSource get/seek methods. The UI thread publishes commands through
 * SubmitControl() and consumes FrameSnapshot / ControlOutcome objects. Commands
 * are executed strictly in order, one at a time, and each accepted command gets
 * exactly one ControlOutcome.
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

  // Iterator support for range-based for loops over video sources
  auto begin() { return sources_.begin(); }
  auto end() { return sources_.end(); }
  auto begin() const { return sources_.begin(); }
  auto end() const { return sources_.end(); }

  int Width(int id = -1);
  int Height(int id = -1);
  float FrameRate(int id = -1);
  float StartTime(int id = -1);
  float EndTime(int id = -1);

  /**
   * @brief Start every decoder and position it at the requested time/frame.
   *        Must be called before Start(). Start failures propagate as exceptions.
   */
  void StartSources(float seek_to = -1., size_t seek_to_frame = -1);

  /**
   * @brief Start the player loop
   *
   * @param pause Whether to pause after the first frame has been published
   */
  void Start(bool pause);

  /**
   * @brief Stop playback, cancel any in-flight control request and resolve every
   *        accepted-but-unfinished command as Cancelled.
   */
  void Stop();

  PlayState State() const { return state_.load(); }

  // Cursor control commands. Each returns its request id; results are reported
  // through TakeControlOutcomes(), exactly one outcome per submission.
  uint64_t Play();
  uint64_t Pause();
  uint64_t InvertPause();
  uint64_t SeekTo(float time_s);
  uint64_t SeekOffset(float offset_s);
  uint64_t StepNextFrame();
  uint64_t StepPrevFrame();
  uint64_t StepNextFrameSingle(int video_id);
  uint64_t StepPrevFrameSingle(int video_id);
  uint64_t SeekOffsetSingle(int video_id, float offset_s);
  uint64_t ResetVideoOffset(int video_id);
  uint64_t ResetAllVideoOffsets();
  uint64_t RequestFormatFallback(const std::vector<int> &video_ids);

  /**
   * @brief Flicker interval in seconds; <= 0 disables flicker
   */
  void SetFlickerInterval(float interval_s) { flicker_interval_s_ = interval_s; }

  /**
   * @brief Get the newest frame publication not yet consumed (coalesced slot).
   * @return nullptr when no newer snapshot exists.
   */
  std::shared_ptr<const FrameSnapshot> TakeSnapshot();

  /**
   * @brief Drain the queued control outcomes (one per accepted command).
   */
  std::vector<ControlOutcome> TakeControlOutcomes();

  /**
   * @brief Consume the number of flicker events since the last call
   */
  uint32_t ConsumeFlickerEvents() { return flicker_events_.exchange(0); }

  /**
   * @brief Consume the resolution-changed notification since the last call
   */
  bool ConsumeResolutionChanged() { return resolution_changed_.exchange(false); }

  /**
   * @brief Consume an unrecoverable player failure (decode/rollback failure).
   */
  bool ConsumeFatalError(std::string *message);

  /**
   * @brief Get current playback time
   * @return Current playback time (seconds)
   */
  float CurrentTime() const { return current_position_s_.load(); }

  /**
   * @brief Check for video streams where hardware decoding was requested but not used
   * @return List of video stream IDs not using hardware decoding
   */
  std::vector<int> CheckUnusedHardwareDecoders() const;

  /**
   * @brief Get video source by ID
   * @param id Video ID
   * @return Pointer to the video source
   */
  VideoSource *GetVideoSource(int id) { return sources_.at(id).get(); }

protected:
  int ResolveId(int id) const { return id < 0 ? main_id_ : id; }

  struct VideoOffset {
    int frame_offset = 0; // Frame offset relative to main video (positive = ahead)
  };

  std::map<int, std::unique_ptr<VideoSource>> sources_;
  int main_id_;

  std::atomic<PlayState> state_;
  // Current playback position (seconds)
  std::atomic<float> current_position_s_;
  std::chrono::time_point<std::chrono::steady_clock> last_update_time_;
  // Timestamp of the last displayed frame
  float last_frame_time_s_ = -1.;
  // Timestamp of the last flicker
  float last_flicker_time_s_ = -1.;
  // Flicker interval (seconds); <= 0 disables it
  float flicker_interval_s_ = 0.;

  int last_width_ = -1, last_height_ = -1;
  bool start_on_pause_ = false; // PlayerLoop-only

  // Playback FPS calculation (fixed time window statistics)
  static constexpr float kFpsWindowSeconds = 1.0f;
  std::atomic<float> playback_fps_{0.0f};
  std::chrono::time_point<std::chrono::steady_clock> fps_window_start_{};
  int fps_window_frame_count_ = 0; // PlayerLoop-only
  bool fps_warmup_ = true;         // PlayerLoop-only

  std::thread loop_thread_;
  void PlayerLoop();

  // PlayerLoop-owned state: the last complete, committed frame set and the
  // per-video offsets. No other thread reads or writes these.
  FrameSet current_frames_;
  std::map<int, VideoOffset> video_offsets_;
  uint64_t last_published_generation_ = 0;

  // Bounded control ingress. Accepted-but-unfinished commands stay in this
  // single queue until PlayerLoop completes them; nothing is moved to another
  // container.
  static constexpr size_t kMaxPendingControls = 64;
  // Total outstanding-work budget: queued + executing + undelivered outcomes.
  // The ingress rejects once the budget is exhausted, so no accepted request's
  // outcome can ever be dropped. (A rejected submission itself still gets one
  // terminal result and may push the undelivered count past the budget; that is
  // the caller's own backlog, not lost accepted work.)
  static constexpr size_t kMaxOutstandingWork = 256;
  std::mutex control_mutex_;
  std::condition_variable control_cv_;
  std::atomic<bool> closing_{false}; // set before draining the queue in Stop()
  std::deque<ControlCommand> control_commands_;
  std::atomic<size_t> executing_{0}; // PlayerLoop-only: command currently running
  std::atomic<uint64_t> next_request_id_{0};
  std::mutex outcome_mutex_;
  std::deque<ControlOutcome> control_outcomes_;
  std::atomic<size_t> outstanding_outcomes_{0}; // undelivered terminal results

  // Single-slot publication mailbox. Publishing overwrites the previous slot
  // (bounded, merge semantics for preview); the UI takes the newest snapshot.
  std::mutex snapshot_mutex_;
  std::shared_ptr<const FrameSnapshot> latest_snapshot_;
  std::atomic<uint64_t> snapshot_generation_{0};
  uint64_t taken_generation_ = 0; // UI thread only

  // Notifications produced by PlayerLoop and consumed by the UI thread
  std::atomic<uint32_t> flicker_events_{0};
  std::atomic<bool> resolution_changed_{false};

  // Unrecoverable failure from PlayerLoop, consumed by the UI thread
  std::atomic<bool> fatal_error_{false};
  std::mutex fatal_mutex_;
  std::string fatal_message_;

  // Submit a command. Always returns the request id: every submission is
  // answered by exactly one outcome (Ok/Boundary/Failed/Cancelled/Rejected).
  uint64_t SubmitControl(ControlCommand cmd);
  void RecordOutcome(ControlOutcome outcome);
  ControlOutcome ExecuteControl(const ControlCommand &cmd);
  void SetFatalError(const std::string &message);

  // Fetch one frame from every source into `out`, binding metadata at fetch
  // time. On any failure the already-fetched sources are rolled back so the
  // committed state and the source cursors stay consistent, then the exception
  // is propagated.
  void FetchCompleteSet(bool forward, FrameSet &out);
  void RollbackFetched(const std::vector<int> &fetched_ids, bool forward);
  // Fetch a complete set if the committed one is incomplete (used before
  // single-video operations so an incomplete set is never published).
  void EnsureCompleteFrameSet();
  uint64_t PublishFrameSet(const FrameSet &set);
  void CommitFrameSet(FrameSet &&set, bool update_position);
  void PlaybackStep();

  void DoSeek(float time_s);
  void ResetVideoOffsetImpl(int video_id);
  void ResetAllVideoOffsetsImpl();
  void SeekOffsetSingleImpl(int video_id, float offset_s);
};

} // namespace EYEQ
