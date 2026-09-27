// Deterministic output-backend tests driving the real DeckLinkOutput state machine
// against the local fake SDK. No device, driver or GPU is touched.
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "DeckLinkAPI.h"
#include "fake_decklink_output.hpp"
#include "output/decklink_backend.hpp"

using namespace EYEQ;
using eyeq_decklink_test::BuffersReleasedWhileAccessing;
using eyeq_decklink_test::DeliverCompletion;
using eyeq_decklink_test::DeliverPlaybackStopped;
using eyeq_decklink_test::LiveObjectCount;
using eyeq_decklink_test::MetadataSets;
using eyeq_decklink_test::OutputFault;
using eyeq_decklink_test::OutputStats;
using eyeq_decklink_test::ResetFakeOutput;
using eyeq_decklink_test::SetConnectionQueryFails;
using eyeq_decklink_test::SetCreateFrameFailAt;
using eyeq_decklink_test::SetDisplayMode;
using eyeq_decklink_test::SetGetSizeFailAt;
using eyeq_decklink_test::SetMetadataQueryFailAt;
using eyeq_decklink_test::SetNativeRunning;
using eyeq_decklink_test::SetOutputFault;
using eyeq_decklink_test::SetScheduleReentrant;
using eyeq_decklink_test::SetStoppedNotificationsOnDisable;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

OutputRequest MakeRequest() {
  OutputRequest request;
  request.device.index = 0;
  request.mode = MonitorMode{3840, 2160, 1000, 50000, true};
  request.connection = VideoConnection::SDI;
  request.pixel_format = PixelFormat::V210;
  return request;
}

OutputRequest MakePqRequest() {
  OutputRequest request = MakeRequest();
  request.color.encoding.primaries = ColorPrimaries::BT2020;
  request.color.encoding.transfer = ColorTransfer::PQ;
  request.hdr.hdr_signaling = true;
  request.hdr.primaries = ColorPrimaries::BT2020;
  request.hdr.transfer = ColorTransfer::PQ;
  return request;
}

std::vector<uint8_t> MakeBytes(std::size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i);
  }
  return bytes;
}

PackedFrameView ViewOf(const std::vector<uint8_t> &bytes) { return PackedFrameView{bytes.data(), bytes.size()}; }

SubmissionInfo MakeInfo(uint64_t submit_id) {
  SubmissionInfo info;
  info.submit_id = submit_id;
  info.source_frame_serial = submit_id + 1000;
  return info;
}

bool PollOnce(DeckLinkOutput &output, OutputEvent *event) { return output.PollEvent(event); }

// Drains events until one of the requested kind appears. Used where a non-fatal
// diagnostic is queued ahead of a completion/stop event by design.
bool PollUntilKind(DeckLinkOutput &output, OutputEvent::Kind kind, OutputEvent *out, int max_polls = 8) {
  for (int i = 0; i < max_polls; ++i) {
    if (!output.PollEvent(out)) {
      return false;
    }
    if (out->kind == kind) {
      return true;
    }
  }
  return false;
}

// Opens the output and returns the resolved layout byte count.
std::size_t OpenDefault(DeckLinkOutput &output) {
  ResolvedOutput resolved;
  std::string error;
  CHECK(output.Open(MakeRequest(), &resolved, &error));
  CHECK(error.empty());
  CHECK(resolved.connection == VideoConnection::SDI);
  CHECK(resolved.layout.width == 3840 && resolved.layout.height == 2160);
  return resolved.layout.image_bytes;
}

void TestOpenCloseDisablesEvenWithZeroFrames() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OpenDefault(output);
    CHECK(OutputStats().enable_calls == 1);
    CHECK(OutputStats().create_frame_calls == 9); // 8 pool + 1 still
    CHECK(OutputStats().set_callback_calls == 1);
    CHECK(OutputStats().row_bytes_calls == 1);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 1);       // zero frames still disables
    CHECK(output.Close(0) == CloseStatus::Closed); // idempotent
    CHECK(OutputStats().disable_calls == 1);
  }
}

