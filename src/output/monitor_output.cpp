#include "output/monitor_output.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <spdlog/fmt/fmt.h>

#include "log.hpp"

using namespace EYEQ;

namespace {

constexpr int kDefaultCloseTimeoutMs = 5000;
// Upper bound on sink events handled per Pump. If more are queued we defer the
// DisplayStill attempt to the next Pump instead of bypassing a possible queued
// fatal event or turning Pump into an unbounded drain loop.
constexpr int kMaxSinkEventsPerPump = 8;
// Frames of lead time added to the query clock before choosing the next display
// tick. The packer benchmark puts submit->ready p95 at <= 3.7 ms (UHD) and the sink
// copy is a few ms, both well inside one 20 ms frame at 50 fps, so one frame of lead
// is enough to land the reference frame on time; anything later is dropped by the
// sink (DisplayedLate/Dropped) rather than slowing the monitor down.
constexpr int kSubmitLeadFrames = 1;
// Pause transition choice. The plan's primary behavior is to stop enqueuing new
// frames and let the hardware hold the last frame, but that run-empty hold is not yet
// confirmed on real hardware. We therefore take the guaranteed-correct fallback: once
// the paced queue has drained, stop the scheduled session (waiting for the confirmed
// PlaybackStopped notification) and display the current frame as a still, which also
// keeps seek/frame-step working while paused. Set this to false to rely on the
// unverified hardware hold instead (the still path then stays unused after playback).
constexpr bool kPauseStopsScheduledPlayback = true;

using Clock = std::chrono::steady_clock;

} // namespace

MonitorOutput::MonitorOutput(RenderContext &context, DisplayCoordinator &coordinator, std::unique_ptr<FrameSink> sink,
                             const ResolvedOutput &output)
    : context_(&context), coordinator_(&coordinator), sink_(std::move(sink)), output_(output) {
  if (sink_ == nullptr) {
    throw std::invalid_argument("MonitorOutput requires a FrameSink");
  }
  if (!context.HasDevice()) {
    throw std::invalid_argument("MonitorOutput requires a RenderContext with a created device");
  }
  if (output.needs_vertical_flip) {
    throw std::invalid_argument("MonitorOutput does not support needs_vertical_flip output");
  }

  // Re-validate the caller-supplied configuration through the frozen 0.5 validator
  // instead of trusting the struct. ValidateRequest also rejects the unsupported
  // encoding/orientation combinations.
  std::string error;
  OutputRequest request;
  request.device = output.device;
  request.mode = output.mode;
  request.connection = output.connection;
  request.pixel_format = output.pixel_format;
  request.color = output.color;
  request.hdr = output.hdr;
  if (!ValidateRequest(request, &error)) {
    throw std::invalid_argument("invalid monitor output configuration: " + error);
  }
  // The double passed ValidateRequest; the float we actually store must still be a
  // finite positive destination peak.
  const float target_peak = static_cast<float>(output.color.target_nits);
  if (!std::isfinite(target_peak) || target_peak <= 0.0f) {
    throw std::invalid_argument("monitor output target_nits is not representable as a positive float");
  }
  if (output.layout.width != output.mode.width || output.layout.height != output.mode.height) {
    throw std::invalid_argument("monitor output layout dimensions must match the resolved mode");
  }
  if (output.layout.pixel_format != output.pixel_format) {
    throw std::invalid_argument("monitor output layout pixel format must match the resolved output");
  }
  // The layout carries the SDK-reported stride; the capacity we can vouch for here
  // is exactly image_bytes, so this also rejects an inconsistent row_bytes/image_bytes.
  if (!ValidateLayout(output.layout, output.layout.image_bytes, &error)) {
    throw std::invalid_argument("invalid monitor output layout: " + error);
  }

  pl_gpu gpu = context.gpu();
  // Only resources this constructor acquired are cleaned up on failure. The
  // borrowed context/coordinator are never touched; the owned sink is destroyed
  // by sink_ because construction failed.
  try {
    pl_fmt format = pl_find_named_fmt(gpu, "rgba32f");
    const auto needed =
        static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    if (format == nullptr || (format->caps & needed) != needed) {
      throw std::runtime_error("MonitorOutput requires a sampleable/renderable/host-readable rgba32f format");
    }
    struct pl_tex_params tex_params = {
        .w = output.layout.width,
        .h = output.layout.height,
        .format = format,
        .sampleable = true,
        .renderable = true,
        .host_readable = true,
    };
    target_.tex = pl_tex_create(gpu, &tex_params);
    if (target_.tex == nullptr) {
      throw std::runtime_error("MonitorOutput failed to create the fixed offscreen target");
    }
    target_.frame.num_planes = 1;
    target_.frame.planes[0].texture = target_.tex;
    target_.frame.planes[0].components = 4;
    target_.frame.planes[0].component_mapping[0] = 0;
    target_.frame.planes[0].component_mapping[1] = 1;
    target_.frame.planes[0].component_mapping[2] = 2;
    target_.frame.planes[0].component_mapping[3] = 3;
    // Single-plane floating point RGB target: full range, RGB, no video-range
    // compression and no extra quantization. The packer consumes this encoding.
    target_.frame.repr = pl_color_repr_rgb;
    target_.frame.repr.sys = PL_COLOR_SYSTEM_RGB;
    target_.frame.repr.levels = PL_COLOR_LEVELS_FULL;
    switch (output.color.encoding.primaries) {
    case ColorPrimaries::BT709:
      target_.frame.color.primaries = PL_COLOR_PRIM_BT_709;
      break;
    case ColorPrimaries::BT2020:
      target_.frame.color.primaries = PL_COLOR_PRIM_BT_2020;
      break;
    default:
      throw std::invalid_argument("unsupported monitor output primaries");
    }
    switch (output.color.encoding.transfer) {
    case ColorTransfer::SDR_BT1886:
      target_.frame.color.transfer = PL_COLOR_TRC_BT_1886;
      break;
    case ColorTransfer::PQ:
      target_.frame.color.transfer = PL_COLOR_TRC_PQ;
      break;
    default:
      throw std::invalid_argument("unsupported monitor output transfer");
    }
    // Destination peak only: this describes the monitor target, it does not
    // override the source mastering peak.
    target_.frame.color.hdr.max_luma = target_peak;

    encoder_ = std::make_unique<OutputEncoder>(context.log(), gpu);
    // The playback/monitor path must never pay for a full-frame NaN readback; the
    // GPU-side gate reduces the common result to one flag word (V2.1 3.2, review
    // decision). It is required, so a GPU without compute/SSBO fails fast here.
    finite_gate_ = std::make_unique<FiniteGate>(context.log(), gpu);
    packer_ = std::make_unique<FramePacker>(gpu, ToPackConfig(output_));
  } catch (...) {
    // The destructor will not run for a throwing constructor; release the raw GPU
    // resource acquired so far. encoder_/packer_ are cleaned up by their unique_ptr.
    if (target_.tex != nullptr) {
      pl_tex_destroy(gpu, &target_.tex);
    }
    throw;
  }
}

