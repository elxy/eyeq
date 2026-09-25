#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "display/output_encode.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"
#include "display/rendered_frame.hpp"
#include "output/frame_packer.hpp"
#include "output/frame_sink.hpp"
#include "output/output_profile.hpp"

namespace EYEQ {

/*
 * Reusable still-only monitor output. It does NOT render: it consumes the common
 * display-linear result produced by the coordination layer (DisplayCoordinator),
 * encodes it to the wire transfer into a fixed offscreen RGB target, packs that
 * target with a FramePacker and hands the packed bytes to
 * FrameSink::DisplayStill. There is no second FrameRenderer here (V2-2).
 *
 * Ownership:
 *   - The RenderContext and the FrameSink are borrowed. The caller keeps both
 *     alive for the whole lifetime of this object and the sink must be dedicated
 *     to this object. The common RenderedFrame is also borrowed: it is owned by
 *     the coordinator's renderer and must stay valid until the request was
 *     displayed/cancelled and its packer slot reaped. The caller must not compose
 *     a new scene or reconfigure the coordinator's precision while a request is
 *     in flight (AcceptsRequests() is the check).
 *   - This object owns the fixed target texture, the output encoder and the packer.
 *   - It calls FrameSink::Close as part of its own Close(); it never destroys or
 *     closes the sink implicitly during construction/rollback.
 *
 * Threading: no internal threads and no locks. Submit/Pump/Cancel/Close and the
 * destructor must be called from one thread. Pump() never blocks or waits on the
 * GPU: it only performs one bounded step and the caller schedules further calls.
 * Close() waits up to its deadline and the destructor calls a bounded Close, so
 * those two are blocking by design.
 *
 * State model: at most one request is in flight. Pump() advances it and returns
 * the current outcome. A request that finished (Displayed/Cancelled/Failed) frees
 * its GPU staging slot and the source refs as soon as the packer allows it; only
 * then is the module idle again and the next request accepted. Two distinct facts
 * are reported separately:
 *   - "the request failed": published immediately in status()/last_result() as soon
 *     as the failure is known, with the stage, submit/frame identity and reason;
 *   - "the resources are drained": the packer slot may still be borrowed by the
 *     GPU. Until it is reclaimed the module keeps the slot/target/source alive and
 *     refuses to overwrite it with a new request (Submit reports Busy,
 *     AcceptsRequests() is false). Failed() never masquerades as Pending/Idle, and
 *     a failed request is never silently retried or auto-restarted.
 * A recoverable failure is therefore still observable during the drain, and once
 * the resources are safely reclaimed the caller may submit the next explicit
 * request. A terminal sink failure keeps terminal_failed_ set until Close().
 */
class MonitorOutput {
public:
  enum class Status { Idle, Pending, Displayed, Cancelled, Failed, Closed };
  enum class SubmitOutcome { Accepted, Busy, Failed, Closed };

  struct Request {
    // The already-composed common result to encode and display. It must be a
    // valid display-linear frame whose size matches the resolved layout and whose
    // primaries match the output (the coordinator's mapping target is expected to
    // be the output target). The frame is borrowed from the coordinator and must
    // stay valid for the whole in-flight period.
    RenderedFrame frame;
    SubmissionInfo info;
  };

  struct StepResult {
    Status status = Status::Idle;
    uint64_t submit_id = 0;
    uint64_t source_frame_serial = 0;
    std::string stage;  // failure/cancel stage: submit, config, feed, render, pack, still, event, cancel, close
    std::string reason; // human-readable detail for Failed, empty otherwise
  };

  // Test/diagnostic counters. They observe how often the real GPU work and the
  // sink hand-off happened; they never drive the state machine. Failure detail is
  // reported through last_result()/close_reason(), not through these counts.
  struct Diagnostics {
    uint64_t render_count = 0;
    uint64_t pack_submit_count = 0;
    uint64_t still_attempts = 0;
    uint64_t still_busy = 0;
    uint64_t displayed = 0;
    uint64_t pack_failed = 0;
  };

  MonitorOutput(RenderContext &context, const ResolvedOutput &output, FrameSink &sink);
  ~MonitorOutput();

  MonitorOutput(const MonitorOutput &) = delete;
  MonitorOutput &operator=(const MonitorOutput &) = delete;
  MonitorOutput(MonitorOutput &&) = delete;
  MonitorOutput &operator=(MonitorOutput &&) = delete;

