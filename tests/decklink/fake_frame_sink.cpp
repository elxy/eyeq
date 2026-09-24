#include "fake_frame_sink.hpp"

namespace EYEQ {
namespace test {

FakeFrameSink::FakeFrameSink(int pool_slots) : pool_slots_(pool_slots), free_slots_(pool_slots) {}

ScheduleOutcome FakeFrameSink::TrySchedule(const PackedFrameView &view, const SubmissionInfo &info,
                                           int64_t display_time, int64_t display_duration) {
  (void)display_duration;

  // Draining and Closed both reject without consuming. Draining is surfaced as
  // Closed because the sink interface has no separate draining outcome.
  if (state_ != State::Running) {
    return ScheduleOutcome::Closed;
  }
  if (fail_before_consumption_) {
    fail_before_consumption_ = false;
    return ScheduleOutcome::DeviceError;
  }
  if (view.data == nullptr || view.size == 0) {
    return ScheduleOutcome::Rejected;
  }
  if (free_slots_ == 0) {
    ++busy_count_;
    return ScheduleOutcome::Busy; // nothing consumed, nothing copied
  }

  --free_slots_;
  ++accepted_count_;
  last_info_ = info;
  copies_[info.submit_id].assign(view.data, view.data + view.size);

  OutputEvent event{};
  event.kind = OutputEvent::Kind::FrameCompleted;
  event.result = CompletionResult::Completed;
  event.submit_id = info.submit_id;
  event.batch_seq = info.batch_seq;
  event.timeline_epoch = info.timeline_epoch;
  event.output_index = info.output_index;
  event.source_frame_serial = info.source_frame_serial;
  event.display_time = display_time;
  events_.push_back(event);
  return ScheduleOutcome::Accepted;
}

StillOutcome FakeFrameSink::DisplayStill(const PackedFrameView &view, const SubmissionInfo &info) {
  // Draining and Closed reject without consuming.
  if (state_ != State::Running) {
    return StillOutcome::Closed;
  }
  // Scheduled playback must not be running: the SDK forbids DisplayVideoFrameSync
  // while scheduled playback is active. Surface the contract violation.
  if (running_) {
    return StillOutcome::DeviceError;
  }
  if (view.data == nullptr || view.size == 0) {
    return StillOutcome::DeviceError;
  }
  // Dedicated still frame: take an independent copy, consume no pool budget, and
  // produce no scheduled completion event.
  copies_[info.submit_id].assign(view.data, view.data + view.size);
  ++still_display_count_;
  return StillOutcome::Displayed;
}

bool FakeFrameSink::PollEvent(OutputEvent *out) {
  if (out == nullptr || events_.empty()) {
    return false;
  }
  *out = events_.front();
  events_.pop_front();
  if (out->kind == OutputEvent::Kind::FrameCompleted) {
    // The single budget return point.
    ++free_slots_;
    ++return_counts_[out->submit_id];
  }
  return true;
}

bool FakeFrameSink::StartScheduled(int64_t start_time, std::string *error) {
  if (state_ != State::Running) {
    if (error != nullptr) {
      *error = "sink is not running";
    }
    return false;
  }
  running_ = true;
  playback_started_ = true;
  clock_ = start_time;
  return true;
}

bool FakeFrameSink::StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) {
  (void)error;
  running_ = false;
  clock_ = stop_at + 7; // device stops slightly later
  if (actual_stop != nullptr) {
    *actual_stop = clock_;
  }
  return true;
}

bool FakeFrameSink::QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) {
  (void)time_scale;
  (void)error;
  if (stream_time != nullptr) {
    *stream_time = clock_;
  }
  if (speed != nullptr) {
    *speed = running_ ? 1.0 : 0.0;
  }
  return true;
}

CloseStatus FakeFrameSink::Close(int drain_timeout_ms) {
  (void)drain_timeout_ms;
  if (state_ == State::Closed) {
    return CloseStatus::Closed;
  }
  if (free_slots_ == pool_slots_) {
    state_ = State::Closed;
    return CloseStatus::Closed;
  }
  state_ = State::Draining;
  return CloseStatus::DrainTimeout;
}

int FakeFrameSink::return_count(uint64_t submit_id) const {
  auto it = return_counts_.find(submit_id);
  return it == return_counts_.end() ? 0 : it->second;
}

bool FakeFrameSink::has_copy(uint64_t submit_id) const { return copies_.count(submit_id) != 0; }

const std::vector<uint8_t> *FakeFrameSink::copy(uint64_t submit_id) const {
  auto it = copies_.find(submit_id);
  return it == copies_.end() ? nullptr : &it->second;
}

} // namespace test
} // namespace EYEQ
