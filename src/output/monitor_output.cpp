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

MonitorOutput::MonitorOutput(RenderContext &context, const ResolvedOutput &output, FrameSink &sink, DisplayMode mode,
                             ScaleMethod scale_method, ScaleMethod plane_scale_method)
    : context_(&context), sink_(&sink), output_(output), mode_(mode) {
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

  FrameRenderer::Config config;
  config.mode = mode;
  config.scale_method = scale_method;
  config.plane_scale_method = plane_scale_method;
  config.pixel_density = 1.0f;
  config.enable_osd = false;      // no SDL_ttf/font dependency for offscreen output
  config.colorspace_hint = false; // no swapchain to hint
  renderer_ = std::make_unique<FrameRenderer>(context, config);

  pl_gpu gpu = context.gpu();
  // Only resources this constructor acquired are cleaned up on failure. The
  // borrowed context and sink are never closed or destroyed here.
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
    // override the source mastering peak (no SetTargetDisplayNits(..., true)).
    target_.frame.color.hdr.max_luma = target_peak;

    packer_ = std::make_unique<FramePacker>(gpu, ToPackConfig(output_));
  } catch (...) {
    // The destructor will not run for a throwing constructor; release the raw GPU
    // resource acquired so far. renderer_/packer_ are cleaned up by their unique_ptr.
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
  renderer_.reset();
}

void MonitorOutput::SetResult(Status status, const char *stage, std::string reason) {
  status_ = status;
  last_result_.status = status;
  last_result_.submit_id = in_flight_info_.submit_id;
  last_result_.source_frame_serial = in_flight_info_.source_frame_serial;
  last_result_.stage = stage != nullptr ? stage : "";
  last_result_.reason = std::move(reason);
}

MonitorOutput::StepResult MonitorOutput::MakeResult(Status status) const {
  StepResult result = last_result_;
  result.status = status;
  return result;
}

bool MonitorOutput::ValidateScene(const Request &request, std::string *error) const {
  // FeedFrames maps every entry of the submitted frame set, not only the ones this
  // scene references. Reject any null entry here, before any mapping happens: the
  // renderer must not have to defend against it (av_frame_ref on a null frame
  // crashes). The Busy path returns before this function is ever called, so an
  // in-flight request still does not read a new input.
  for (const auto &[id, frame] : request.frames) {
    if (frame == nullptr) {
      *error = fmt::format("submitted frame for video {} is null", id);
      return false;
    }
  }

  auto require = [&](int id) -> bool {
    if (request.frames.find(id) == request.frames.end()) {
      *error = fmt::format("scene references video {} which is not part of the submitted frame set", id);
      return false;
    }
    return true;
  };

  if (!require(request.main_source_id)) {
    return false;
  }

  const DisplayState &state = request.state;
  if (state.show_ref && !state.ref_id.has_value()) {
    *error = "show_ref is set but ref_id is missing";
    return false;
  }

  // Collect every source the renderer will actually sample for this mode. This
  // mirrors fill_render.cpp / slide_render.cpp / grid_render.cpp branch by branch:
  // an amplify reference is sampled whenever it is present, even in the non-ref
  // view, so it must come from THIS request and never from the renderer's cache.
  std::vector<int> needed;
  switch (mode_) {
  case DisplayMode::Fill:
    if (state.show_ref) {
      needed.push_back(state.ref_id.value());
    } else {
      needed.push_back(state.fill.id);
      if (state.amplify.has_value() && state.ref_id.has_value()) {
        needed.push_back(state.ref_id.value());
      }
    }
    break;
  case DisplayMode::Slide:
    if (state.show_ref) {
      needed.push_back(state.ref_id.value());
    } else {
      needed.push_back(state.slide.left_id);
      needed.push_back(state.slide.right_id);
      if (state.amplify.has_value() && state.ref_id.has_value()) {
        needed.push_back(state.ref_id.value());
      }
    }
    break;
  case DisplayMode::Grid: {
    // GridRender uses grid.cols/rows for both the normal and the reference view (in
    // the reference view every cell samples ref_id but the cell layout is still
    // computed from the grid), so validate the structure once for both views. A
    // trailing empty cell is legal: GridRender skips ids past the end of the list,
    // so only the actually visible ids are required to be present.
    if (state.grid.rows <= 0 || state.grid.cols <= 0) {
      *error = fmt::format("grid layout has an invalid size {}x{}", state.grid.cols, state.grid.rows);
      return false;
    }
    if (state.ids.empty()) {
      *error = "grid has no video ids";
      return false;
    }
    if (state.show_ref) {
      needed.push_back(state.ref_id.value());
    } else {
      const std::size_t rows = static_cast<std::size_t>(state.grid.rows);
      const std::size_t cols = static_cast<std::size_t>(state.grid.cols);
      const std::size_t visible = std::min(rows * cols, state.ids.size());
      for (std::size_t i = 0; i < visible; ++i) {
        needed.push_back(state.ids[i]);
      }
      if (state.amplify.has_value() && state.ref_id.has_value()) {
        needed.push_back(state.ref_id.value());
      }
    }
    break;
  }
  default:
    *error = "unsupported display mode for monitor output";
    return false;
  }

  for (int id : needed) {
    if (!require(id)) {
      return false;
    }
  }
  return true;
}

