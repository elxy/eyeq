// Fault-injection test for the monitor composition layer. This target compiles the
// real frame_packer.cpp with EYEQ_FRAME_PACKER_TEST_HOOKS (and deliberately does not
// link eyeq_frame_packer), so the injected readback failure really hits the packer
// used by MonitorOutput.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/vulkan.h>

#include "display/display_coordinator.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"
#include "fake_frame_sink.hpp"
#include "frame_packer_test_hooks.hpp"
#include "output/monitor_output.hpp"
#include "output/output_profile.hpp"
#include "scripted_frame_sink.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (condition) {
    std::fprintf(stdout, "  ok   %s\n", what.c_str());
  } else {
    std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

using EYEQ::MonitorOutput;

int g_remaining_download_failures = 0;
bool g_gpu_lost = false;

bool HookDownload(pl_gpu gpu, const struct pl_tex_transfer_params *params) {
  if (g_remaining_download_failures > 0) {
    --g_remaining_download_failures;
    return false;
  }
  return pl_tex_download(gpu, params);
}

// Test-only device-loss signal: the real GPU stays healthy so the probe can reset
// the flag and let the retained slot drain. It is never a real GPU reset.
bool HookGpuIsFailed(pl_gpu gpu) {
  if (g_gpu_lost) {
    return true;
  }
  return pl_gpu_is_failed(gpu);
}

std::shared_ptr<AVFrame> MakeSolidFrame(AVStream *stream, int width, int height, float value) {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("av_frame_alloc failed");
  }
  frame->format = AV_PIX_FMT_GBRPF32LE;
  frame->width = width;
  frame->height = height;
  frame->colorspace = AVCOL_SPC_RGB;
  frame->color_range = AVCOL_RANGE_JPEG;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("av_frame_get_buffer failed");
  }
  for (int p = 0; p < 3; ++p) {
    for (int y = 0; y < height; ++y) {
      float *row = reinterpret_cast<float *>(frame->data[p] + static_cast<ptrdiff_t>(y) * frame->linesize[p]);
      for (int x = 0; x < width; ++x) {
        row[x] = value;
      }
    }
  }
  frame->opaque = stream;
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

EYEQ::ResolvedOutput BuildResolved(int width, int height) {
  EYEQ::ResolvedOutput resolved;
  resolved.device.index = 0;
  resolved.mode.width = width;
  resolved.mode.height = height;
  resolved.mode.frame_duration = 1;
  resolved.mode.time_scale = 25;
  resolved.mode.is_progressive = true;
  resolved.connection = EYEQ::VideoConnection::SDI;
  resolved.pixel_format = EYEQ::PixelFormat::R210;
  resolved.layout.width = width;
  resolved.layout.height = height;
  resolved.layout.pixel_format = EYEQ::PixelFormat::R210;
  resolved.layout.row_bytes = EYEQ::MinimalRowBytes(EYEQ::PixelFormat::R210, width);
  resolved.layout.image_bytes = static_cast<std::size_t>(resolved.layout.row_bytes) * static_cast<std::size_t>(height);
  resolved.color.encoding.primaries = EYEQ::ColorPrimaries::BT709;
  resolved.color.encoding.transfer = EYEQ::ColorTransfer::SDR_BT1886;
  resolved.color.encoding.range = EYEQ::ColorRange::Video;
  resolved.color.encoding.chroma_loc = EYEQ::ChromaLocation::Left;
  resolved.color.target_nits = 100.0;
  resolved.hdr.hdr_signaling = false;
  resolved.hdr.primaries = EYEQ::ColorPrimaries::BT709;
  resolved.hdr.transfer = EYEQ::ColorTransfer::SDR_BT1886;
  resolved.needs_vertical_flip = false;
  return resolved;
}

std::unique_ptr<EYEQ::DisplayCoordinator> MakeCoordinator(EYEQ::RenderContext &context,
                                                          const EYEQ::ResolvedOutput &resolved) {
  EYEQ::DisplayCoordinator::Config cfg;
  cfg.mode = EYEQ::DisplayMode::Fill;
  cfg.pixel_density = 1.0f;
  cfg.enable_osd = false;
  cfg.colorspace_hint = false;
  cfg.mapping_target.primaries = PL_COLOR_PRIM_BT_709;
  cfg.mapping_target.peak_nits = static_cast<float>(resolved.color.target_nits);
  cfg.mapping_target.black_nits = 0.0f;
  cfg.mapping_target.sdr_white_nits = PL_COLOR_SDR_WHITE;
  cfg.canvas_w = resolved.layout.width;
  cfg.canvas_h = resolved.layout.height;
  return std::make_unique<EYEQ::DisplayCoordinator>(context, cfg, nullptr);
}

