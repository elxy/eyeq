// Drives the real DeckLinkOutput backend through RunTestSignal against the local
// fake SDK. No device, driver or GPU is touched and the real dispatch is not linked.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "DeckLinkAPI.h"
#include "fake_decklink_output.hpp"
#include "output/decklink_backend.hpp"
#include "test_pattern.hpp"
#include "test_signal_session.hpp"

using namespace EYEQ;
using eyeq_decklink_test::LastScheduledBytes;
using eyeq_decklink_test::LiveObjectCount;
using eyeq_decklink_test::OutputFault;
using eyeq_decklink_test::OutputStats;
using eyeq_decklink_test::ResetFakeOutput;
using eyeq_decklink_test::SetCompletionGate;
using eyeq_decklink_test::SetCreateFrameFailAt;
using eyeq_decklink_test::SetDisplayMode;
using eyeq_decklink_test::SetOutputFault;
using eyeq_decklink_test::SetScheduleReentrant;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

// 1080p keeps the sanitized run cheap; the whitelist accepts 1920x1080 and
// 3840x2160, and the fake would otherwise advertise 4K.
void Prepare() {
  ResetFakeOutput();
  SetDisplayMode(1920, 1080, 1000, 50000, true);
  EYEQ::test_signal_stop_requested = 0;
}

OutputRequest MakeRequest(VideoConnection connection = VideoConnection::SDI) {
  OutputRequest request;
  request.device.index = 0;
  request.mode = MonitorMode{1920, 1080, 1000, 50000, true};
  request.connection = connection;
  request.pixel_format = PixelFormat::V210;
  return request;
}

ResolvedOutput OpenDefault(DeckLinkOutput &output, VideoConnection connection = VideoConnection::SDI) {
  ResolvedOutput resolved;
  std::string error;
  CHECK(output.Open(MakeRequest(connection), &resolved, &error));
  CHECK(error.empty());
  return resolved;
}

TestSignalOptions MakeOptions(uint64_t frames, uint64_t preroll, int timeout_ms = 60000) {
  TestSignalOptions options;
  options.frame_count = frames;
  options.preroll_frames = preroll;
  options.drain_timeout_ms = timeout_ms;
  return options;
}

// Test-only seam that returns Busy once for a chosen submit_id and delegates
// everything else, so the session's retry path can be exercised deterministically
// even though the fake completes snapshots synchronously.
class BusyOnceSink final : public FrameSink {
public:
  BusyOnceSink(FrameSink &inner, uint64_t busy_submit_id) : inner_(inner), busy_submit_id_(busy_submit_id) {}

  ScheduleOutcome TrySchedule(const PackedFrameView &view, const SubmissionInfo &info, int64_t display_time,
                              int64_t display_duration) override {
    if (info.submit_id == busy_submit_id_) {
      busy_submit_id_ = std::numeric_limits<uint64_t>::max();
      ++busy_count_;
      return ScheduleOutcome::Busy;
    }
    return inner_.TrySchedule(view, info, display_time, display_duration);
  }
  StillOutcome DisplayStill(const PackedFrameView &view, const SubmissionInfo &info) override {
    return inner_.DisplayStill(view, info);
  }
  bool PollEvent(OutputEvent *out) override { return inner_.PollEvent(out); }
  bool StartScheduled(int64_t start_time, std::string *error) override {
    return inner_.StartScheduled(start_time, error);
  }
  bool StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) override {
    return inner_.StopScheduled(stop_at, actual_stop, error);
  }
  bool QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) override {
    return inner_.QueryClock(time_scale, stream_time, speed, error);
  }
  CloseStatus Close(int drain_timeout_ms) override { return inner_.Close(drain_timeout_ms); }

  int busy_count() const { return busy_count_; }

private:
  FrameSink &inner_;
  uint64_t busy_submit_id_;
  int busy_count_ = 0;
};

void TestRunSchedulesAndCompletes() {
  Prepare();
  SetScheduleReentrant(true);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(RunTestSignal(output, resolved, MakeOptions(3, 4), &result, &error));
    CHECK(error.empty());
    CHECK(result.submitted == 3);
    CHECK(result.completed == 3);
    CHECK(result.late == 0 && result.dropped == 0 && result.flushed == 0);
    CHECK(result.started);
    CHECK(OutputStats().start_calls == 1);
    CHECK(OutputStats().schedule_calls == 3);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 1);
  }
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