void TestOpenInvalidRequestDoesNoNativeCalls() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OutputRequest request = MakeRequest();
    request.mode.is_progressive = false; // interlaced rejected by ValidateRequest
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(request, &resolved, &error));
    CHECK(!error.empty());
    CHECK(OutputStats().enable_calls == 0);
    CHECK(OutputStats().create_frame_calls == 0);
    CHECK(OutputStats().schedule_calls == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestOpenRejectsUnsupportedModeBeforeEnable() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::SupportFalse);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().enable_calls == 0);
    CHECK(OutputStats().create_frame_calls == 0);
  }
}

void TestOpenRejectsMissingMode() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::ModeMissing);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().enable_calls == 0);
  }
}

void TestOpenRejectsUnsupportedHdr() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::HdrUnsupported);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakePqRequest(), &resolved, &error));
    CHECK(OutputStats().enable_calls == 0); // HDR gate is checked before Enable
  }
}

void TestOpenRollsBackEachFailure() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::EnableFails);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().enable_calls == 1);
    CHECK(OutputStats().disable_calls == 0); // never enabled -> never disables
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::SetCallbackFails);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().disable_calls == 1); // enabled then rolled back
  }
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::CreateFrameFails);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().disable_calls == 1);
  }
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::ConnectionMissing);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().enable_calls == 1);
    CHECK(OutputStats().disable_calls == 1); // enabled then connection check failed
  }
}

void TestAcceptedCopiesBytesAndReturnsCompletion() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x11);
    SetScheduleReentrant(false);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(OutputStats().start_access_calls == 1);
    CHECK(OutputStats().end_access_calls == 1);
    CHECK(OutputStats().set_flags_calls == 1);
    CHECK(OutputStats().schedule_calls == 1);
    const std::vector<uint8_t> snapshot = eyeq_decklink_test::LastScheduledBytes();
    CHECK(snapshot.size() == size);
    CHECK(std::vector<uint8_t>(snapshot.begin(), snapshot.begin() + 16) ==
          std::vector<uint8_t>(bytes.begin(), bytes.begin() + 16));
    // Mutating the caller bytes after Accepted must not change the SDK frame copy.
    bytes[0] = 0xFF;
    CHECK(eyeq_decklink_test::LastScheduledBytes()[0] != 0xFF);
    CHECK(output.unreconciled_frame_count() == 1);

    CHECK(DeliverCompletion(0, 0));
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::FrameCompleted);
    CHECK(event.result == CompletionResult::Completed);
    CHECK(event.submit_id == 1 && event.source_frame_serial == 1001);
    CHECK(event.display_time == 1000);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 1);
  }
}