MonitorOutput::~MonitorOutput() {
  if (!closed_) {
    Close(kDefaultCloseTimeoutMs);
    if (!closed_) {
      Logger->error("MonitorOutput destroyed without a successful Close ({}); in-flight resources were not drained",
                    close_reason_);
    }
  }
  // Destroy the packer before the target it borrows; the packer terminates loudly if
  // a slot is still owned, which is the documented fail-fast boundary.
  packer_.reset();
  if (target_.tex != nullptr && context_ != nullptr) {
    pl_tex_destroy(context_->gpu(), &target_.tex);
  }
  encoder_.reset();
}

void MonitorOutput::SetResult(Outcome outcome, const char *stage, std::string reason) {
  last_result_.outcome = outcome;
  last_result_.submit_id = in_flight_info_.submit_id;
  last_result_.source_frame_serial = in_flight_info_.source_frame_serial;
  last_result_.stage = stage != nullptr ? stage : "";
  last_result_.reason = std::move(reason);
}

bool MonitorOutput::ValidateFrame(const Request &request, std::string *error) const {
  const RenderedFrame &frame = request.frame;
  if (!frame.IsValid()) {
    *error = "the submitted common result is not valid (null texture or zero size)";
    return false;
  }
  if (frame.width != output_.layout.width || frame.height != output_.layout.height) {
    *error = fmt::format("common result size {}x{} does not match the resolved layout {}x{}", frame.width, frame.height,
                         output_.layout.width, output_.layout.height);
    return false;
  }
  // The encode step is transfer-only, so the common result's primaries must
  // already be the output primaries; the coordination layer owns the mapping
  // target. Reject a mismatch instead of silently mis-tagging the wire data.
  enum pl_color_primaries expected = PL_COLOR_PRIM_BT_709;
  switch (output_.color.encoding.primaries) {
  case ColorPrimaries::BT709:
    expected = PL_COLOR_PRIM_BT_709;
    break;
  case ColorPrimaries::BT2020:
    expected = PL_COLOR_PRIM_BT_2020;
    break;
  default:
    *error = "unsupported monitor output primaries";
    return false;
  }
  if (frame.color.primaries != expected) {
    *error = fmt::format("common result primaries '{}' do not match the output primaries '{}'",
                         pl_color_primaries_name(frame.color.primaries), pl_color_primaries_name(expected));
    return false;
  }
  if (frame.color.transfer != PL_COLOR_TRC_LINEAR) {
    *error =
        fmt::format("common result transfer '{}' is not display-linear", pl_color_transfer_name(frame.color.transfer));
    return false;
  }
  if (!std::isfinite(frame.sdr_white_nits) || frame.sdr_white_nits <= 0.0f || !std::isfinite(frame.black_nits) ||
      frame.black_nits < 0.0f) {
    *error = "common result carries invalid SDR white/black luminance";
    return false;
  }
  return true;
}

