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

  try {
    // Encode the common display-linear result into the wire transfer. Transfer
    // only: the primaries already match (validated above), no tone/gamut mapping
    // runs here.
    EncodeParams params;
    params.transfer =
        output_.color.encoding.transfer == ColorTransfer::PQ ? OutputTransfer::PQ : OutputTransfer::SDR_BT1886;
    params.sdr_white_nits = request.frame.sdr_white_nits;
    params.black_nits = request.frame.black_nits;
    encoder_->Encode(target_.tex, request.frame.texture, params);
  } catch (const std::exception &e) {
    SetResult(Outcome::Failed, "render", e.what());
    return SubmitOutcome::Failed;
  } catch (...) {
    SetResult(Outcome::Failed, "render", "unknown encode failure");
    return SubmitOutcome::Failed;
  }
  ++diagnostics_.render_count;

  slot_ = packer_->Submit(target_.tex, in_flight_info_.submit_id);
  ++diagnostics_.pack_submit_count;
  if (slot_ == nullptr) {
    SetResult(Outcome::Failed, "pack", "FramePacker::Submit returned nullptr (ring full or shutting down)");
    return SubmitOutcome::Failed;
  }

  request_in_flight_ = true;
  drain_pending_ = false;
  cancel_requested_ = false;
  SetResult(Outcome::None, "", "");
  return SubmitOutcome::Accepted;
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
  }
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
  if (closed_ || closing_) {
    return;
  }

  if (request_in_flight_) {
    const StepResult step = Pump();
    if (request_in_flight_) {
      // Still draining; the borrow must stay until the packer reclaims the slot.
      return;
    }
    ReleaseBorrow();
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

  if (borrowed_) {
    // Should not happen (the borrow is released with the terminating request), but
    // never leave the coordinator stuck if it does.
    ReleaseBorrow();
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
  AcquireBorrow();

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

  ReleaseBorrow();
  Logger->error("Monitor output submit rejected (outcome {}): stage '{}': {}", static_cast<int>(outcome),
                last_result_.stage, last_result_.reason);
  // Never silently restart a failed output: keep the desktop running and report.
  failed_ = true;
}

void MonitorOutput::AcquireBorrow() {
  if (borrowed_) {
    return;
  }
  coordinator_->AcquireResult();
  borrowed_ = true;
}

void MonitorOutput::ReleaseBorrow() {
  if (!borrowed_) {
    return;
  }
  coordinator_->ReleaseResult();
  borrowed_ = false;
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