void TestBusyDoesNotConsume() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x22);
    for (int i = 0; i < 8; ++i) {
      CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(static_cast<uint64_t>(i)), 1000, 1000) ==
            ScheduleOutcome::Accepted);
    }
    CHECK(OutputStats().start_access_calls == 8);
    const auto access_before = OutputStats().schedule_calls;
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(99), 1000, 1000) == ScheduleOutcome::Busy);
    CHECK(OutputStats().schedule_calls == access_before); // Busy consumed nothing

    // Completing one frame frees one slot; the same submission can now be accepted.
    CHECK(DeliverCompletion(0, 0));
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(100), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestRejectsInvalidScheduleInput() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x33);
    // Wrong display duration is rejected before any native work.
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 999) == ScheduleOutcome::Rejected);
    // Undersized view.
    PackedFrameView small{bytes.data(), 4};
    CHECK(output.TrySchedule(small, MakeInfo(1), 1000, 1000) == ScheduleOutcome::Rejected);
    CHECK(OutputStats().schedule_calls == 0);
    CHECK(OutputStats().start_access_calls == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestSyncReentrantCompletionDoesNotDeadlock() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x44);
    SetScheduleReentrant(true);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(5), 1000, 1000) == ScheduleOutcome::Accepted);
    SetScheduleReentrant(false);
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::FrameCompleted && event.submit_id == 5);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestScheduleFailureRetainsThenSettles() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x55);
    SetOutputFault(OutputFault::ScheduleFails);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(7), 1000, 1000) == ScheduleOutcome::DeviceError);
    CHECK(output.unreconciled_frame_count() == 1); // record/frame retained
    // New submissions are refused after a fatal failure.
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(8), 1000, 1000) == ScheduleOutcome::Closed);
    // A completion that already arrived is still settled exactly once. The sticky
    // error diagnostic is surfaced first, so poll until the slot is reconciled.
    CHECK(DeliverCompletion(0, 3)); // Flushed
    bool reconciled = false;
    OutputEvent event;
    for (int i = 0; i < 4 && !reconciled; ++i) {
      if (PollOnce(output, &event)) {
        reconciled = output.unreconciled_frame_count() == 0;
      } else {
        break;
      }
    }
    CHECK(reconciled);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestScheduleFailureWithReentrantCallback() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x66);
    SetScheduleReentrant(true);
    SetOutputFault(OutputFault::ScheduleFails);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(9), 1000, 1000) == ScheduleOutcome::DeviceError);
    SetOutputFault(OutputFault::None);
    SetScheduleReentrant(false);
    // The reentrant completion was queued; consuming it reclaims the slot once. The
    // sticky error diagnostic may be surfaced first.
    CHECK(output.unreconciled_frame_count() == 1);
    bool reconciled = false;
    OutputEvent event;
    for (int i = 0; i < 4 && !reconciled; ++i) {
      if (PollOnce(output, &event)) {
        reconciled = output.unreconciled_frame_count() == 0;
      } else {
        break;
      }
    }
    CHECK(reconciled);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestCompletionResultsAreDistinguished() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x77);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(DeliverCompletion(eyeq_decklink_test::LastScheduledFrameIndex(), 1)); // DisplayedLate
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.result == CompletionResult::DisplayedLate);

    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(2), 2000, 1000) == ScheduleOutcome::Accepted);
    CHECK(DeliverCompletion(eyeq_decklink_test::LastScheduledFrameIndex(), 2)); // Dropped
    CHECK(PollOnce(output, &event));
    CHECK(event.result == CompletionResult::Dropped);

    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(3), 3000, 1000) == ScheduleOutcome::Accepted);
    CHECK(DeliverCompletion(eyeq_decklink_test::LastScheduledFrameIndex(), 3)); // Flushed
    CHECK(PollOnce(output, &event));
    CHECK(event.result == CompletionResult::Flushed);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestStillFrameAndPlaybackGate() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x88);
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(1)) == StillOutcome::Displayed);
    CHECK(OutputStats().display_sync_calls == 1);

    CHECK(output.StartScheduled(0, nullptr));

    std::string error;
    CHECK(output.QueryClock(50000, nullptr, nullptr, &error));
    CHECK(OutputStats().stream_time_calls == 1);

    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(2)) == StillOutcome::DeviceError); // playback running
    CHECK(output.StartScheduled(0, nullptr) == false);                                   // already running

    int64_t actual_stop = 0;
    CHECK(output.StopScheduled(0, &actual_stop, &error));
    CHECK(OutputStats().stop_calls == 1);
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(3)) == StillOutcome::DeviceError); // stopping, unconfirmed
    CHECK(DeliverPlaybackStopped());
    OutputEvent event;
    // The two gated still attempts queued diagnostics; the stopped notification still
    // arrives and is processed once the scheduler reports idle.
    CHECK(PollUntilKind(output, OutputEvent::Kind::PlaybackStopped, &event));
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(4)) == StillOutcome::Displayed); // now idle
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestStopConfirmationFailureKeepsUncertain() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x99);
    CHECK(output.StartScheduled(0, nullptr));
    SetOutputFault(OutputFault::IsRunningFails);
    std::string error;
    CHECK(output.StopScheduled(0, nullptr, &error));
    CHECK(DeliverPlaybackStopped());
    OutputEvent event;
    CHECK(PollOnce(output, &event));               // surfaces the error diagnostic
    CHECK(event.kind == OutputEvent::Kind::Error); // never a fabricated PlaybackStopped
    CHECK(event.stage == OutputEvent::Stage::Stop);
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(1)) == StillOutcome::DeviceError); // still uncertain
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestStoppedNotificationWhileStillActiveIsError() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x9A);
    CHECK(output.StartScheduled(0, nullptr));
    std::string error;
    CHECK(output.StopScheduled(0, nullptr, &error));
    SetNativeRunning(true); // native still reports active despite the notification
    CHECK(DeliverPlaybackStopped());
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::Error);
    CHECK(event.stage == OutputEvent::Stage::Stop);
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(1)) == StillOutcome::DeviceError); // not idle
    SetNativeRunning(false);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestCloseWithOutstandingFrameSettlesUnknown() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0xAA);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(output.StartScheduled(0, nullptr));
    // Never deliver a completion; Disable flushes, and the frame is settled unknown.
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 1);
    CHECK(OutputStats().stop_calls == 1); // playback was running
    OutputEvent event;
    bool saw_error = false;
    while (PollOnce(output, &event)) {
      if (event.kind == OutputEvent::Kind::Error) {
        saw_error = true;
      }
    }
    CHECK(saw_error);
  }
}