bool MonitorOutput::ReapSlotIfPossible() {
  if (slot_ == nullptr) {
    return true;
  }
  try {
    packer_->Reap(slot_);
  } catch (const std::logic_error &) {
    // A Failed slot whose GPU work is still in use. Keep the handle; a later Pump
    // or Close() must finish the drain.
    return false;
  }
  slot_ = nullptr;
  return true;
}

bool MonitorOutput::AdvanceAndReapSlot() {
  if (slot_ == nullptr) {
    return true;
  }
  // One bounded, non-blocking poll so a Pending slot can reach Ready/Failed while
  // the module is already in a failed/terminal state. Never waits on the GPU.
  if (packer_->Poll(slot_) == PackPoll::NotReady) {
    return false;
  }
  return ReapSlotIfPossible();
}

void MonitorOutput::RecordTerminalEvent(const OutputEvent &event) {
  terminal_failed_ = true;
  if (event.message[0] != '\0') {
    terminal_reason_ = event.message;
  } else if (event.kind == OutputEvent::Kind::DeviceLost) {
    terminal_reason_ = "sink reported device loss";
  } else {
    terminal_reason_ = "sink reported an error event";
  }
}

void MonitorOutput::RecordCompletion(const OutputEvent &event) {
  if (event.kind != OutputEvent::Kind::FrameCompleted) {
    return;
  }
  switch (event.result) {
  case CompletionResult::Completed:
    ++diagnostics_.paced_completed;
    break;
  case CompletionResult::DisplayedLate:
    ++diagnostics_.paced_late;
    SPDLOG_LOGGER_TRACE(Logger, "Monitor output frame {} was displayed late (dropped-frame semantics)",
                        event.submit_id);
    break;
  case CompletionResult::Dropped:
    ++diagnostics_.paced_sink_dropped;
    SPDLOG_LOGGER_TRACE(Logger, "Monitor output frame {} was dropped by the sink (dropped-frame semantics)",
                        event.submit_id);
    break;
  case CompletionResult::Flushed:
    ++diagnostics_.paced_flushed;
    SPDLOG_LOGGER_TRACE(Logger, "Monitor output frame {} was flushed by the sink", event.submit_id);
    break;
  }
}

int MonitorOutput::DrainSinkEvents() {
  int events_drained = 0;
  OutputEvent event{};
  while (events_drained < kMaxSinkEventsPerPump) {
    if (!sink_->PollEvent(&event)) {
      break;
    }
    ++events_drained;
    if (event.kind == OutputEvent::Kind::Error || event.kind == OutputEvent::Kind::DeviceLost) {
      RecordTerminalEvent(event);
      break;
    }
    if (event.kind == OutputEvent::Kind::PlaybackStopped) {
      stop_pending_ = false;
      continue;
    }
    RecordCompletion(event);
  }
  return events_drained;
}