  /*
   * Accepted: the request is in flight and the GPU work was submitted; call Pump().
   *   The input is not modified.
   * Busy:     a request is already in flight; @p request is not read, mapped or
   *   modified at all. Retry after the current request finishes.
   * Failed:   the request could not be started (config/scene/feed/pack) or the sink
   *   rejected it; see last_result() for the stage, identity and reason. The module
   *   is still usable unless a terminal sink failure occurred.
   * Closed:   Close() was called; no request is accepted.
   *
   * An unexpected feed/render exception is recorded in status()/last_result()
   * (stage "feed"/"render") and then rethrown; the module keeps no in-flight
   * request and no old Displayed result is left behind to describe the failure.
   */
  SubmitOutcome Submit(Request &request);

  // One bounded step: drains a bounded number of non-blocking sink events, polls
  // the packer and, when the packed bytes are ready, attempts at most one
  // DisplayStill. Never sleeps or waits for the GPU. A terminal Error/DeviceLost
  // event is always handled before any DisplayStill attempt.
  StepResult Pump();

  // Logical cancel of the in-flight request: it will never reach DisplayStill and
  // is reclaimed (Reap) once its GPU work is complete. No-op when idle.
  void Cancel();

  Status status() const { return status_; }
  // True only when a new Submit would be accepted right now: the module is not
  // closed/closing/terminal and owns no in-flight (or failed-but-undrained) request.
  bool AcceptsRequests() const { return !closed_ && !closing_ && !terminal_failed_ && !request_in_flight_; }
  const StepResult &last_result() const { return last_result_; }
  const std::string &close_reason() const { return close_reason_; }
  const Diagnostics &diagnostics() const { return diagnostics_; }

  // Fixed offscreen target frame (diagnostics/tests only). The target is owned by
  // this object and is not valid after destruction.
  const struct pl_frame &target_frame() const { return target_.frame; }

  /*
   * Stops accepting requests, logically cancels any in-flight one and drains the
   * packer and the borrowed sink within one total deadline of @p drain_timeout_ms
   * (the remaining budget is passed to each step). Returns Closed only when the
   * packer is Idle and the sink reported Closed. DrainTimeout/DeviceError mean the
   * object (and its sources) must stay alive, close_reason() explains it, and
   * Close() may be retried.
   */
  CloseStatus Close(int drain_timeout_ms);

private:
  struct Target {
    pl_tex tex = nullptr;
    struct pl_frame frame{};
  };

  void SetResult(Status status, const char *stage, std::string reason);
  StepResult MakeResult(Status status) const;
  // Reclaim the current staging slot without polling: valid when it is known to be
  // Ready or already polled to Failed. Returns true when the module no longer owns
  // the request's GPU/source resources (and clears the retained source refs then).
  bool ReapSlotIfPossible();
  // Advance a not-yet-known slot by one non-blocking poll, then try to reclaim it.
  // Used after a failure/terminal event so a Pending slot can still drain without
  // waiting for Close(). Never blocks on the GPU.
  bool AdvanceAndReapSlot();
  void RecordTerminalEvent(const OutputEvent &event);
  // Validates the borrowed common result against the resolved output.
  bool ValidateFrame(const Request &request, std::string *error) const;

  RenderContext *context_;
  FrameSink *sink_;
  ResolvedOutput output_;

  Target target_;
  std::unique_ptr<OutputEncoder> encoder_;
  std::unique_ptr<FramePacker> packer_;

  bool closed_ = false;
  bool closing_ = false;
  bool terminal_failed_ = false;
  std::string terminal_reason_;
  std::string close_reason_;

  bool request_in_flight_ = false;
  // True when the current request already failed (status_/last_result_ published)
  // but its staging slot could not be reclaimed yet; the slot and source stay alive
  // and no new request may overwrite them until the drain completes.
  bool drain_pending_ = false;
  bool cancel_requested_ = false;
  StagingSlot *slot_ = nullptr;
  std::map<int, std::shared_ptr<AVFrame>> in_flight_frames_;
  SubmissionInfo in_flight_info_{};

  Status status_ = Status::Idle;
  StepResult last_result_{};
  Diagnostics diagnostics_{};
};

}; // namespace EYEQ
