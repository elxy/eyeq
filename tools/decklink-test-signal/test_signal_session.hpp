#pragma once

#include <csignal>
#include <cstdint>
#include <string>

#include "output/frame_sink.hpp"
#include "output/output_profile.hpp"

namespace EYEQ {

// Consumer-thread stop request. A SIGINT/SIGTERM handler may only set this flag
// (nothing else: no SDK, logging, locks or allocation). RunTestSignal observes it
// on the consumer thread, stops submitting new frames and returns so the caller can
// Close the output normally instead of relying on process exit to release the
// device.
extern volatile std::sig_atomic_t test_signal_stop_requested;

// Parameters for one test-signal run. frame_count is the number of frames to
// submit; preroll_frames is how many accepted frames to queue before starting
// scheduled playback; drain_timeout_ms bounds both the wait for a free device
// frame and the post-submit drain.
struct TestSignalOptions {
  uint64_t frame_count = 0;
  uint64_t preroll_frames = 0;
  int drain_timeout_ms = 0;
};

// Honest run summary. submitted counts accepted submissions; the completion
// counters classify the completion events for those submissions. Nothing is
// fabricated: a run that fails or times out reports the progress it actually made.
struct TestSignalResult {
  uint64_t submitted = 0;
  uint64_t completed = 0;
  uint64_t late = 0;
  uint64_t dropped = 0;
  uint64_t flushed = 0;
  bool started = false;
};

// Drives one test-signal run through the FrameSink seam: renders a deterministic
// pattern per frame, schedules frame_count frames at sequential display times, and
// drains completion events. Returns false with *error on a scheduling failure, an
// Error/DeviceLost event or a drain timeout. Reuses a single staging buffer and
// retries the same frame on Busy; it never drops or skips a frame.
bool RunTestSignal(FrameSink &sink, const ResolvedOutput &resolved, const TestSignalOptions &options,
                   TestSignalResult *result, std::string *error);

} // namespace EYEQ