MonitorOutput::SubmitOutcome MonitorOutput::Submit(Request &request) {
  if (closed_) {
    SetResult(Outcome::Failed, "submit", "monitor output is closed");
    return SubmitOutcome::Closed;
  }
  if (closing_) {
    SetResult(Outcome::Failed, "submit", "monitor output is closing");
    return SubmitOutcome::Closed;
  }
  if (terminal_failed_) {
    SetResult(Outcome::Failed, "submit", terminal_reason_);
    return SubmitOutcome::Failed;
  }
  if (request_in_flight_) {
    // Busy: do not read, encode, modify or consume the new input in any way.
    return SubmitOutcome::Busy;
  }

  in_flight_info_ = request.info;

  std::string frame_error;
  if (!ValidateFrame(request, &frame_error)) {
    SetResult(Outcome::Failed, "config", std::move(frame_error));
    return SubmitOutcome::Failed;
  }

  // GPU-side NaN/Inf gate: no full-frame host readback. A non-finite common result
  // is a hard error, never encoded and never silently clamped. EncodeAndPack runs it
  // so both the still and the paced path are covered.
  std::string stage;
  std::string reason;
  StagingSlot *packed_slot = nullptr;
  if (!EncodeAndPack(request.frame, in_flight_info_, &packed_slot, &stage, &reason)) {
    SetResult(Outcome::Failed, stage.c_str(), std::move(reason));
    return SubmitOutcome::Failed;
  }
  slot_ = packed_slot;

  request_in_flight_ = true;
  drain_pending_ = false;
  cancel_requested_ = false;
  SetResult(Outcome::None, "", "");
  return SubmitOutcome::Accepted;
}

bool MonitorOutput::EncodeAndPack(const RenderedFrame &frame, const SubmissionInfo &info, StagingSlot **slot,
                                  std::string *stage, std::string *reason) {
  *slot = nullptr;

  // GPU-side NaN/Inf gate: the only readback is a single flag word, so continuous
  // playback never downloads a full frame. A non-finite result is a hard error.
  try {
    finite_gate_->Validate(frame.texture);
    ++diagnostics_.finite_checks;
  } catch (const std::exception &e) {
    ++diagnostics_.finite_failures;
    *stage = "finite";
    *reason = e.what();
    return false;
  }

  try {
    // Encode the common display-linear result into the wire transfer. Transfer
    // only: the primaries already match (validated by the caller), no tone/gamut
    // mapping runs here.
    EncodeParams params;
    params.transfer =
        output_.color.encoding.transfer == ColorTransfer::PQ ? OutputTransfer::PQ : OutputTransfer::SDR_BT1886;
    params.sdr_white_nits = frame.sdr_white_nits;
    params.black_nits = frame.black_nits;
    encoder_->Encode(target_.tex, frame.texture, params);
  } catch (const std::exception &e) {
    *stage = "render";
    *reason = e.what();
    return false;
  } catch (...) {
    *stage = "render";
    *reason = "unknown encode failure";
    return false;
  }
  ++diagnostics_.render_count;

  StagingSlot *packed = packer_->Submit(target_.tex, info.submit_id);
  ++diagnostics_.pack_submit_count;
  if (packed == nullptr) {
    // Ring full or shutting down. The paced caller treats this as transient and
    // drops the oldest unscheduled frame; the still caller reports it as a failure.
    *stage = "pack";
    *reason = "FramePacker::Submit returned nullptr (ring full or shutting down)";
    return false;
  }
  *slot = packed;
  return true;
}

void MonitorOutput::Cancel() {
  if (request_in_flight_) {
    cancel_requested_ = true;
  }
}