void TestLateCallbackAfterCloseIsSafe() {
  ResetFakeOutput();
  DeckLinkOutput *output = new DeckLinkOutput();
  const std::size_t size = OpenDefault(*output);
  std::vector<uint8_t> bytes = MakeBytes(size, 0xBB);
  CHECK(output->TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
  eyeq_decklink_test::CaptureRegisteredCallbackForLateDelivery();
  CHECK(output->Close(0) == CloseStatus::Closed);
  delete output; // safe: lifecycle is Closed
  // The captured reference keeps CallbackState alive; the late call must not crash
  // and must not touch released backend resources (it sees receiving=false).
  CHECK(eyeq_decklink_test::DeliverLateCompletion(0, 0));
}

void TestPartialPoolCreationReleasesAllFrames() {
  const int fail_points[] = {1, 3, 9}; // first pool frame, a middle frame, the still frame
  for (const int fail_at : fail_points) {
    ResetFakeOutput();
    {
      DeckLinkOutput output;
      SetCreateFrameFailAt(fail_at);
      ResolvedOutput resolved;
      std::string error;
      CHECK(!output.Open(MakeRequest(), &resolved, &error));
      CHECK(!error.empty());
      CHECK(output.Close(0) == CloseStatus::Closed);
    }
    ResetFakeOutput();
    CHECK(LiveObjectCount() == 0);
  }
}

void TestPartialPoolInterfaceFailuresReleaseEverything() {
  const auto run_case = [](void (*setup)()) {
    ResetFakeOutput();
    {
      DeckLinkOutput output;
      setup();
      ResolvedOutput resolved;
      std::string error;
      CHECK(!output.Open(MakeRequest(), &resolved, &error));
      CHECK(output.Close(0) == CloseStatus::Closed);
    }
    ResetFakeOutput();
    CHECK(LiveObjectCount() == 0);
  };
  run_case([] { SetOutputFault(OutputFault::GetSizeFails); }); // first frame capacity read
  run_case([] { SetGetSizeFailAt(3); });                       // later frame capacity read
  run_case([] { SetMetadataQueryFailAt(5); });                 // later frame metadata interface
}

void TestCloseRetriesAfterTransientNativeFailures() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OpenDefault(output);
    SetOutputFault(OutputFault::DisableFails);
    CHECK(output.Close(0) == CloseStatus::DeviceError);
    CHECK(OutputStats().disable_calls == 1);
    // A second attempt retries the still-needed Disable instead of giving up.
    CHECK(output.Close(0) == CloseStatus::DeviceError);
    CHECK(OutputStats().disable_calls == 2);
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 3);
    CHECK(OutputStats().set_callback_calls == 2); // registered once, unregistered once
  }
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OpenDefault(output);
    SetOutputFault(OutputFault::SetCallbackFails);
    CHECK(output.Close(0) == CloseStatus::DeviceError); // Disable ok, unregister fails
    CHECK(OutputStats().disable_calls == 1);
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed); // only the missing step is retried
    CHECK(OutputStats().disable_calls == 1);
    CHECK(OutputStats().set_callback_calls == 3); // Open + failed + successful unregister
  }
}

void TestOpenRollbackRetriesAfterTransientDisableFailure() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    // Fail frame pool creation after the output was enabled and the callback
    // registered, then fail the rollback Disable once.
    SetCreateFrameFailAt(3);
    SetOutputFault(OutputFault::DisableFails);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(OutputStats().enable_calls == 1);
    CHECK(OutputStats().disable_calls == 1);
    // Open returned false but native teardown could not finish, so the session stays
    // open-but-closing: it refuses new work and can be finished by Close.
    SetOutputFault(OutputFault::None);
    SetCreateFrameFailAt(0);
    std::vector<uint8_t> bytes(16, 0);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 0, 1000) == ScheduleOutcome::Closed);
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 2);
    CHECK(OutputStats().set_callback_calls == 2); // registered once, unregistered once
  }
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