void TestLongSequenceStaysContinuous() {
  Prepare();
  SetScheduleReentrant(true);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(RunTestSignal(output, resolved, MakeOptions(20, 4), &result, &error));
    CHECK(error.empty());
    CHECK(result.submitted == 20);
    CHECK(result.completed == 20);
    CHECK(OutputStats().schedule_calls == 20);
    CHECK(output.unreconciled_frame_count() == 0);
    std::vector<uint8_t> expected(resolved.layout.image_bytes, 0);
    RenderTestPattern(19, resolved.layout, resolved.color.encoding, expected.data());
    CHECK(LastScheduledBytes() == expected);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestInjectedBusyRetriesSameFrame() {
  Prepare();
  SetScheduleReentrant(true);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    BusyOnceSink sink(output, 3);
    TestSignalResult result;
    std::string error;
    CHECK(RunTestSignal(sink, resolved, MakeOptions(6, 2), &result, &error));
    CHECK(error.empty());
    CHECK(sink.busy_count() == 1);
    CHECK(result.submitted == 6);
    CHECK(result.completed == 6);
    // Busy consumed nothing: one native schedule call per accepted frame.
    CHECK(OutputStats().schedule_calls == 6);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestStillPath() {
  Prepare();
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    std::vector<uint8_t> staging(resolved.layout.image_bytes, 0);
    RenderTestPattern(0, resolved.layout, resolved.color.encoding, staging.data());
    SubmissionInfo info;
    info.submit_id = 0;
    CHECK(output.DisplayStill({staging.data(), staging.size()}, info) == StillOutcome::Displayed);
    CHECK(OutputStats().display_sync_calls == 1);
    CHECK(OutputStats().start_calls == 0);
    CHECK(OutputStats().schedule_calls == 0);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestInvalidLayoutRejected() {
  Prepare();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved = OpenDefault(output);
    resolved.layout.row_bytes = 0; // corrupt the resolved layout
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(1, 1), &result, &error));
    CHECK(error.find("layout") != std::string::npos);
    CHECK(OutputStats().schedule_calls == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestScheduleRejectsFailsHonestly() {
  Prepare();
  SetOutputFault(OutputFault::ScheduleRejects);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(3, 4), &result, &error));
    CHECK(!error.empty());
    CHECK(result.submitted == 0);
    CHECK(result.completed == 0);
    CHECK(!result.started);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
  SetOutputFault(OutputFault::None);
}

void TestBusyStatusRefusesBeforeEnable() {
  Prepare();
  SetOutputFault(OutputFault::Busy);
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(error.find("busy") != std::string::npos);
    CHECK(OutputStats().enable_calls == 0);
    CHECK(OutputStats().disable_calls == 0);
    CHECK(OutputStats().status_calls == 1);
  }
  SetOutputFault(OutputFault::None);
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

void TestStatusMissingRefusesBeforeEnable() {
  Prepare();
  SetOutputFault(OutputFault::StatusMissing);
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(error.find("IDeckLinkStatus") != std::string::npos);
    CHECK(OutputStats().enable_calls == 0);
    CHECK(OutputStats().disable_calls == 0);
  }
  SetOutputFault(OutputFault::None);
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

void TestStatusQueryFailsRefusesBeforeEnable() {
  Prepare();
  SetOutputFault(OutputFault::StatusQueryFails);
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(error.find("bmdDeckLinkStatusBusy") != std::string::npos);
    CHECK(OutputStats().enable_calls == 0);
    CHECK(OutputStats().disable_calls == 0);
    CHECK(OutputStats().status_calls == 1);
  }
  SetOutputFault(OutputFault::None);
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

void TestHdmiWiring() {
  Prepare();
  SetScheduleReentrant(true);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output, VideoConnection::HDMI);
    CHECK(resolved.connection == VideoConnection::HDMI);
    CHECK(OutputStats().support_connection == static_cast<int>(bmdVideoConnectionHDMI));
    TestSignalResult result;
    std::string error;
    CHECK(RunTestSignal(output, resolved, MakeOptions(2, 2), &result, &error));
    CHECK(result.submitted == 2);
    CHECK(result.completed == 2);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void DelayTwentyMs(void *) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

// Total runtime is deliberately above the stall timeout; per-frame progress is not,
// so the run must not be truncated by a whole-run deadline.
void TestStallTimeoutIsPerProgress() {
  Prepare();
  SetScheduleReentrant(true);
  SetCompletionGate(DelayTwentyMs, nullptr);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(RunTestSignal(output, resolved, MakeOptions(6, 4, 100), &result, &error));
    CHECK(error.empty());
    CHECK(result.submitted == 6);
    CHECK(result.completed == 6);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
  SetCompletionGate(nullptr, nullptr);
}

// The completion gate runs on the consumer thread and only sets the stop flag, the
// same thing a SIGINT/SIGTERM handler is allowed to do.
void RequestStopFromGate(void *) { EYEQ::test_signal_stop_requested = 1; }

void TestInterruptStopsSubmitting() {
  Prepare();
  SetScheduleReentrant(true);
  SetCompletionGate(RequestStopFromGate, nullptr);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(20, 4), &result, &error));
    CHECK(error.rfind("interrupted", 0) == 0);
    CHECK(result.submitted == 1);
    CHECK(result.submitted < 20);
    CHECK(!result.started);
    // A proper finalization (as the CLI does) still observes the completion that
    // Close retains instead of reporting a stale zero.
    CHECK(output.Close(0) == CloseStatus::Closed);
    OutputEvent event;
    bool saw_completion = false;
    while (output.PollEvent(&event)) {
      if (event.kind == OutputEvent::Kind::FrameCompleted) {
        saw_completion = true;
      }
    }
    CHECK(saw_completion);
    CHECK(output.unreconciled_frame_count() == 0);
  }
  SetCompletionGate(nullptr, nullptr);
  EYEQ::test_signal_stop_requested = 0;
}