MonitorOutput::StepResult MonitorOutput::Pump() {
  if (closed_) {
    return last_result_;
  }
  if (closing_) {
    // Close() owns the packer/sink from here; do not poll handles it may recycle.
    return last_result_;
  }

  // Handle a bounded number of non-blocking sink events BEFORE any DisplayStill, so
  // an already-queued Error/DeviceLost can never be hidden by a display.
  const int events_drained = DrainSinkEvents();
  const bool event_cap_reached = events_drained == kMaxSinkEventsPerPump;

  if (!request_in_flight_) {
    if (terminal_failed_) {
      // Publish the terminal reason even when there was no request in flight, so
      // last_result() never keeps reporting the previous success.
      SetResult(Outcome::Failed, "event", terminal_reason_);
    }
    return last_result_;
  }

  if (terminal_failed_) {
    // The in-flight request cannot succeed. Publish the failure now and keep
    // draining its slot; the resources stay alive until the packer allows the reclaim.
    SetResult(Outcome::Failed, "event", terminal_reason_);
    const bool drained = AdvanceAndReapSlot();
    request_in_flight_ = !drained;
    drain_pending_ = !drained;
    return last_result_;
  }

  if (drain_pending_) {
    // The failure was already published; only finish reclaiming the resources.
    // Never fall back to a pending result while a failed request still owns GPU work.
    const bool drained = AdvanceAndReapSlot();
    request_in_flight_ = !drained;
    drain_pending_ = !drained;
    return last_result_;
  }

  if (event_cap_reached) {
    // The queue may still hold a fatal event; defer the display to the next Pump.
    return last_result_;
  }

  const PackPoll poll = packer_->Poll(slot_);
  if (poll == PackPoll::Failed) {
    ++diagnostics_.pack_failed;
    // Publish the failure immediately even if the slot cannot be reclaimed yet:
    // "Failed" means the request failed, not that its resources are free.
    SetResult(Outcome::Failed, "pack", "packer slot reported a failure");
    const bool drained = ReapSlotIfPossible();
    request_in_flight_ = !drained;
    drain_pending_ = !drained;
    return last_result_;
  }
  if (poll == PackPoll::NotReady) {
    return last_result_;
  }

  // The packed bytes are Ready.
  if (cancel_requested_) {
    // A logically cancelled request must never reach DisplayStill.
    ReapSlotIfPossible();
    request_in_flight_ = false;
    drain_pending_ = false;
    SetResult(Outcome::Cancelled, "cancel", "request cancelled before display");
    return last_result_;
  }

  PackedFrameView view{packer_->Bytes(slot_), packer_->Size(slot_)};
  ++diagnostics_.still_attempts;
  const StillOutcome outcome = sink_->DisplayStill(view, in_flight_info_);
  if (outcome == StillOutcome::Busy) {
    ++diagnostics_.still_busy;
    // Keep the SAME ready slot, bytes and SubmissionInfo; retry on the next Pump.
    return last_result_;
  }
  if (outcome == StillOutcome::Displayed) {
    ++diagnostics_.displayed;
    ReapSlotIfPossible();
    request_in_flight_ = false;
    drain_pending_ = false;
    SetResult(Outcome::Displayed, "still", "");
    return last_result_;
  }

  // DeviceError / Closed: nothing was displayed and the sink is terminal. Publish
  // the failure and never retry the sink as Busy.
  terminal_failed_ = true;
  terminal_reason_ = outcome == StillOutcome::Closed ? "sink is closed" : "sink reported a device error";
  SetResult(Outcome::Failed, "still", terminal_reason_);
  const bool drained = ReapSlotIfPossible();
  request_in_flight_ = !drained;
  drain_pending_ = !drained;
  return last_result_;
}

void MonitorOutput::Tick() {
  if (closed_ || closing_ || terminal_failed_ || failed_) {
    return;
  }
  if (playback_started_ || playback_requested_) {
    TickPaced();
    return;
  }
  TickStill();
}

void MonitorOutput::TickStill() {
  // DisplayStill is only legal once scheduled playback is confirmed idle (the SDK
  // forbids the two from coexisting). Wait for the PlaybackStopped event; the gap is
  // not a failure.
  if (stop_pending_) {
    DrainSinkEvents();
    if (stop_pending_ || terminal_failed_) {
      return;
    }
    // The session is idle now. Force exactly one still display of the current frame
    // so the monitor freezes on the last composed result instead of depending on the
    // unverified run-empty hold behavior.
    last_submitted_render_id_ = 0;
  }

  if (request_in_flight_) {
    const StepResult step = Pump();
    if (request_in_flight_) {
      // Still draining; pump it again next turn.
      return;
    }
    switch (step.outcome) {
    case Outcome::Displayed:
      Logger->debug("Monitor output displayed request {} (serial {})", step.submit_id, step.source_frame_serial);
      break;
    case Outcome::Cancelled:
      Logger->info("Monitor output cancelled request {}", step.submit_id);
      break;
    case Outcome::Failed:
      failed_ = true;
      Logger->error("Monitor output request {} failed at stage '{}': {}", step.submit_id, step.stage, step.reason);
      break;
    case Outcome::None:
      break;
    }
    return;
  }

  if (failed_ || !AcceptsRequests()) {
    return;
  }

  const RenderedFrame &frame = coordinator_->result();
  if (!frame.IsValid()) {
    return;
  }
  // resize/OSD do not change render_id, so they never resubmit and never change
  // the packed bytes; a real scene change composes again and gets a new render_id.
  if (frame.identity.render_id == last_submitted_render_id_) {
    return;
  }
  SubmitLatest(frame);
}

