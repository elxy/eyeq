#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "display/display_coordinator.hpp"
#include "display/render.hpp"
#include "display/rendered_frame.hpp"
#include "output/frame_sink.hpp"
#include "output/monitor_output.hpp"
#include "output/output_profile.hpp"

namespace EYEQ {

/*
 * Wires the still-only MonitorOutput to the coordination layer and enforces the
 * borrow pairing (V2.1 section 5.2 / V2-2 debt #4).
 *
 * It owns the FrameSink (the real DeckLinkBackend or an injected fake) and the
 * MonitorOutput built on it. It does NOT render: the caller composes and presents
 * the desktop first, then Tick() hands the *same* common result to the monitor.
 *
 * Borrow discipline:
 *   - Tick() only submits when MonitorOutput::AcceptsRequests() and the latest
 *     composed result (render_id) was not submitted yet.
 *   - Before Submit it calls DisplayCoordinator::AcquireResult() and keeps the
 *     borrow until the request terminates (Displayed/Cancelled/Failed and the
 *     packer slot reaped), then calls ReleaseResult().
 *   - While borrowed the coordinator rejects a real recomposition (a different
 *     scene key) and mapping-target/canvas changes; a caller that wants a new
 *     scene must Tick() until the request drains.
 *   - A submit failure is published in last_result() and stops further automatic
 *     submissions (failed()); the failed output is never silently restarted.
 *
 * No internal threads and no locks: Tick/Close and the destructor run on the one
 * UI thread, exactly like the coordinator.
 */
class MonitorController {
public:
  // @p sink is owned by this object and must be dedicated to it. @p resolved is the
  // output resolved by the backend Open; it is the comparison target the coordinator
  // was configured with.
  MonitorController(RenderContext &context, DisplayCoordinator &coordinator, std::unique_ptr<FrameSink> sink,
                    const ResolvedOutput &resolved);
  ~MonitorController();

  MonitorController(const MonitorController &) = delete;
  MonitorController &operator=(const MonitorController &) = delete;
  MonitorController(MonitorController &&) = delete;
  MonitorController &operator=(MonitorController &&) = delete;

  // One bounded, non-blocking step: pump the in-flight request and, once the
  // output is idle, submit the latest composed result if it is new. Must be
  // called every event-loop iteration so a slow request cannot starve progress.
  void Tick();

  // Logically cancel the in-flight request: it will never reach DisplayStill and
  // is reclaimed once its GPU work is complete (the borrow is released then).
  void Cancel();

  // True while a consumer (this controller) holds the common result.
  bool result_borrowed() const { return borrowed_; }
  bool in_flight() const { return monitor_ != nullptr && monitor_->in_flight(); }
  bool accepts_requests() const { return monitor_ != nullptr && monitor_->AcceptsRequests(); }
  // True after a submit failed; the controller keeps the output but stops
  // scheduling until Close (no automatic restart of a failed output).
  bool failed() const { return failed_; }
  MonitorOutput::Status status() const { return monitor_->status(); }
  const MonitorOutput::StepResult &last_result() const { return monitor_->last_result(); }
  const MonitorOutput::Diagnostics &diagnostics() const { return monitor_->diagnostics(); }
  const ResolvedOutput &resolved() const { return resolved_; }
  uint64_t submit_count() const { return submit_id_; }

  // Stop accepting/submitting, cancel any in-flight request and drain the packer
  // and sink within the total deadline. Returns the sink-level CloseStatus.
  CloseStatus Close(int drain_timeout_ms);

private:
  void SubmitLatest(const RenderedFrame &frame);
  void ReleaseBorrow();

  RenderContext *context_;
  DisplayCoordinator *coordinator_;
  std::unique_ptr<FrameSink> sink_;
  ResolvedOutput resolved_;
  std::unique_ptr<MonitorOutput> monitor_;

  bool borrowed_ = false;
  bool failed_ = false;
  bool closed_ = false;
  uint64_t submit_id_ = 0;
  uint64_t last_submitted_render_id_ = 0;
};

} // namespace EYEQ
