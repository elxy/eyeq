#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "display/display_coordinator.hpp"
#include "display/output_encode.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"
#include "display/rendered_frame.hpp"
#include "output/frame_packer.hpp"
#include "output/frame_sink.hpp"
#include "output/output_profile.hpp"

namespace EYEQ {

/*
 * Still-only monitor output. It does NOT render: it consumes the common
 * display-linear result produced by the DisplayCoordinator, encodes it to the
 * wire transfer into a fixed offscreen RGB target, packs that target with a
 * FramePacker and hands the packed bytes to FrameSink::DisplayStill.
 *
 * It owns the FrameSink (the real DeckLinkBackend or an injected fake) and is
 * wired to the coordinator: Tick() submits the coordinator's latest composed
 * result when it is new, so the caller no longer needs a separate controller
 * object. The desktop keeps its own presentation path.
 *
 * The borrowed common result carries no acquire/release protocol. The safety
 * argument is two documented invariants, checked by the queue_order_gpu sentinel
 * test:
 *   1. Same pl_gpu and queue: Submit() dispatches the encode before returning, so
 *     the borrowed RenderedFrame texture is finished with (read) before any later
 *     recomposition can overwrite it. On one queue, submission order is execution
 *     order, and the encode already copied the result into this object's own
 *     target. The desktop present path relies on the same property.
 *   2. Exit ordering: monitor.Close() (drains the packer and the sink) runs before
 *     the coordinator releases its renderer textures, which runs before the
 *     swapchain/context release.
 *
 * Ownership:
 *   - The RenderContext and the DisplayCoordinator are borrowed. The caller keeps
 *     both alive for the whole lifetime of this object.
 *   - This object owns the FrameSink, the fixed target texture, the output encoder
 *     and the packer.
 *   - It calls FrameSink::Close as part of its own Close(). Because it owns the
 *     sink, a throwing constructor destroys the sink instead of leaving it
 *     half-initialized.
 *
 * Threading: no internal threads and no locks. Every public method and the
 * destructor must be called from one thread (the UI thread in the player).
 * Pump()/Tick() never block or wait on the GPU: each performs one bounded step and
 * the caller schedules further calls. Close() waits up to its deadline and the
 * destructor calls a bounded Close, so those two are blocking by design.
 *
 * State model: at most one request is in flight. Tick()/Pump() advance it.
 *   - status() reports only occupancy (Idle / InFlight / Closed). It never carries
 *     the outcome of the last request.
 *   - last_result() reports the outcome of the last request
 *     (Displayed / Cancelled / Failed); None means no request has completed yet
 *     (an in-flight request is still running).
 *   - A request that finished frees its GPU staging slot and the source refs as
 *     soon as the packer allows it; only then is the module idle again and the
 *     next request accepted. "The request failed" and "the resources are drained"
 *     are therefore two distinct facts: the failure is published immediately,
 *     while the module keeps the slot/target/source alive and refuses to overwrite
 *     it (Submit reports Busy, AcceptsRequests() is false) until the reclaim
 *     completes. A failed request is never silently retried or auto-restarted.
 *   - A terminal sink failure (Error/DeviceLost, or a sink that closed) stops
 *     monitoring for the rest of the run: terminal_failed_ stays set until Close().
 */
class MonitorOutput {
public:
  // Occupancy of the module. Derived from the internal flags; never the outcome.
  enum class Status { Idle, InFlight, Closed };
  // Outcome of the last request. None == no completed request yet.
  enum class Outcome { None, Displayed, Cancelled, Failed };
  enum class SubmitOutcome { Accepted, Busy, Failed, Closed };

  struct Request {
    // The already-composed common result to encode and display. It must be a
    // valid display-linear frame whose size matches the resolved layout and whose
    // primaries match the output (the coordinator's mapping target is expected to
    // be the output target). The frame is borrowed from the coordinator and only
    // has to stay valid until Submit() returns: the encode is dispatched inside
    // Submit() (invariant 1), after which this object reads only its own target.
    RenderedFrame frame;
    SubmissionInfo info;
  };

  struct StepResult {
    Outcome outcome = Outcome::None;
    uint64_t submit_id = 0;
    uint64_t source_frame_serial = 0;
    std::string stage;  // failure/cancel stage: submit, config, render, pack, still, event, cancel, close
    std::string reason; // human-readable detail for Failed, empty otherwise
  };

  // Test/diagnostic counters. They observe how often the real GPU work and the
  // sink hand-off happened; they never drive the state machine.
  struct Diagnostics {
    uint64_t render_count = 0;
    uint64_t pack_submit_count = 0;
    uint64_t still_attempts = 0;
    uint64_t still_busy = 0;
    uint64_t displayed = 0;
    uint64_t pack_failed = 0;
    uint64_t finite_checks = 0;
    uint64_t finite_failures = 0;
  };