void MonitorOutput::SubmitLatest(const RenderedFrame &frame) {
  Request request;
  request.frame = frame;
  request.info.submit_id = ++submit_id_;
  request.info.source_frame_serial = frame.identity.source_serials;

  const SubmitOutcome outcome = Submit(request);
  if (outcome == SubmitOutcome::Accepted) {
    last_submitted_render_id_ = frame.identity.render_id;
    Logger->debug("Monitor output submitted request {} (scene {}, render {}, serial {})", request.info.submit_id,
                  frame.identity.scene_revision, frame.identity.render_id, frame.identity.source_serials);
    return;
  }

  Logger->error("Monitor output submit rejected (outcome {}): stage '{}': {}", static_cast<int>(outcome),
                last_result_.stage, last_result_.reason);
  // Never silently restart a failed output: keep the desktop running and report.
  failed_ = true;
}

// ---------------------------------------------------------------------------
// Playback (paced) path
// ---------------------------------------------------------------------------

void MonitorOutput::SetPlayback(bool enabled) {
  if (playback_requested_ == enabled) {
    return;
  }
  playback_requested_ = enabled;
  if (enabled) {
    Logger->info("Monitor output playback enabled (paced to the output mode clock)");
  } else {
    // Stop enqueuing new frames. The already-queued ones drain in TickPaced; the
    // session is stopped and the still path takes over once the queue is empty, so
    // pause freezes the last frame and a subsequent seek can still be displayed.
    Logger->info("Monitor output playback paused; draining the queued frames");
  }
}

bool MonitorOutput::EnsurePlaybackStarted(std::string *error) {
  if (playback_started_) {
    return true;
  }
  if (!sink_->StartScheduled(0, error)) {
    return false;
  }
  playback_started_ = true;
  stop_pending_ = false;
  last_scheduled_display_time_ = -1;
  Logger->debug("Monitor output scheduled playback started");
  return true;
}

int64_t MonitorOutput::NextDisplayTick(int64_t clock) const {
  const int64_t frame_duration = output_.mode.frame_duration;
  // A valid mode always has a positive frame duration (validated at construction).
  const int64_t base = clock + static_cast<int64_t>(kSubmitLeadFrames) * frame_duration;
  int64_t tick = (base / frame_duration) * frame_duration;
  if (tick < base) {
    tick += frame_duration;
  }
  // Never reuse or go back: display times must be strictly increasing per session.
  if (tick <= last_scheduled_display_time_) {
    tick = last_scheduled_display_time_ + frame_duration;
  }
  return tick;
}

void MonitorOutput::DropPacedFront() {
  if (paced_.empty()) {
    return;
  }
  StagingSlot *slot = paced_.front().slot;
  paced_.pop_front();
  if (slot != nullptr) {
    packer_->Abort(slot);
  }
}

void MonitorOutput::SubmitPacedLatest() {
  const RenderedFrame &frame = coordinator_->result();
  if (!frame.IsValid()) {
    return;
  }
  if (frame.identity.render_id == last_paced_render_id_) {
    return;
  }

  // The paced queue is bounded by the packer ring. On overflow the oldest
  // unscheduled frame is dropped so the newest one wins (desktop mailbox semantics).
  while (paced_.size() >= static_cast<std::size_t>(FramePacker::kRingCapacity)) {
    ++diagnostics_.paced_dropped;
    DropPacedFront();
  }

  const uint64_t render_id = frame.identity.render_id;
  SubmissionInfo info;
  // Share submit_id_ with the still path so SubmissionInfo identities stay unique
  // across a session that mixes still and paced frames.
  info.submit_id = ++submit_id_;
  info.source_frame_serial = frame.identity.source_serials;

  std::string stage;
  std::string reason;
  StagingSlot *slot = nullptr;
  if (!EncodeAndPack(frame, info, &slot, &stage, &reason)) {
    if (stage == "pack") {
      // Ring still full after dropping the oldest frame (its GPU work has not been
      // reclaimed yet). Drop this composition too and retry with the next one.
      ++diagnostics_.paced_dropped;
      return;
    }
    // Encode failure is a per-frame hard failure, like the still path.
    SetResult(Outcome::Failed, stage.c_str(), std::move(reason));
    failed_ = true;
    return;
  }

  paced_.push_back(PacedFrame{slot, info});
  last_paced_render_id_ = render_id;
  ++diagnostics_.paced_submits;
}

