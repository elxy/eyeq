#include "test_signal_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "output/pixel_format.hpp"
#include "test_pattern.hpp"

namespace EYEQ {

volatile std::sig_atomic_t test_signal_stop_requested = 0;

namespace {

using Clock = std::chrono::steady_clock;

const char *OutcomeToString(ScheduleOutcome outcome) {
  switch (outcome) {
  case ScheduleOutcome::Accepted:
    return "accepted";
  case ScheduleOutcome::Busy:
    return "busy";
  case ScheduleOutcome::Rejected:
    return "rejected";
  case ScheduleOutcome::DeviceError:
    return "device error";
  case ScheduleOutcome::Closed:
    return "closed";
  }
  return "unknown";
}

} // namespace

bool RunTestSignal(FrameSink &sink, const ResolvedOutput &resolved, const TestSignalOptions &options,
                   TestSignalResult *result, std::string *error) {
  if (result != nullptr) {
    *result = TestSignalResult();
  }
  if (error != nullptr) {
    error->clear();
  }

  const BufferLayout &layout = resolved.layout;
  const int64_t frame_duration = resolved.mode.frame_duration;

  uint64_t submitted = 0;
  uint64_t completed = 0;
  uint64_t late = 0;
  uint64_t dropped = 0;
  uint64_t flushed = 0;
  bool started = false;
  bool interrupted = false;

  auto fail = [&](const std::string &message) {
    if (error != nullptr) {
      *error = message;
    }
    if (result != nullptr) {
      result->submitted = submitted;
      result->completed = completed;
      result->late = late;
      result->dropped = dropped;
      result->flushed = flushed;
      result->started = started;
    }
    return false;
  };

  if (frame_duration <= 0) {
    return fail("resolved mode has no valid frame duration");
  }
  if (options.drain_timeout_ms < 0) {
    return fail("drain timeout must not be negative");
  }
  // Re-check the layout precondition here as well as in the CLI: an invalid or
  // unknown-format layout must never reach the generator.
  std::string layout_error;
  if (!ValidateLayout(layout, layout.image_bytes, &layout_error)) {
    return fail("resolved layout is invalid: " + layout_error);
  }
  if (options.frame_count > 0) {
    const uint64_t max_frames = static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / frame_duration);
    if (options.frame_count > max_frames) {
      return fail("requested frame count exceeds the representable timeline");
    }
  }

  std::vector<uint8_t> staging(layout.image_bytes, 0);
  const std::chrono::milliseconds timeout(options.drain_timeout_ms);

  // The deadline bounds lack of progress, not total playback duration: the authorized
  // 10-second output may legitimately run longer than a 5-second stall timeout. It is
  // reset whenever a poll returns a frame budget and restarted for the final drain.
  Clock::time_point progress_deadline = Clock::now() + timeout;
  std::string pending_error;

  // Drains every queued event, classifying completions and recording the first
  // Error/DeviceLost. Returns whether any event was processed (i.e. progress).
  auto poll_events = [&]() -> bool {
    bool any = false;
    OutputEvent event;
    while (sink.PollEvent(&event)) {
      any = true;
      switch (event.kind) {
      case OutputEvent::Kind::FrameCompleted:
        switch (event.result) {
        case CompletionResult::Completed:
          ++completed;
          break;
        case CompletionResult::DisplayedLate:
          ++late;
          break;
        case CompletionResult::Dropped:
          ++dropped;
          break;
        case CompletionResult::Flushed:
          ++flushed;
          break;
        }
        break;
      case OutputEvent::Kind::Error:
      case OutputEvent::Kind::DeviceLost:
        if (pending_error.empty()) {
          // Keep stage/HRESULT/identity, not just the text.
          char buffer[320];
          std::snprintf(buffer, sizeof(buffer), "sink reported %s stage=%d status=0x%08X submit_id=%llu: %s",
                        event.kind == OutputEvent::Kind::DeviceLost ? "DeviceLost" : "Error",
                        static_cast<int>(event.stage), static_cast<unsigned>(event.os_status),
                        static_cast<unsigned long long>(event.submit_id), event.message);
          pending_error = buffer;
        }
        break;
      case OutputEvent::Kind::PlaybackStopped:
        break;
      }
    }
    return any;
  };