void TestInterruptBeforeFirstFrame() {
  Prepare();
  EYEQ::test_signal_stop_requested = 1;
  SetScheduleReentrant(true);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(5, 4), &result, &error));
    CHECK(error.rfind("interrupted", 0) == 0);
    CHECK(result.submitted == 0);
    CHECK(OutputStats().schedule_calls == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
  EYEQ::test_signal_stop_requested = 0;
}

// A real device error must not be reclassified as an intentional interruption just
// because the stop flag happened to be set.
void TestScheduleErrorIsNotInterruption() {
  Prepare();
  SetScheduleReentrant(true);
  SetCompletionGate(RequestStopFromGate, nullptr);
  SetOutputFault(OutputFault::ScheduleFails);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(3, 4), &result, &error));
    CHECK(error.rfind("interrupted", 0) != 0);
    CHECK(error.find("ScheduleVideoFrame failed") != std::string::npos);
    CHECK(error.find("stage=") != std::string::npos);
    CHECK(error.find("status=") != std::string::npos);
    CHECK(result.submitted == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
  SetCompletionGate(nullptr, nullptr);
  SetOutputFault(OutputFault::None);
  EYEQ::test_signal_stop_requested = 0;
}

// The backend can return Closed while retaining a real error (here a failed native
// stop); a post-Close drain must still surface it.
void TestStopFailureRetainedAfterClose() {
  Prepare();
  SetScheduleReentrant(true);
  SetOutputFault(OutputFault::StopFails);
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(RunTestSignal(output, resolved, MakeOptions(1, 1), &result, &error));
    CHECK(result.completed == 1);
    CHECK(output.Close(0) == CloseStatus::Closed);
    OutputEvent event;
    bool saw_stop_error = false;
    while (output.PollEvent(&event)) {
      if (event.kind == OutputEvent::Kind::Error && event.stage == OutputEvent::Stage::Close &&
          std::string(event.message).find("StopScheduledPlayback") != std::string::npos) {
        saw_stop_error = true;
      }
    }
    CHECK(saw_stop_error);
  }
  SetOutputFault(OutputFault::None);
}

// A failed Open whose rollback Disable also failed leaves a retryable session. The
// tool must still attempt Close and surface the earlier shutdown error.
void TestOpenRollbackFailureThenSuccessfulClose() {
  Prepare();
  SetCreateFrameFailAt(1);
  SetOutputFault(OutputFault::DisableFails);
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(error.find("CreateVideoFrame") != std::string::npos);
    CHECK(OutputStats().enable_calls == 1);
    // Clear the transient fault: the retryable session must now close.
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
    OutputEvent event;
    bool saw_shutdown_error = false;
    while (output.PollEvent(&event)) {
      if (event.kind == OutputEvent::Kind::Error && event.stage == OutputEvent::Stage::Close) {
        saw_shutdown_error = true;
      }
    }
    CHECK(saw_shutdown_error);
  }
  SetOutputFault(OutputFault::None);
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

// The session must reject a full-pool preroll without waiting for completions that
// cannot happen before StartScheduled: playback starts early instead of deadlocking.
void TestFullPoolBeforePrerollStartsEarly() {
  Prepare();
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(20, 9, 100), &result, &error));
    CHECK(OutputStats().start_calls == 1); // started early, no pre-Start deadlock
    CHECK(result.submitted == 8);          // the whole pool
    CHECK(result.started);
    CHECK(error.find("no frame budget") != std::string::npos);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

// With playback already started and no completions arriving, the session reports a
// bounded no-progress timeout instead of spinning forever.
void TestNoProgressTimeout() {
  Prepare();
  {
    DeckLinkOutput output;
    const ResolvedOutput resolved = OpenDefault(output);
    TestSignalResult result;
    std::string error;
    CHECK(!RunTestSignal(output, resolved, MakeOptions(20, 1, 100), &result, &error));
    CHECK(result.started);
    CHECK(result.submitted == 8);
    CHECK(error.find("no frame budget") != std::string::npos);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

} // namespace

int main() {
  TestRunSchedulesAndCompletes();
  TestLongSequenceStaysContinuous();
  TestInjectedBusyRetriesSameFrame();
  TestStillPath();
  TestInvalidLayoutRejected();
  TestScheduleRejectsFailsHonestly();
  TestBusyStatusRefusesBeforeEnable();
  TestStatusMissingRefusesBeforeEnable();
  TestStatusQueryFailsRefusesBeforeEnable();
  TestHdmiWiring();
  TestStallTimeoutIsPerProgress();
  TestInterruptStopsSubmitting();
  TestInterruptBeforeFirstFrame();
  TestScheduleErrorIsNotInterruption();
  TestStopFailureRetainedAfterClose();
  TestOpenRollbackFailureThenSuccessfulClose();
  TestFullPoolBeforePrerollStartsEarly();
  TestNoProgressTimeout();
  std::printf("decklink_test_signal: OK\n");
  return 0;
}