void MonitorOutput::StopPlaybackSession() {
  if (!playback_started_) {
    return;
  }
  std::string error;
  int64_t actual_stop = 0;
  if (!sink_->StopScheduled(0, &actual_stop, &error)) {
    terminal_failed_ = true;
    terminal_reason_ = "StopScheduledPlayback failed: " + error;
    SetResult(Outcome::Failed, "stop", terminal_reason_);
    playback_started_ = false;
    return;
  }
  playback_started_ = false;
  stop_pending_ = true;
  Logger->debug("Monitor output scheduled playback stopped");
}

void MonitorOutput::TickPaced() {
  const int drained = DrainSinkEvents();
  if (terminal_failed_) {
    // The paced frames cannot succeed. Publish the terminal failure; the slots stay
    // alive until Close() reclaims them.
    SetResult(Outcome::Failed, "event", terminal_reason_);
    return;
  }
  if (drained == kMaxSinkEventsPerPump) {
    // A fatal event may still be queued; do not schedule more work this turn.
    return;
  }

  // A previous StopScheduled has not been confirmed idle yet (PlaybackStopped not
  // received). Do not try to start a new session while the backend is still Stopping;
  // this makes a rapid pause->play transition safe.
  if (stop_pending_) {
    return;
  }

  // A freeze still can still be draining its readback when playback resumes right
  // after a pause; its bytes were never displayed and are moot now. Drain the slot
  // one poll per turn (never blocking) so the paced queue regains the full ring
  // within a few ticks. Abort is not used here: an aborted slot is only reclaimed
  // once its SOURCE texture polls idle, and the encode target keeps being rewritten
  // every paced frame, which would starve the reclamation for the whole session.
  if (request_in_flight_) {
    const PackPoll drain = slot_ != nullptr ? packer_->Poll(slot_) : PackPoll::Failed;
    if (drain == PackPoll::Ready) {
      packer_->Reap(slot_);
      slot_ = nullptr;
      request_in_flight_ = false;
      SetResult(Outcome::None, "", "");
    } else if (drain == PackPoll::Failed) {
      // Mirror the still path: a readback failure is reported and the module stops.
      // The slot stays retained until Close() reclaims it.
      request_in_flight_ = false;
      SetResult(Outcome::Failed, "pack", "freeze still readback failed on resume");
      failed_ = true;
    }
  }

  // Paused: once every already-queued frame has been scheduled, stop the session so
  // the still path can take over (pause freezes the last frame). With the switch off
  // the session is left running and the hardware is expected to hold the last frame.
  if (!playback_requested_ && paced_.empty()) {
    if (kPauseStopsScheduledPlayback) {
      StopPlaybackSession();
    }
    return;
  }

  if (!playback_started_) {
    std::string error;
    if (!EnsurePlaybackStarted(&error)) {
      terminal_failed_ = true;
      terminal_reason_ = "StartScheduledPlayback failed: " + error;
      SetResult(Outcome::Failed, "start", terminal_reason_);
      return;
    }
  }

  if (!paced_.empty()) {
    PacedFrame &head = paced_.front();
    const PackPoll poll = packer_->Poll(head.slot);
    if (poll == PackPoll::Failed) {
      // A pack readback failure drops this frame; it does not poison the session.
      ++diagnostics_.pack_failed;
      ++diagnostics_.paced_dropped;
      SPDLOG_LOGGER_TRACE(Logger, "Monitor output paced frame {} failed to pack; dropped", head.info.submit_id);
      DropPacedFront();
      return;
    }
    if (poll == PackPoll::Ready) {
      int64_t clock = 0;
      std::string error;
      if (!sink_->QueryClock(output_.mode.time_scale, &clock, nullptr, &error)) {
        terminal_failed_ = true;
        terminal_reason_ = "QueryClock failed: " + error;
        SetResult(Outcome::Failed, "clock", terminal_reason_);
        return;
      }
      const int64_t display_time = NextDisplayTick(clock);
      PackedFrameView view{packer_->Bytes(head.slot), packer_->Size(head.slot)};
      const ScheduleOutcome outcome = sink_->TrySchedule(view, head.info, display_time, output_.mode.frame_duration);
      if (outcome == ScheduleOutcome::Accepted) {
        ++diagnostics_.paced_scheduled;
        last_scheduled_display_time_ = display_time;
        Logger->debug("Monitor output scheduled paced frame {} at {}, serial {}", head.info.submit_id, display_time,
                      head.info.source_frame_serial);
        packer_->Reap(head.slot);
        paced_.pop_front();
      } else if (outcome == ScheduleOutcome::Busy) {
        // Device frame pool full: NOTHING was consumed. Keep this frame at the head
        // and still consider newer compositions, so overflow drops the oldest
        // unscheduled frame instead of stalling the newest one (newest wins).
      } else {
        terminal_failed_ = true;
        terminal_reason_ =
            outcome == ScheduleOutcome::Closed ? "sink closed while scheduling" : "sink rejected a scheduled frame";
        SetResult(Outcome::Failed, "schedule", terminal_reason_);
        return;
      }
    }
  }

  if (playback_requested_) {
    SubmitPacedLatest();
  }
}