void TestReopenAfterCloseAndRecoverableFailure() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    CHECK(output.Close(0) == CloseStatus::Closed);
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    CHECK(resolved.layout.image_bytes == size);
    CHECK(OutputStats().enable_calls == 2);
    std::vector<uint8_t> bytes = MakeBytes(resolved.layout.image_bytes, 0x11);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(2), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }

  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0xDD);
    SetOutputFault(OutputFault::ScheduleFails);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::DeviceError);
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes2 = MakeBytes(resolved.layout.image_bytes, 0x22);
    CHECK(output.TrySchedule(ViewOf(bytes2), MakeInfo(2), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestFindDisplayModeMatchesScanningAndNativeTimebase() {
  // A 1080i50 (25 frames/s) mode must not satisfy a 1080p25 request even though the
  // frame rate matches.
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetDisplayMode(1920, 1080, 1000, 25000, false);
    OutputRequest request = MakeRequest();
    request.mode = MonitorMode{1920, 1080, 1000, 25000, true};
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(request, &resolved, &error));
    CHECK(!error.empty());
    CHECK(OutputStats().enable_calls == 0);
  }
  // An equivalent rational with a different exact timebase resolves to the native
  // SDK mode, not to a copy of the request.
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetDisplayMode(3840, 2160, 2, 100, true);
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    CHECK(resolved.mode.is_progressive);
    CHECK(resolved.mode.frame_duration == 2);
    CHECK(resolved.mode.time_scale == 100);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestFindDisplayModeFrameRateFailureIsReported() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    SetOutputFault(OutputFault::FrameRateFails);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(error.find("GetFrameRate") != std::string::npos);
    CHECK(OutputStats().enable_calls == 0);
  }
}