  const uint64_t frame_count = options.frame_count;
  const uint64_t preroll = std::min<uint64_t>(options.preroll_frames, frame_count);

  // With a zero preroll there is nothing to queue first: start before submitting.
  if (frame_count > 0 && preroll == 0) {
    std::string start_error;
    if (!sink.StartScheduled(0, &start_error)) {
      return fail("StartScheduled failed: " + start_error);
    }
    started = true;
  }

  for (uint64_t index = 0; index < frame_count; ++index) {
    // The signal handler only sets this flag; the consumer thread observes it here.
    if (test_signal_stop_requested != 0) {
      interrupted = true;
      break;
    }
    // Consume events that arrived since the previous submission so completions stay
    // counted and an asynchronous error is not deferred until the next Busy.
    poll_events();
    if (!pending_error.empty()) {
      return fail(pending_error);
    }

    RenderTestPattern(index, layout, resolved.color.encoding, staging.data());

    SubmissionInfo info;
    info.submit_id = index;
    info.batch_seq = index;
    info.timeline_epoch = 0;
    info.output_index = index;
    info.source_frame_serial = index;

    const int64_t display_time = static_cast<int64_t>(index) * frame_duration;
    const PackedFrameView view{staging.data(), staging.size()};

    for (;;) {
      if (test_signal_stop_requested != 0) {
        interrupted = true;
        break;
      }
      const ScheduleOutcome outcome = sink.TrySchedule(view, info, display_time, frame_duration);
      if (outcome == ScheduleOutcome::Accepted) {
        ++submitted;
        // An accepted submission is progress: the stall budget is relative to it.
        progress_deadline = Clock::now() + timeout;
        break;
      }
      if (outcome == ScheduleOutcome::Busy) {
        // The device frame pool is full. If scheduled playback has not started, no
        // frame can complete before StartScheduled, so start now instead of waiting
        // for completions that cannot happen. This bounds preroll to the pool without
        // the tool needing to know the pool depth.
        if (!started) {
          std::string start_error;
          if (!sink.StartScheduled(0, &start_error)) {
            return fail("StartScheduled failed: " + start_error);
          }
          started = true;
        }
        const bool progressed = poll_events();
        if (!pending_error.empty()) {
          return fail(pending_error);
        }
        if (test_signal_stop_requested != 0) {
          interrupted = true;
          break;
        }
        if (progressed) {
          // A frame budget was returned: retry instead of declaring a stale timeout.
          progress_deadline = Clock::now() + timeout;
          continue;
        }
        if (Clock::now() >= progress_deadline) {
          return fail("no frame budget became available within the timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      // Non-recoverable outcome: consume the diagnostic the backend just recorded so
      // its stage/HRESULT/identity is not lost, then report it.
      poll_events();
      if (!pending_error.empty()) {
        return fail(pending_error);
      }
      return fail(std::string("schedule ") + OutcomeToString(outcome) + " for frame " + std::to_string(index));
    }

    if (interrupted) {
      break;
    }

    if (!started && submitted >= preroll) {
      std::string start_error;
      if (!sink.StartScheduled(0, &start_error)) {
        return fail("StartScheduled failed: " + start_error);
      }
      started = true;
    }
  }

  // A stop request ends the run early; the caller still Closes the output to settle
  // outstanding frames instead of relying on process exit. The error text is the
  // authoritative interruption signal so a real device error is never reclassified.
  if (interrupted) {
    return fail("interrupted by stop request after " + std::to_string(submitted) + " frame(s)");
  }

  // The final drain gets its own budget, starting now.
  progress_deadline = Clock::now() + timeout;
  for (;;) {
    poll_events();
    if (!pending_error.empty()) {
      return fail(pending_error);
    }
    const uint64_t settled = completed + late + dropped + flushed;
    if (settled >= submitted) {
      break;
    }
    if (test_signal_stop_requested != 0) {
      return fail("interrupted during drain");
    }
    if (Clock::now() >= progress_deadline) {
      return fail("drain timed out: " + std::to_string(submitted - settled) + " frame(s) unsettled");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (result != nullptr) {
    result->submitted = submitted;
    result->completed = completed;
    result->late = late;
    result->dropped = dropped;
    result->flushed = flushed;
    result->started = started;
  }
  return true;
}

} // namespace EYEQ