CloseStatus MonitorOutput::Close(int drain_timeout_ms) {
  if (closed_) {
    return CloseStatus::Closed;
  }
  closing_ = true;
  close_reason_.clear();

  const int bounded = drain_timeout_ms < 0 ? 0 : drain_timeout_ms;
  const auto deadline = Clock::now() + std::chrono::milliseconds(bounded);
  auto remaining_ms = [&]() -> int {
    const auto now = Clock::now();
    if (now >= deadline) {
      return 0;
    }
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
  };

  // 0. Stop a live scheduled session first: nothing new may be scheduled after a
  //    Close started. StopScheduled is idempotent (the backend reports Idle as a
  //    no-op), so a retried Close is safe.
  if (playback_started_) {
    std::string stop_error;
    int64_t actual_stop = 0;
    sink_->StopScheduled(0, &actual_stop, &stop_error);
    playback_started_ = false;
  }
  playback_requested_ = false;

  // 0b. Drop every paced frame so the packer can reach Idle. A Ready paced slot is
  //     owned by us (Abort frees it); a Pending one becomes Aborted and is cancelled
  //     by Shutdown.
  while (!paced_.empty()) {
    StagingSlot *slot = paced_.front().slot;
    paced_.pop_front();
    if (slot != nullptr) {
      packer_->Abort(slot);
    }
  }

  // 0c. Drain the queued sink events so completed frames return their device frame
  //     budget before the sink is asked to Close. The real backend also reconciles
  //     internally; the scripted/fake sinks need the events consumed.
  {
    OutputEvent ignored{};
    while (sink_->PollEvent(&ignored)) {
    }
  }

  // 1. Reclaim our own Ready slot (including a slot the sink left Busy) BEFORE
  //    Shutdown; otherwise the packer would keep reporting TimedOut forever. A
  //    Pending/Failed slot is left for Shutdown to cancel.
  if (request_in_flight_ && slot_ != nullptr) {
    const PackPoll poll = packer_->Poll(slot_);
    if (poll == PackPoll::Ready) {
      ReapSlotIfPossible();
      request_in_flight_ = false;
      drain_pending_ = false;
    } else if (poll == PackPoll::Failed && ReapSlotIfPossible()) {
      request_in_flight_ = false;
      drain_pending_ = false;
    }
  }
  cancel_requested_ = true;

  // 2. Drain the packer with the remaining budget. After Shutdown the slot handle
  //    is owned by the packer and must not be Polled/Reap'ed again.
  const PackDrain drain = packer_->Shutdown(remaining_ms());
  slot_ = nullptr;
  if (drain == PackDrain::Idle) {
    request_in_flight_ = false;
    drain_pending_ = false;
  }

  // 3. Close the owned sink with whatever budget is left.
  const CloseStatus sink_status = sink_->Close(remaining_ms());

  // Only a truly idle packer and a closed sink complete the shutdown.
  if (drain == PackDrain::Idle && sink_status == CloseStatus::Closed) {
    closed_ = true;
    close_reason_ = "closed";
    return CloseStatus::Closed;
  }
  if (drain == PackDrain::Failed) {
    close_reason_ = "packer shutdown reported a GPU failure; resources are retained";
    return CloseStatus::DeviceError;
  }
  if (sink_status == CloseStatus::DeviceError) {
    close_reason_ = "sink reported a device error while closing; resources are retained";
    return CloseStatus::DeviceError;
  }
  close_reason_ = "packer or sink did not drain within the total deadline; retry Close";
  return CloseStatus::DrainTimeout;
}