void TestPqMetadataUsesFloatSetters() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OutputRequest request = MakePqRequest();
    request.hdr.cll = ContentLightLevel{true, 1000.5, 400.5};
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(request, &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x33);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
    bool saw_cll = false;
    bool saw_fall = false;
    for (const eyeq_decklink_test::FakeMetadataSet &set : MetadataSets()) {
      if (set.id == static_cast<int64_t>(bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel)) {
        CHECK(set.type == eyeq_decklink_test::kFakeMetadataTypeFloat);
        CHECK(set.float_value == 1000.5);
        saw_cll = true;
      }
      if (set.id == static_cast<int64_t>(bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel)) {
        CHECK(set.type == eyeq_decklink_test::kFakeMetadataTypeFloat);
        CHECK(set.float_value == 400.5);
        saw_fall = true;
      }
    }
    CHECK(saw_cll && saw_fall);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestMetadataFailurePreservesStageAndIdentity() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OutputRequest request = MakePqRequest();
    request.hdr.cll = ContentLightLevel{true, 1000.0, 400.0};
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(request, &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x77);
    SetOutputFault(OutputFault::MetadataFails);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(123), 1000, 1000) == ScheduleOutcome::DeviceError);
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::Error);
    CHECK(event.stage == OutputEvent::Stage::Metadata);
    CHECK(event.os_status == static_cast<int32_t>(E_FAIL));
    CHECK(event.submit_id == 123);
    CHECK(event.message[0] != '\0');
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestStillGateEmitsDiagnostic() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x88);
    CHECK(output.StartScheduled(0, nullptr));
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(1)) == StillOutcome::DeviceError);
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::Error);
    CHECK(event.stage == OutputEvent::Stage::DisplayStill);
    CHECK(event.message[0] != '\0');
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestGetBytesFailureEndsAccessSafely() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x44);
    // GetBytes fails but EndAccess succeeds: the frame is safe to reuse, so this is
    // not a quarantine case and the session can still be closed cleanly.
    SetOutputFault(OutputFault::GetBytesFails);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::DeviceError);
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestEventRingOverflowStopsSubmissions() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x55);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
    for (int i = 0; i < 128; ++i) {
      CHECK(DeliverPlaybackStopped());
    }
    // The real completion event is the 129th and is dropped by the bounded ring.
    CHECK(DeliverCompletion(eyeq_decklink_test::LastScheduledFrameIndex(), 0));
    OutputEvent event;
    int errors = 0;
    int frames = 0;
    while (output.PollEvent(&event)) {
      errors += event.kind == OutputEvent::Kind::Error ? 1 : 0;
      frames += event.kind == OutputEvent::Kind::FrameCompleted ? 1 : 0;
    }
    CHECK(errors >= 1);
    CHECK(frames == 0);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(2), 1000, 1000) != ScheduleOutcome::Accepted);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestUnknownCompletionResultIsError() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0x66);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 1000, 1000) == ScheduleOutcome::Accepted);
    CHECK(DeliverCompletion(eyeq_decklink_test::LastScheduledFrameIndex(), 99)); // not an SDK result
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::Error); // never fabricated as Completed
    CHECK(event.stage == OutputEvent::Stage::Schedule);
    CHECK(output.unreconciled_frame_count() == 0); // the callback still reclaimed the slot
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestScheduleRejectsInvalidTimeBounds() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0xEE);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), std::numeric_limits<int64_t>::max(), 1000) ==
          ScheduleOutcome::Rejected);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(2), -1, 1000) == ScheduleOutcome::Rejected);
    CHECK(OutputStats().schedule_calls == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestEarlyRollbackWithoutCallbackStateClosesSafely() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    // The output is enabled, then the connection query fails before CallbackState is
    // created, and the rollback Disable also fails once.
    SetConnectionQueryFails(true);
    SetOutputFault(OutputFault::DisableFails);
    ResolvedOutput resolved;
    std::string error;
    CHECK(!output.Open(MakeRequest(), &resolved, &error));
    CHECK(error.find("VideoOutputConnection") != std::string::npos);
    CHECK(OutputStats().enable_calls == 1);
    CHECK(OutputStats().disable_calls == 1);
    CHECK(OutputStats().set_callback_calls == 0); // CallbackState was never created
    SetConnectionQueryFails(false);
    SetOutputFault(OutputFault::None);
    // Close must not dereference the missing CallbackState, still finish this
    // session's Disable, and keep the earlier rollback failure readable.
    CHECK(output.Close(0) == CloseStatus::Closed);
    CHECK(OutputStats().disable_calls == 2);
    OutputEvent event;
    bool saw_close_error = false;
    while (output.PollEvent(&event)) {
      if (event.kind == OutputEvent::Kind::Error && event.stage == OutputEvent::Stage::Close) {
        saw_close_error = true;
      }
    }
    CHECK(saw_close_error);
  }
  ResetFakeOutput();
  CHECK(LiveObjectCount() == 0);
}

