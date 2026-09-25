#include "output/monitor_controller.hpp"

#include <stdexcept>
#include <utility>

#include <spdlog/fmt/fmt.h>

#include "log.hpp"

using namespace EYEQ;

namespace {

constexpr int kDefaultCloseTimeoutMs = 5000;

} // namespace

MonitorController::MonitorController(RenderContext &context, DisplayCoordinator &coordinator,
                                     std::unique_ptr<FrameSink> sink, const ResolvedOutput &resolved)
    : context_(&context), coordinator_(&coordinator), sink_(std::move(sink)), resolved_(resolved) {
  if (sink_ == nullptr) {
    throw std::invalid_argument("MonitorController requires a FrameSink");
  }
  // If this throws, the only resources acquired so far are the sink (owned by
  // sink_, which the unique_ptr then destroys/closes) and nothing else; the
  // MonitorOutput constructor rolls back only what it acquired itself.
  monitor_ = std::make_unique<MonitorOutput>(context, resolved_, *sink_);
}

MonitorController::~MonitorController() {
  if (monitor_) {
    const CloseStatus status = monitor_->Close(kDefaultCloseTimeoutMs);
    if (status != CloseStatus::Closed) {
      Logger->error("MonitorController destroyed without a clean close: {} ({})", static_cast<int>(status),
                    monitor_->close_reason());
    }
  }
  // The monitor is destroyed here so no GPU work of ours references the borrowed
  // result any more; only then is releasing the borrow safe.
  monitor_.reset();
  if (borrowed_) {
    ReleaseBorrow();
  }
}

void MonitorController::Tick() {
  if (closed_ || monitor_ == nullptr) {
    return;
  }

  if (monitor_->in_flight()) {
    const MonitorOutput::StepResult step = monitor_->Pump();
    if (monitor_->in_flight()) {
      // Still draining; the borrow must stay until the packer reclaims the slot.
      return;
    }
    ReleaseBorrow();
    switch (step.status) {
    case MonitorOutput::Status::Displayed:
      Logger->debug("Monitor output displayed request {} (serial {})", step.submit_id, step.source_frame_serial);
      break;
    case MonitorOutput::Status::Cancelled:
      Logger->info("Monitor output cancelled request {}", step.submit_id);
      break;
    case MonitorOutput::Status::Failed:
      failed_ = true;
      Logger->error("Monitor output request {} failed at stage '{}': {}", step.submit_id, step.stage, step.reason);
      break;
    default:
      break;
    }
    return;
  }

  if (borrowed_) {
    // Should not happen (the borrow is released with the terminating request), but
    // never leave the coordinator stuck if it does.
    ReleaseBorrow();
  }
  if (failed_ || !monitor_->AcceptsRequests()) {
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

void MonitorController::Cancel() {
  if (monitor_ != nullptr) {
    monitor_->Cancel();
  }
}

void MonitorController::SubmitLatest(const RenderedFrame &frame) {
  coordinator_->AcquireResult();
  borrowed_ = true;

  MonitorOutput::Request request;
  request.frame = frame;
  request.info.submit_id = ++submit_id_;
  request.info.batch_seq = 0;
  request.info.timeline_epoch = frame.identity.scene_revision;
  request.info.output_index = 0;
  request.info.source_frame_serial = frame.identity.source_serials;

  const MonitorOutput::SubmitOutcome outcome = monitor_->Submit(request);
  if (outcome == MonitorOutput::SubmitOutcome::Accepted) {
    last_submitted_render_id_ = frame.identity.render_id;
    Logger->debug("Monitor output submitted request {} (scene {}, render {}, serial {})", submit_id_,
                  frame.identity.scene_revision, frame.identity.render_id, frame.identity.source_serials);
    return;
  }

  ReleaseBorrow();
  const MonitorOutput::StepResult &result = monitor_->last_result();
  Logger->error("Monitor output submit rejected (outcome {}): stage '{}': {}", static_cast<int>(outcome), result.stage,
                result.reason);
  // Never silently restart a failed output: keep the desktop running and report.
  failed_ = true;
}

void MonitorController::ReleaseBorrow() {
  if (!borrowed_) {
    return;
  }
  coordinator_->ReleaseResult();
  borrowed_ = false;
}

CloseStatus MonitorController::Close(int drain_timeout_ms) {
  if (closed_) {
    return CloseStatus::Closed;
  }
  const CloseStatus status = monitor_->Close(drain_timeout_ms);
  if (status == CloseStatus::Closed) {
    closed_ = true;
    ReleaseBorrow();
  } else {
    // DrainTimeout / DeviceError: the monitor may still own GPU work that
    // references the borrowed common result. Keep the borrow so the coordinator
    // cannot overwrite the texture; the caller must retry Close or keep the
    // resources alive and report. Nothing is force-destroyed here.
    Logger->error("Monitor output did not close: {} ({}); borrowed resources are retained", static_cast<int>(status),
                  monitor_->close_reason());
  }
  return status;
}
