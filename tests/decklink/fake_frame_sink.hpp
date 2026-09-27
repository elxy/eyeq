#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "output/frame_sink.hpp"

namespace EYEQ {
namespace test {

// Deterministic FrameSink double for CPU contract tests. It models a fixed device
// frame pool and an event queue. Completion/flushed event consumption is the only
// place a device frame budget is returned. It does not model SDK DMA or CPU access.
class FakeFrameSink final : public FrameSink {
public:
  enum class State { Running, Draining, Closed };

  explicit FakeFrameSink(int pool_slots);

  ScheduleOutcome TrySchedule(const PackedFrameView &view, const SubmissionInfo &info, int64_t display_time,
                              int64_t display_duration) override;
  StillOutcome DisplayStill(const PackedFrameView &view, const SubmissionInfo &info) override;
  bool PollEvent(OutputEvent *out) override;
  bool StartScheduled(int64_t start_time, std::string *error) override;
  bool StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) override;
  bool QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) override;
  CloseStatus Close(int drain_timeout_ms) override;

  // Test helpers: fault injection and inspection only. They never drive frame
  // accounting; budget is returned exclusively through PollEvent.
  void fail_next_before_consumption() { fail_before_consumption_ = true; }
  // Test-only clock control (the real sink advances it from the hardware clock).
  void set_clock(int64_t clock) { clock_ = clock; }
  void advance_clock(int64_t delta) { clock_ += delta; }
  bool playback_running() const { return running_; }
  State state() const { return state_; }
  int pool_slots() const { return pool_slots_; }
  int free_slots() const { return free_slots_; }
  int busy_count() const { return busy_count_; }
  int accepted_count() const { return accepted_count_; }
  int still_display_count() const { return still_display_count_; }
  int return_count(uint64_t submit_id) const;
  bool has_copy(uint64_t submit_id) const;
  const std::vector<uint8_t> *copy(uint64_t submit_id) const;
  const SubmissionInfo &last_info() const { return last_info_; }

private:
  int pool_slots_;
  int free_slots_;
  State state_ = State::Running;
  bool playback_started_ = false;
  bool fail_before_consumption_ = false;
  bool running_ = false;
  int64_t clock_ = 0;
  int busy_count_ = 0;
  int accepted_count_ = 0;
  int still_display_count_ = 0;
  SubmissionInfo last_info_{};
  std::deque<OutputEvent> events_;
  std::map<uint64_t, std::vector<uint8_t>> copies_;
  std::map<uint64_t, int> return_counts_;
};

} // namespace test
} // namespace EYEQ
