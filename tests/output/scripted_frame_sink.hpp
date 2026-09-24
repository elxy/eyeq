#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

#include "output/frame_sink.hpp"

namespace eyeq_test {

// Decorates a FrameSink to inject DisplayStill outcomes, PollEvent results and
// Close statuses. Everything that is not scripted is forwarded to the wrapped
// sink, so the real copy/inspection behaviour of FakeFrameSink is preserved and
// the frozen fake itself does not need to change.
//
// A scripted DisplayStill outcome is returned WITHOUT forwarding, so a scripted
// Busy/DeviceError/Closed really models "the sink consumed nothing".
class ScriptedFrameSink final : public EYEQ::FrameSink {
public:
  explicit ScriptedFrameSink(EYEQ::FrameSink &inner) : inner_(&inner) {}

  void ScriptStill(EYEQ::StillOutcome outcome, int times = 1) {
    for (int i = 0; i < times; ++i) {
      still_script_.push_back(outcome);
    }
  }
  void ScriptClose(EYEQ::CloseStatus status, int times = 1) {
    for (int i = 0; i < times; ++i) {
      close_script_.push_back(status);
    }
  }
  void ScriptEvent(const EYEQ::OutputEvent &event) { event_script_.push_back(event); }

  // Inspection of the DisplayStill calls the composition layer made.
  int still_calls() const { return still_calls_; }
  int still_forwarded() const { return still_forwarded_; }
  bool still_views_consistent() const { return still_views_consistent_; }
  const std::vector<uint8_t> &last_bytes() const { return last_bytes_; }
  const EYEQ::SubmissionInfo &last_info() const { return last_info_; }

  EYEQ::ScheduleOutcome TrySchedule(const EYEQ::PackedFrameView &view, const EYEQ::SubmissionInfo &info,
                                    int64_t display_time, int64_t display_duration) override {
    return inner_->TrySchedule(view, info, display_time, display_duration);
  }

  EYEQ::StillOutcome DisplayStill(const EYEQ::PackedFrameView &view, const EYEQ::SubmissionInfo &info) override {
    ++still_calls_;
    if (first_view_) {
      first_view_ = false;
    } else if (last_bytes_.size() != view.size || !std::equal(last_bytes_.begin(), last_bytes_.end(), view.data)) {
      still_views_consistent_ = false;
    }
    last_bytes_.assign(view.data, view.data + view.size);
    last_info_ = info;

    if (!still_script_.empty()) {
      const EYEQ::StillOutcome outcome = still_script_.front();
      still_script_.pop_front();
      return outcome;
    }
    ++still_forwarded_;
    return inner_->DisplayStill(view, info);
  }

  bool PollEvent(EYEQ::OutputEvent *out) override {
    if (!event_script_.empty()) {
      *out = event_script_.front();
      event_script_.pop_front();
      return true;
    }
    return inner_->PollEvent(out);
  }

  EYEQ::CloseStatus Close(int drain_timeout_ms) override {
    if (!close_script_.empty()) {
      const EYEQ::CloseStatus status = close_script_.front();
      close_script_.pop_front();
      if (status == EYEQ::CloseStatus::Closed) {
        inner_->Close(0); // keep the wrapped sink consistent when we claim closed
      }
      return status;
    }
    return inner_->Close(drain_timeout_ms);
  }

  bool StartScheduled(int64_t start_time, std::string *error) override {
    return inner_->StartScheduled(start_time, error);
  }
  bool StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) override {
    return inner_->StopScheduled(stop_at, actual_stop, error);
  }
  bool QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) override {
    return inner_->QueryClock(time_scale, stream_time, speed, error);
  }

private:
  EYEQ::FrameSink *inner_;
  std::deque<EYEQ::StillOutcome> still_script_;
  std::deque<EYEQ::CloseStatus> close_script_;
  std::deque<EYEQ::OutputEvent> event_script_;

  int still_calls_ = 0;
  int still_forwarded_ = 0;
  bool first_view_ = true;
  bool still_views_consistent_ = true;
  std::vector<uint8_t> last_bytes_;
  EYEQ::SubmissionInfo last_info_{};
};

} // namespace eyeq_test
