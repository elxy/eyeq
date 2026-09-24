#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace EYEQ {

// Read-only packed bytes handed to the sink. The caller keeps ownership; the sink
// copies them while TrySchedule/DisplayStill runs.
struct PackedFrameView {
  const uint8_t *data = nullptr;
  std::size_t size = 0;
};

// Opaque identity for one schedule attempt. The backend stores and echoes it and
// never interprets source_frame_serial; the output-index <-> source-serial mapping
// belongs to the caller.
struct SubmissionInfo {
  uint64_t submit_id = 0; // unique per schedule attempt
  uint64_t batch_seq = 0;
  uint64_t timeline_epoch = 0;
  uint64_t output_index = 0;
  uint64_t source_frame_serial = 0; // opaque echo
};

enum class ScheduleOutcome { Accepted, Busy, Rejected, DeviceError, Closed };
enum class StillOutcome { Displayed, Busy, DeviceError, Closed };
enum class CompletionResult { Completed, DisplayedLate, Dropped, Flushed };
enum class CloseStatus { Closed, DrainTimeout, DeviceError };

struct OutputEvent {
  enum class Kind { FrameCompleted, PlaybackStopped, Error, DeviceLost };
  enum class Stage { None, Negotiate, Enable, Access, Metadata, Schedule, DisplayStill, Start, Stop, Close };

  Kind kind = Kind::FrameCompleted;
  CompletionResult result = CompletionResult::Completed;

  uint64_t submit_id = 0;
  uint64_t batch_seq = 0;
  uint64_t timeline_epoch = 0;
  uint64_t output_index = 0;
  uint64_t source_frame_serial = 0;
  int64_t display_time = 0;

  int64_t actual_stop_time = 0;
  int64_t stream_time = 0;

  Stage stage = Stage::None;
  int32_t os_status = 0;
  char message[192] = {};
};

// The only public output seam. The real SDK backend and the test double implement
// the same methods, so control logic never needs to downcast. Device enumeration
// and Open live on the real backend, not here.
class FrameSink {
public:
  virtual ~FrameSink() = default;

  // Accepted: an SDK frame was acquired, the bytes were copied, CPU access ended,
  //           metadata applied and the native schedule call succeeded. The caller
  //           may release its packed staging frame.
  // Busy:     no free device frame; NOTHING is consumed (no access, no byte read);
  //           the caller keeps its staging frame and retries the SAME SubmissionInfo.
  // Rejected/DeviceError/Closed: the attempt is not accepted; the caller releases
  //           its staging frame and follows its error policy.
  virtual ScheduleOutcome TrySchedule(const PackedFrameView &view, const SubmissionInfo &info, int64_t display_time,
                                      int64_t display_duration) = 0;

  // Still frame. Only valid while scheduled playback is NOT running (the SDK
  // forbids DisplayVideoFrameSync during scheduled playback). Same Busy semantics.
  // Displayed means the bytes were copied into a dedicated device still frame and
  // the caller may release its staging frame; it does not consume video frame pool
  // budget and produces no scheduled completion event.
  virtual StillOutcome DisplayStill(const PackedFrameView &view, const SubmissionInfo &info) = 0;

  // Non-blocking; drains one event. Completion/flushed events are the only point at
  // which device frame budget is returned.
  virtual bool PollEvent(OutputEvent *out) = 0;

  virtual bool StartScheduled(int64_t start_time, std::string *error) = 0;
  virtual bool StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) = 0;
  virtual bool QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) = 0;

  // DrainTimeout means the sink is draining and still owns outstanding frames; the
  // caller must keep the object alive and retry (or terminate). Closed means every
  // frame was reconciled and the sink is closed.
  virtual CloseStatus Close(int drain_timeout_ms) = 0;
};

} // namespace EYEQ