bool MonitorOutput::ReapSlotIfPossible() {
  if (slot_ == nullptr) {
    in_flight_frames_.clear();
    return true;
  }
  try {
    packer_->Reap(slot_);
  } catch (const std::logic_error &) {
    // A Failed slot whose GPU work is still in use. Keep the handle and the source
    // refs; a later Pump or Close() must finish the drain.
    return false;
  }
  slot_ = nullptr;
  in_flight_frames_.clear();
  return true;
}

bool MonitorOutput::AdvanceAndReapSlot() {
  if (slot_ == nullptr) {
    in_flight_frames_.clear();
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
    SetResult(Status::Closed, "submit", "monitor output is closed");
    return SubmitOutcome::Closed;
  }
  if (closing_) {
    SetResult(Status::Closed, "submit", "monitor output is closing");
    return SubmitOutcome::Closed;
  }
  if (terminal_failed_) {
    SetResult(Status::Failed, "submit", terminal_reason_);
    return SubmitOutcome::Failed;
  }
  if (request_in_flight_) {
    // Busy: do not read, map, modify or consume the new input in any way.
    return SubmitOutcome::Busy;
  }

  in_flight_info_ = request.info;

  std::string scene_error;
  if (!ValidateScene(request, &scene_error)) {
    SetResult(Status::Failed, "config", std::move(scene_error));
    return SubmitOutcome::Failed;
  }

  in_flight_frames_ = request.frames; // shared refs keep the AVFrame headers alive
  const DisplayState &state = request.state;
  renderer_->SetMainSource(request.main_source_id);

  std::vector<int> failed_ids;
  try {
    failed_ids = renderer_->FeedFrames(in_flight_frames_);
  } catch (const std::exception &e) {
    // Publish the failure for this request before propagating, so status() and
    // last_result() can never describe the failure with a previous success.
    in_flight_frames_.clear();
    SetResult(Status::Failed, "feed", e.what());
    throw;
  }
  if (!failed_ids.empty()) {
    std::string reason = fmt::format("frame update failed for video {}", failed_ids.front());
    in_flight_frames_.clear();
    SetResult(Status::Failed, "feed", std::move(reason));
    return SubmitOutcome::Failed;
  }

  try {
    renderer_->Render(target_.frame, state);
  } catch (const std::exception &e) {
    in_flight_frames_.clear();
    SetResult(Status::Failed, "render", e.what());
    throw;
  } catch (...) {
    in_flight_frames_.clear();
    SetResult(Status::Failed, "render", "unknown render failure");
    throw;
  }
  ++diagnostics_.render_count;

  slot_ = packer_->Submit(target_.tex, in_flight_info_.submit_id);
  ++diagnostics_.pack_submit_count;
  if (slot_ == nullptr) {
    in_flight_frames_.clear();
    SetResult(Status::Failed, "pack", "FramePacker::Submit returned nullptr (ring full or shutting down)");
    return SubmitOutcome::Failed;
  }

  request_in_flight_ = true;
  drain_pending_ = false;
  cancel_requested_ = false;
  SetResult(Status::Pending, "", "");
  return SubmitOutcome::Accepted;
}