EYEQ::MonitorOutput::Request MakeRequest(EYEQ::DisplayCoordinator &coordinator,
                                         const std::map<int, std::shared_ptr<AVFrame>> &frames,
                                         const EYEQ::DisplayState &state, int main_id, uint64_t submit_id) {
  coordinator.SetMainSource(main_id);
  coordinator.FeedFrames(frames);
  EYEQ::MonitorOutput::Request request;
  request.frame = coordinator.RenderScene(state);
  request.info.submit_id = submit_id;
  return request;
}

MonitorOutput::StepResult Drive(MonitorOutput &monitor, int max_pumps = 50000) {
  for (int i = 0; i < max_pumps; ++i) {
    const MonitorOutput::StepResult step = monitor.Pump();
    if (step.status != MonitorOutput::Status::Pending) {
      return step;
    }
  }
  throw std::runtime_error("monitor did not finish within the pump bound");
}

} // namespace

int main() {
  g_failures = 0;
  g_checks = 0;

  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    pl_gpu gpu = context.gpu();
    (void)gpu;

    AVFormatContext *format = avformat_alloc_context();
    if (!format) {
      throw std::runtime_error("avformat_alloc_context failed");
    }
    AVStream *stream = avformat_new_stream(format, nullptr);

    const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54);
    EYEQ::test::FakeFrameSink fake(1);
    eyeq_test::ScriptedFrameSink scripted(fake);

    // Fail exactly the first readback, then forward to the real implementation.
    g_remaining_download_failures = 1;
    EYEQ::frame_packer_test::MutableHooks().tex_download = &HookDownload;

    auto coordinator = MakeCoordinator(context, resolved);
    MonitorOutput monitor(context, resolved, scripted);

    auto frame = MakeSolidFrame(stream, 64, 54, 0.3f);
    EYEQ::DisplayState request_state = EYEQ::DisplayState{};
    request_state.ids = {5};
    request_state.scale = 1.0f;
    request_state.fill.id = 5;
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 2001);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "fault request accepted");

    const auto failed = Drive(monitor);
    Check(failed.status == MonitorOutput::Status::Failed, "injected readback failure reports Failed");
    Check(failed.stage == "pack" && !failed.reason.empty(), "failure reports stage pack and a reason");
    Check(fake.still_display_count() == 0, "a failed pack is never displayed");
    Check(monitor.diagnostics().pack_failed >= 1,
          "the injected readback failure actually reached the hooked real packer");

    // The failure is published immediately, but its slot may still be draining. The
    // next explicit request must wait until AcceptsRequests() reports the resources
    // are reclaimed; the module must keep reporting Failed meanwhile.
    bool regressed = false;
    int drain_guard = 0;
    while (!monitor.AcceptsRequests() && drain_guard < 50000) {
      const auto step = monitor.Pump();
      if (step.status != MonitorOutput::Status::Failed) {
        regressed = true;
        break;
      }
      ++drain_guard;
    }
    Check(!regressed, "the failed request never regresses to Pending while draining");
    Check(monitor.AcceptsRequests(), "the failed request's resources are reclaimed before the next submit");
    Check(monitor.status() == MonitorOutput::Status::Failed, "status still reports the failure after draining");

    // A new explicit request (not an automatic restart) succeeds once the injection
    // is exhausted, proving the failure did not silently poison the module.
    auto second = MakeSolidFrame(stream, 64, 54, 0.6f);
    EYEQ::DisplayState next_state = EYEQ::DisplayState{};
    next_state.ids = {5};
    next_state.scale = 1.0f;
    next_state.fill.id = 5;
    auto next = MakeRequest(*coordinator, {{5, second}}, next_state, 5, 2002);
    Check(monitor.Submit(next) == MonitorOutput::SubmitOutcome::Accepted, "request after failure accepted");
    const auto displayed = Drive(monitor);
    Check(displayed.status == MonitorOutput::Status::Displayed, "request after failure displays");
    Check(fake.still_display_count() == 1, "the successful request displays exactly once");

    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "fault monitor closed");

    EYEQ::frame_packer_test::MutableHooks().tex_download = nullptr;

    // F1: a request that already failed but whose slot cannot be reclaimed yet must
    // publish the failure immediately, keep the slot/source alive and refuse to
    // overwrite them, instead of reporting Pending forever.
    {
      EYEQ::test::FakeFrameSink gpu_fake(1);
      eyeq_test::ScriptedFrameSink gpu_scripted(gpu_fake);
      g_remaining_download_failures = 0;
      g_gpu_lost = true;
      EYEQ::frame_packer_test::MutableHooks().gpu_is_failed = &HookGpuIsFailed;

      auto coordinator = MakeCoordinator(context, resolved);
      MonitorOutput gpu_monitor(context, resolved, gpu_scripted);
      auto gpu_frame = MakeSolidFrame(stream, 64, 54, 0.3f);
      EYEQ::DisplayState gpu_request_state = EYEQ::DisplayState{};
      gpu_request_state.ids = {5};
      gpu_request_state.scale = 1.0f;
      gpu_request_state.fill.id = 5;
      auto gpu_request = MakeRequest(*coordinator, {{5, gpu_frame}}, gpu_request_state, 5, 3001);
      Check(gpu_monitor.Submit(gpu_request) == MonitorOutput::SubmitOutcome::Accepted, "gpu-lost request accepted");

      const auto first = gpu_monitor.Pump();
      Check(first.status == MonitorOutput::Status::Failed, "gpu-lost failure is reported immediately as Failed");
      Check(first.stage == "pack" && !first.reason.empty(), "gpu-lost failure carries stage and reason");
      Check(gpu_monitor.status() == MonitorOutput::Status::Failed, "status is Failed, not Pending");
      Check(!gpu_monitor.AcceptsRequests(), "gpu-lost module refuses new requests while resources are retained");
      Check(gpu_monitor.diagnostics().pack_failed >= 1, "the failure reached the real hooked packer");

      // While the GPU is lost the failure must stay observable and never regress to
      // Pending, and a new request must be Busy rather than overwrite the slot.
      bool stayed_failed = true;
      for (int i = 0; i < 5; ++i) {
        const auto step = gpu_monitor.Pump();
        if (step.status != MonitorOutput::Status::Failed || step.stage != "pack" || step.reason.empty()) {
          stayed_failed = false;
        }
      }
      Check(stayed_failed, "failure stays Failed with a stable stage across retries");
      Check(!gpu_monitor.AcceptsRequests(), "module still refuses requests while the slot cannot be reclaimed");

      auto second_frame = MakeSolidFrame(stream, 64, 54, 0.9f);
      EYEQ::DisplayState gpu_second_state = EYEQ::DisplayState{};
      gpu_second_state.ids = {9};
      gpu_second_state.scale = 1.0f;
      gpu_second_state.fill.id = 9;
      auto gpu_second = MakeRequest(*coordinator, {{9, second_frame}}, gpu_second_state, 9, 3002);
      Check(gpu_monitor.Submit(gpu_second) == MonitorOutput::SubmitOutcome::Busy,
            "a new submit is Busy while the failed slot is retained, not accepted");
      Check(gpu_fake.still_display_count() == 0, "nothing is displayed while the failure is retained");

      // Recover the (test-only) device signal so the retained slot can drain.
      g_gpu_lost = false;
      const auto drained = Drive(gpu_monitor);
      Check(drained.status == MonitorOutput::Status::Failed, "the drained failure still reports Failed");
      Check(gpu_monitor.AcceptsRequests(), "module accepts requests after the resources are reclaimed");
      Check(gpu_monitor.Submit(gpu_second) == MonitorOutput::SubmitOutcome::Accepted,
            "an explicit request after the drain is accepted");
      Check(Drive(gpu_monitor).status == MonitorOutput::Status::Displayed,
            "the explicit request after the drain displays");
      Check(gpu_fake.still_display_count() == 1, "the recovered request displayed exactly once");
      Check(gpu_monitor.Close(2000) == EYEQ::CloseStatus::Closed, "gpu-lost monitor closed");

      EYEQ::frame_packer_test::MutableHooks().gpu_is_failed = nullptr;
      g_gpu_lost = false;
    }

    // F3: an idle DeviceLost event must publish a stable terminal reason instead of
    // leaving status()/last_result() on Idle with an empty stage.
    {
      EYEQ::test::FakeFrameSink event_fake(1);
      eyeq_test::ScriptedFrameSink event_scripted(event_fake);
      EYEQ::OutputEvent device_lost{};
      device_lost.kind = EYEQ::OutputEvent::Kind::DeviceLost;
      std::snprintf(device_lost.message, sizeof(device_lost.message), "%s", "scripted device lost");
      event_scripted.ScriptEvent(device_lost);

      auto coordinator = MakeCoordinator(context, resolved);
      MonitorOutput event_monitor(context, resolved, event_scripted);
      const auto step = event_monitor.Pump();
      Check(step.status == MonitorOutput::Status::Failed && step.stage == "event",
            "idle DeviceLost event publishes Failed at stage event");
      Check(!step.reason.empty(), "idle DeviceLost failure carries a reason");
      Check(event_monitor.status() == MonitorOutput::Status::Failed, "status reflects the idle device event");
      Check(event_monitor.last_result().status == MonitorOutput::Status::Failed &&
                event_monitor.last_result().stage == "event",
            "last_result is updated for an idle device event");
      Check(!event_monitor.AcceptsRequests(), "an idle terminal event stops accepting requests");

      auto event_frame = MakeSolidFrame(stream, 64, 54, 0.3f);
      EYEQ::DisplayState event_request_state = EYEQ::DisplayState{};
      event_request_state.ids = {5};
      event_request_state.scale = 1.0f;
      event_request_state.fill.id = 5;
      auto event_request = MakeRequest(*coordinator, {{5, event_frame}}, event_request_state, 5, 3101);
      Check(event_monitor.Submit(event_request) == MonitorOutput::SubmitOutcome::Failed,
            "submit after an idle DeviceLost reports Failed");
      Check(event_fake.still_display_count() == 0, "an idle terminal event never displays");
      event_monitor.Close(2000);
    }

    // F3: a terminal event arriving after a completed request must overwrite the
    // previous Displayed result instead of leaving it in place.
    {
      EYEQ::test::FakeFrameSink post_fake(1);
      eyeq_test::ScriptedFrameSink post_scripted(post_fake);
      auto coordinator = MakeCoordinator(context, resolved);
      MonitorOutput post_monitor(context, resolved, post_scripted);
      auto post_frame = MakeSolidFrame(stream, 64, 54, 0.5f);
      EYEQ::DisplayState post_request_state = EYEQ::DisplayState{};
      post_request_state.ids = {5};
      post_request_state.scale = 1.0f;
      post_request_state.fill.id = 5;
      auto post_request = MakeRequest(*coordinator, {{5, post_frame}}, post_request_state, 5, 3201);
      Check(post_monitor.Submit(post_request) == MonitorOutput::SubmitOutcome::Accepted,
            "post-completion request accepted");
      Check(Drive(post_monitor).status == MonitorOutput::Status::Displayed, "post-completion request displayed");
      Check(post_monitor.last_result().status == MonitorOutput::Status::Displayed,
            "post-completion result starts as Displayed");

      EYEQ::OutputEvent post_lost{};
      post_lost.kind = EYEQ::OutputEvent::Kind::DeviceLost;
      std::snprintf(post_lost.message, sizeof(post_lost.message), "%s", "scripted post-completion device lost");
      post_scripted.ScriptEvent(post_lost);

      const auto step = post_monitor.Pump();
      Check(step.status == MonitorOutput::Status::Failed && step.stage == "event",
            "post-completion DeviceLost publishes Failed at stage event");
      Check(post_monitor.status() == MonitorOutput::Status::Failed, "post-completion event updates status");
      Check(post_monitor.last_result().status == MonitorOutput::Status::Failed &&
                post_monitor.last_result().stage == "event",
            "post-completion event overwrites the previous Displayed last_result");
      Check(!post_monitor.AcceptsRequests(), "post-completion terminal event stops accepting requests");
      post_monitor.Close(2000);
    }

    avformat_free_context(format);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  std::fprintf(stdout, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