void TestLaterFatalErrorIsNotSwallowed() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0xAB);
    // A recoverable still-frame access failure is reported first...
    SetOutputFault(OutputFault::StartAccessFails);
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(123)) == StillOutcome::DeviceError);
    OutputEvent event;
    CHECK(PollOnce(output, &event));
    CHECK(event.kind == OutputEvent::Kind::Error);
    CHECK(event.stage == OutputEvent::Stage::Access);
    CHECK(event.submit_id == 123);
    // ...then the still frame succeeds again...
    SetOutputFault(OutputFault::None);
    CHECK(output.DisplayStill(ViewOf(bytes), MakeInfo(124)) == StillOutcome::Displayed);
    // ...and a later fatal schedule failure must still be reported, never swallowed
    // by the first-error snapshot.
    SetOutputFault(OutputFault::ScheduleFails);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(125), 1000, 1000) == ScheduleOutcome::DeviceError);
    bool saw_schedule = false;
    while (PollOnce(output, &event)) {
      if (event.kind == OutputEvent::Kind::Error && event.stage == OutputEvent::Stage::Schedule &&
          event.submit_id == 125) {
        saw_schedule = true;
      }
    }
    CHECK(saw_schedule);
    SetOutputFault(OutputFault::None);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestRetainedOverflowIsReported() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OpenDefault(output);
    // 300 invalid still frames each queue one diagnostic; the bounded queue holds 256.
    for (int i = 0; i < 300; ++i) {
      CHECK(output.DisplayStill(PackedFrameView{}, MakeInfo(static_cast<uint64_t>(i + 1))) ==
            StillOutcome::DeviceError);
    }
    OutputEvent event;
    int delivered = 0;
    uint64_t first_id = 0;
    bool overflow_reported = false;
    while (output.PollEvent(&event)) {
      if (delivered == 0) {
        first_id = event.submit_id;
      }
      ++delivered;
      const std::string message = event.message;
      if (message.find("overflow") != std::string::npos) {
        overflow_reported = true;
      }
    }
    CHECK(delivered == 257); // 256 queued diagnostics plus one loss summary
    CHECK(first_id == 45);   // the oldest 44 were dropped
    CHECK(overflow_reported);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestStartRefusedWhenCallbackOverflowPending() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    const std::size_t size = OpenDefault(output);
    std::vector<uint8_t> bytes = MakeBytes(size, 0xCD);
    CHECK(output.TrySchedule(ViewOf(bytes), MakeInfo(1), 0, 1000) == ScheduleOutcome::Accepted);
    for (int i = 0; i < 129; ++i) {
      CHECK(DeliverPlaybackStopped());
    }
    // No PollEvent first: the pending overflow alone must refuse the native start.
    std::string error;
    CHECK(!output.StartScheduled(0, &error));
    CHECK(!error.empty());
    CHECK(OutputStats().start_calls == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestOverflowDuringCloseIsReported() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    OpenDefault(output);
    // The burst is delivered by Disable, i.e. after Close's entry overflow check but
    // before event reception is closed; the final check must still report it.
    SetStoppedNotificationsOnDisable(129);
    CHECK(output.Close(0) == CloseStatus::Closed);
    SetStoppedNotificationsOnDisable(0);
    OutputEvent event;
    bool saw_overflow = false;
    while (output.PollEvent(&event)) {
      const std::string message = event.message;
      if (message.find("overflow") != std::string::npos) {
        saw_overflow = true;
      }
    }
    CHECK(saw_overflow);
  }
}

} // namespace

int main() {
  TestOpenCloseDisablesEvenWithZeroFrames();
  TestOpenInvalidRequestDoesNoNativeCalls();
  TestOpenRejectsUnsupportedModeBeforeEnable();
  TestOpenRejectsMissingMode();
  TestOpenRejectsUnsupportedHdr();
  TestOpenRollsBackEachFailure();
  TestAcceptedCopiesBytesAndReturnsCompletion();
  TestBusyDoesNotConsume();
  TestRejectsInvalidScheduleInput();
  TestSyncReentrantCompletionDoesNotDeadlock();
  TestScheduleFailureRetainsThenSettles();
  TestScheduleFailureWithReentrantCallback();
  TestCompletionResultsAreDistinguished();
  TestStillFrameAndPlaybackGate();
  TestStopConfirmationFailureKeepsUncertain();
  TestStoppedNotificationWhileStillActiveIsError();
  TestCloseWithOutstandingFrameSettlesUnknown();
  TestLateCallbackAfterCloseIsSafe();
  TestPartialPoolCreationReleasesAllFrames();
  TestPartialPoolInterfaceFailuresReleaseEverything();
  TestCloseRetriesAfterTransientNativeFailures();
  TestOpenRollbackRetriesAfterTransientDisableFailure();
  TestEarlyRollbackWithoutCallbackStateClosesSafely();
  TestLaterFatalErrorIsNotSwallowed();
  TestRetainedOverflowIsReported();
  TestStartRefusedWhenCallbackOverflowPending();
  TestOverflowDuringCloseIsReported();
  TestReopenAfterCloseAndRecoverableFailure();
  TestFindDisplayModeMatchesScanningAndNativeTimebase();
  TestFindDisplayModeFrameRateFailureIsReported();
  TestPqMetadataUsesFloatSetters();
  TestMetadataFailurePreservesStageAndIdentity();
  TestStillGateEmitsDiagnostic();
  TestGetBytesFailureEndsAccessSafely();
  TestEventRingOverflowStopsSubmissions();
  TestUnknownCompletionResultIsError();
  TestScheduleRejectsInvalidTimeBounds();
  std::printf("decklink output test: OK\n");
  return 0;
}