void MonitorOutput::Cancel() {
  if (request_in_flight_) {
    cancel_requested_ = true;
  }
}

MonitorOutput::StepResult MonitorOutput::Pump() {
  if (closed_) {
    return MakeResult(Status::Closed);
  }
  if (closing_) {
    // Close() owns the packer/sink from here; do not poll handles it may recycle.
    return MakeResult(Status::Pending);
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
      // status()/last_result() never keep reporting the previous Idle/success.
      SetResult(Status::Failed, "event", terminal_reason_);
      return last_result_;
    }
    return MakeResult(Status::Idle);
  }

  if (terminal_failed_) {
    // The in-flight request cannot succeed. Publish the failure now and keep
    // draining its slot; the resources/source stay alive until the packer allows
    // the reclaim.
    SetResult(Status::Failed, "event", terminal_reason_);
    const bool drained = AdvanceAndReapSlot();
    request_in_flight_ = !drained;
    drain_pending_ = !drained;
    return last_result_;
  }

  if (drain_pending_) {
    // The failure was already published; only finish reclaiming the resources.
    // Never fall back to Pending/Busy while a failed request still owns GPU work.
    const bool drained = AdvanceAndReapSlot();
    request_in_flight_ = !drained;
    drain_pending_ = !drained;
    return last_result_;
  }

  if (event_cap_reached) {
    // The queue may still hold a fatal event; defer the display to the next Pump.
    return MakeResult(Status::Pending);
  }

  const PackPoll poll = packer_->Poll(slot_);
  if (poll == PackPoll::Failed) {
    ++diagnostics_.pack_failed;
    // Publish the failure immediately even if the slot cannot be reclaimed yet:
    // "Failed" means the request failed, not that its resources are free.
    SetResult(Status::Failed, "pack", "packer slot reported a failure");
    const bool drained = ReapSlotIfPossible();
    request_in_flight_ = !drained;
    drain_pending_ = !drained;
    return last_result_;
  }
  if (poll == PackPoll::NotReady) {
    return MakeResult(Status::Pending);
  }

  // The packed bytes are Ready.
  if (cancel_requested_) {
    // A logically cancelled request must never reach DisplayStill.
    ReapSlotIfPossible();
    request_in_flight_ = false;
    drain_pending_ = false;
    SetResult(Status::Cancelled, "cancel", "request cancelled before display");
    return last_result_;
  }

  PackedFrameView view{packer_->Bytes(slot_), packer_->Size(slot_)};
  ++diagnostics_.still_attempts;
  const StillOutcome outcome = sink_->DisplayStill(view, in_flight_info_);
  if (outcome == StillOutcome::Busy) {
    ++diagnostics_.still_busy;
    // Keep the SAME ready slot, bytes and SubmissionInfo; retry on the next Pump.
    return MakeResult(Status::Pending);
  }
  if (outcome == StillOutcome::Displayed) {
    ++diagnostics_.displayed;
    ReapSlotIfPossible();
    request_in_flight_ = false;
    drain_pending_ = false;
    SetResult(Status::Displayed, "still", "");
    return last_result_;
  }

  // DeviceError / Closed: nothing was displayed and the sink is terminal. Publish
  // the failure and never retry the sink as Busy.
  terminal_failed_ = true;
  terminal_reason_ = outcome == StillOutcome::Closed ? "sink is closed" : "sink reported a device error";
  SetResult(Status::Failed, "still", terminal_reason_);
  const bool drained = ReapSlotIfPossible();
  request_in_flight_ = !drained;
  drain_pending_ = !drained;
  return last_result_;
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
    in_flight_frames_.clear();
  }

  // 3. Close the borrowed sink with whatever budget is left.
  const CloseStatus sink_status = sink_->Close(remaining_ms());

  // Only a truly idle packer and a closed sink complete the shutdown.
  if (drain == PackDrain::Idle && sink_status == CloseStatus::Closed) {
    closed_ = true;
    status_ = Status::Closed;
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