  // @p sink is owned by this object and must be dedicated to it. @p coordinator is
  // borrowed and must stay alive; Tick() reads its latest composed result.
  MonitorOutput(RenderContext &context, DisplayCoordinator &coordinator, std::unique_ptr<FrameSink> sink,
                const ResolvedOutput &output);
  ~MonitorOutput();

  MonitorOutput(const MonitorOutput &) = delete;
  MonitorOutput &operator=(const MonitorOutput &) = delete;
  MonitorOutput(MonitorOutput &&) = delete;
  MonitorOutput &operator=(MonitorOutput &&) = delete;

  /*
   * One bounded, non-blocking step: pump the in-flight request and, once the
   * output is idle, submit the coordinator's latest composed result if it is new.
   * Must be called every event-loop iteration so a slow request cannot starve
   * progress.
   */
  void Tick();

  /*
   * Low-level submit of an explicit request. Accepted: the request is in flight
   * and the GPU work was submitted; call Pump()/Tick(). Busy: a request is already
   * in flight; @p request is not read, mapped or modified at all. Failed: the
   * request could not be started (config/render/pack) or the sink rejected it;
   * see last_result(). Closed: Close() was called.
   */
  SubmitOutcome Submit(Request &request);

  // One bounded low-level step: drains a bounded number of non-blocking sink
  // events, polls the packer and, when the packed bytes are ready, attempts at
  // most one DisplayStill. Never sleeps or waits for the GPU. A terminal
  // Error/DeviceLost event is always handled before any DisplayStill attempt.
  StepResult Pump();

  // Logical cancel of the in-flight request: it will never reach DisplayStill and
  // is reclaimed (Reap) once its GPU work is complete. No-op when idle.
  void Cancel();

  // Occupancy only; use last_result() for the outcome.
  Status status() const {
    if (closed_) {
      return Status::Closed;
    }
    return request_in_flight_ ? Status::InFlight : Status::Idle;
  }
  // True while the module owns an in-flight request (including a failed request
  // whose packer slot is still draining). The caller must keep pumping it and
  // must not compose a new scene that would overwrite the borrowed result.
  bool in_flight() const { return request_in_flight_; }
  // True only when a new Submit would be accepted right now: the module is not
  // closed/closing/terminal and owns no in-flight (or failed-but-undrained) request.
  bool AcceptsRequests() const { return !closed_ && !closing_ && !terminal_failed_ && !request_in_flight_; }
  // True after a terminal sink event. Monitoring stays stopped until Close().
  bool terminal_failed() const { return terminal_failed_; }
  // True after Tick() stopped scheduling because a request failed; the output is
  // kept but never restarted automatically.
  bool failed() const { return failed_; }
  const StepResult &last_result() const { return last_result_; }
  const std::string &close_reason() const { return close_reason_; }
  const Diagnostics &diagnostics() const { return diagnostics_; }
  const ResolvedOutput &resolved() const { return output_; }
  // Number of requests Tick() submitted automatically.
  uint64_t submit_count() const { return submit_id_; }

  // Fixed offscreen target frame (diagnostics/tests only). The target is owned by
  // this object and is not valid after destruction.
  const struct pl_frame &target_frame() const { return target_.frame; }

  /*
   * Stops accepting requests, logically cancels any in-flight one and drains the
   * packer and the owned sink within one total deadline of @p drain_timeout_ms
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

  void SetResult(Outcome outcome, const char *stage, std::string reason);
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
  // Tick() scheduling helper.
  void SubmitLatest(const RenderedFrame &frame);

  RenderContext *context_ = nullptr;
  DisplayCoordinator *coordinator_ = nullptr;
  std::unique_ptr<FrameSink> sink_;
  ResolvedOutput output_;

  Target target_;
  std::unique_ptr<OutputEncoder> encoder_;
  std::unique_ptr<FiniteGate> finite_gate_;
  std::unique_ptr<FramePacker> packer_;

  bool closed_ = false;
  bool closing_ = false;
  bool terminal_failed_ = false;
  std::string terminal_reason_;
  std::string close_reason_;

  bool request_in_flight_ = false;
  // True when the current request already failed (last_result_ published) but its
  // staging slot could not be reclaimed yet; the slot and source stay alive and no
  // new request may overwrite them until the drain completes.
  bool drain_pending_ = false;
  bool cancel_requested_ = false;
  StagingSlot *slot_ = nullptr;
  SubmissionInfo in_flight_info_{};

  StepResult last_result_{};
  Diagnostics diagnostics_{};

  // Tick() scheduling state.
  bool failed_ = false;
  uint64_t submit_id_ = 0;
  uint64_t last_submitted_render_id_ = 0;
};

}; // namespace EYEQ
