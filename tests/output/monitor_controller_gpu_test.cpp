// MonitorController integration tests: the real DisplayCoordinator (single
// FrameRenderer) + the real MonitorOutput -> FramePacker -> FrameSink chain,
// driven through the same Tick() discipline main uses. It proves the borrow
// pairing, the desktop/monitor shared result, resize/OSD byte stability, scene
// resubmission and the Busy/cancel/failure/drain/close paths on the new interface.
//
// Headless Vulkan device; the DeckLink SDK and a real window are never used.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/display_coordinator.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"
#include "fake_frame_sink.hpp"
#include "output/monitor_controller.hpp"
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

using Color = std::array<float, 3>;
using PixelFunc = std::function<Color(int, int)>;

struct SourceOwner {
  AVFormatContext *format = nullptr;
  AVStream *stream = nullptr;
  SourceOwner() {
    format = avformat_alloc_context();
    if (!format) {
      throw std::runtime_error("avformat_alloc_context failed");
    }
    stream = avformat_new_stream(format, nullptr);
    if (!stream) {
      avformat_free_context(format);
      format = nullptr;
      throw std::runtime_error("avformat_new_stream failed");
    }
  }
  ~SourceOwner() {
    if (format) {
      avformat_free_context(format);
    }
  }
  SourceOwner(const SourceOwner &) = delete;
  SourceOwner &operator=(const SourceOwner &) = delete;
};

std::shared_ptr<AVFrame> MakeFrame(AVStream *stream, int width, int height, const PixelFunc &pixel) {
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
  for (int y = 0; y < height; ++y) {
    float *g = reinterpret_cast<float *>(frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0]);
    float *b = reinterpret_cast<float *>(frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1]);
    float *r = reinterpret_cast<float *>(frame->data[2] + static_cast<ptrdiff_t>(y) * frame->linesize[2]);
    for (int x = 0; x < width; ++x) {
      const Color c = pixel(x, y);
      g[x] = c[1];
      b[x] = c[2];
      r[x] = c[0];
    }
  }
  frame->opaque = stream;
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

EYEQ::ResolvedOutput BuildResolved(int width, int height, EYEQ::PixelFormat format, bool hdr, double target_nits) {
  EYEQ::ResolvedOutput resolved;
  resolved.device.index = 0;
  resolved.mode.width = width;
  resolved.mode.height = height;
  resolved.mode.frame_duration = 1;
  resolved.mode.time_scale = 25;
  resolved.mode.is_progressive = true;
  resolved.connection = EYEQ::VideoConnection::SDI;
  resolved.pixel_format = format;
  resolved.layout.width = width;
  resolved.layout.height = height;
  resolved.layout.pixel_format = format;
  resolved.layout.row_bytes = EYEQ::MinimalRowBytes(format, width);
  resolved.layout.image_bytes = static_cast<std::size_t>(resolved.layout.row_bytes) * static_cast<std::size_t>(height);
  resolved.color.encoding.primaries = hdr ? EYEQ::ColorPrimaries::BT2020 : EYEQ::ColorPrimaries::BT709;
  resolved.color.encoding.transfer = hdr ? EYEQ::ColorTransfer::PQ : EYEQ::ColorTransfer::SDR_BT1886;
  resolved.color.encoding.range = EYEQ::ColorRange::Video;
  resolved.color.encoding.chroma_loc = EYEQ::ChromaLocation::Left;
  resolved.color.target_nits = target_nits;
  resolved.hdr.hdr_signaling = hdr;
  resolved.hdr.primaries = resolved.color.encoding.primaries;
  resolved.hdr.transfer = resolved.color.encoding.transfer;
  resolved.needs_vertical_flip = false;
  return resolved;
}

struct OffscreenTarget {
  pl_tex tex = nullptr;
  struct pl_frame frame{};
};

OffscreenTarget CreateTarget(pl_gpu gpu, int width, int height, enum pl_color_primaries primaries,
                             enum pl_color_transfer transfer) {
  pl_fmt format = pl_find_fmt(
      gpu, PL_FMT_FLOAT, 4, 32, 32,
      static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
  if (!format) {
    throw std::runtime_error("no rgba32f texture format available");
  }
  struct pl_tex_params params = {
      .w = width, .h = height, .format = format, .sampleable = true, .renderable = true, .host_readable = true};
  OffscreenTarget target;
  target.tex = pl_tex_create(gpu, &params);
  if (!target.tex) {
    throw std::runtime_error("offscreen target creation failed");
  }
  target.frame.num_planes = 1;
  target.frame.planes[0].texture = target.tex;
  target.frame.planes[0].components = 4;
  target.frame.planes[0].component_mapping[0] = 0;
  target.frame.planes[0].component_mapping[1] = 1;
  target.frame.planes[0].component_mapping[2] = 2;
  target.frame.planes[0].component_mapping[3] = 3;
  target.frame.repr = pl_color_repr_rgb;
  target.frame.repr.sys = PL_COLOR_SYSTEM_RGB;
  target.frame.repr.levels = PL_COLOR_LEVELS_FULL;
  target.frame.color.primaries = primaries;
  target.frame.color.transfer = transfer;
  return target;
}

EYEQ::DisplayState MakeState(std::vector<int> ids, int main_id) {
  EYEQ::DisplayState state{};
  state.ids = ids;
  state.scale = 1.0f;
  state.offset_x = 0.0f;
  state.offset_y = 0.0f;
  state.fill.id = main_id;
  state.slide.split_pos = 0.5f;
  state.slide.left_id = ids.empty() ? main_id : ids[0];
  state.slide.right_id = ids.size() > 1 ? ids[1] : (ids.empty() ? main_id : ids[0]);
  state.grid.cols = static_cast<int>(ids.size());
  state.grid.rows = 1;
  return state;
}

std::unique_ptr<EYEQ::DisplayCoordinator> MakeCoordinator(EYEQ::RenderContext &context, EYEQ::DisplayMode mode,
                                                          const EYEQ::ResolvedOutput &resolved) {
  EYEQ::DisplayCoordinator::Config cfg;
  cfg.mode = mode;
  cfg.scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.plane_scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.pixel_density = 1.0f;
  cfg.enable_osd = false;
  cfg.colorspace_hint = false;
  cfg.present_dither = false;
  cfg.work_precision = EYEQ::WorkPrecision::F16;
  cfg.mapping_target.primaries = (resolved.color.encoding.primaries == EYEQ::ColorPrimaries::BT2020)
                                     ? PL_COLOR_PRIM_BT_2020
                                     : PL_COLOR_PRIM_BT_709;
  cfg.mapping_target.peak_nits = static_cast<float>(resolved.color.target_nits);
  cfg.mapping_target.black_nits = 0.0f;
  cfg.mapping_target.sdr_white_nits = PL_COLOR_SDR_WHITE;
  cfg.canvas_w = resolved.layout.width;
  cfg.canvas_h = resolved.layout.height;
  return std::make_unique<EYEQ::DisplayCoordinator>(context, cfg, nullptr);
}

Color Ramp(int x, int y, int w) {
  const float v = (x + y) / static_cast<float>(2 * w);
  return {v, 1.0f - v, 0.5f * v};
}

void Feed(EYEQ::DisplayCoordinator &coordinator, AVStream *stream, int w, int h) {
  std::map<int, std::shared_ptr<AVFrame>> frames;
  frames[0] = MakeFrame(stream, w, h, [w](int x, int y) { return Ramp(x, y, w); });
  coordinator.FeedFrames(frames);
}

int DriveController(EYEQ::MonitorController &controller, int max_ticks = 100000) {
  for (int i = 0; i < max_ticks; ++i) {
    controller.Tick();
    if (!controller.in_flight()) {
      return i + 1;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Desktop and monitor consume the same composed result; the packed bytes are
// stable across a pure-UI re-present and change with a real scene change.
// ---------------------------------------------------------------------------
void TestSharedResultAndResubmit(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[shared result, resize/OSD stability, resubmission]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
  EYEQ::test::FakeFrameSink *fake_ptr = fake.get();
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  auto controller = std::make_unique<EYEQ::MonitorController>(context, *coordinator, std::move(scripted), resolved);

  EYEQ::DisplayState state = MakeState({0}, 0);
  Feed(*coordinator, stream, w, h);
  auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  coordinator->PresentTo(desktop.frame, state);
  const pl_tex shared = coordinator->result().texture;
  const uint64_t first_render = coordinator->result().identity.render_id;

  Check(DriveController(*controller) > 0, "the first monitor request completes");
  Check(controller->diagnostics().displayed == 1, "exactly one still frame was displayed");
  Check(controller->submit_count() == 1, "exactly one submit was issued");
  const std::vector<uint8_t> *bytes_after_first = fake_ptr->copy(1);
  Check(bytes_after_first != nullptr && !bytes_after_first->empty(), "the sink captured the packed bytes");
  const std::vector<uint8_t> first_bytes = bytes_after_first != nullptr ? *bytes_after_first : std::vector<uint8_t>{};
  Check(!controller->result_borrowed(), "the borrow is released after the request terminates");
  Check(coordinator->result().texture == shared, "the monitor consumed the same common result texture");
  Check(!controller->in_flight(), "no request is in flight afterwards");

  // Pure-UI re-present (resize/OSD): the scene key is unchanged, so the render id
  // is stable and the controller must not resubmit or change the packed bytes.
  coordinator->PresentTo(desktop.frame, state);
  Check(coordinator->result().identity.render_id == first_render, "a pure-UI re-present keeps the render identity");
  controller->Tick();
  Check(controller->submit_count() == 1, "a resize/OSD re-present does not resubmit");
  const std::vector<uint8_t> *bytes_after_ui = fake_ptr->copy(1);
  Check(bytes_after_ui != nullptr && *bytes_after_ui == first_bytes,
        "the packed bytes are unchanged after a resize/OSD");

  // A real scene change recomposes and resubmits.
  state.scale = 1.5f;
  state.offset_x = 6.0f;
  state.window_attached = false;
  coordinator->SetMainSource(0);
  coordinator->PresentTo(desktop.frame, state);
  Check(coordinator->result().identity.render_id != first_render, "a scene change produces a new render identity");
  Check(DriveController(*controller) > 0, "the scene-change request completes");
  Check(controller->submit_count() == 2, "a scene change resubmits exactly once");
  Check(controller->diagnostics().displayed == 2, "the second scene is displayed");
  const std::vector<uint8_t> *bytes_after_change = fake_ptr->copy(2);
  Check(bytes_after_change != nullptr && *bytes_after_change != first_bytes, "the new scene changes the packed bytes");

  controller->Close(1000);
  Check(!controller->result_borrowed(), "a clean close releases the borrow");
  controller->Tick();
  Check(controller->submit_count() == 2, "Tick after close does nothing");

  pl_tex_destroy(context.gpu(), &desktop.tex);
}

// ---------------------------------------------------------------------------
// Borrow discipline: while a request is in flight the coordinator rejects a real
// recomposition and mapping/canvas changes, and defers a precision change.
// ---------------------------------------------------------------------------
void TestBorrowRejectsRecompose(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[borrow rejects recompose and defers precision]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  // Keep the request in flight for a few pumps, then let the wrapped sink display.
  scripted->ScriptStill(EYEQ::StillOutcome::Busy, 3);
  auto controller = std::make_unique<EYEQ::MonitorController>(context, *coordinator, std::move(scripted), resolved);

  EYEQ::DisplayState state = MakeState({0}, 0);
  Feed(*coordinator, stream, w, h);
  auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  coordinator->PresentTo(desktop.frame, state);

  controller->Tick(); // submits and stays in flight
  Check(controller->in_flight(), "the request is in flight");
  Check(controller->result_borrowed(), "the common result is borrowed while in flight");

  EYEQ::DisplayState changed = state;
  changed.scale = 1.7f;
  changed.offset_x = 3.0f;
  changed.window_attached = false;
  Check(coordinator->WouldRecompose(changed), "the coordinator reports the changed scene would recompose");

  bool threw = false;
  try {
    coordinator->RenderScene(changed);
  } catch (const std::logic_error &) {
    threw = true;
  }
  Check(threw, "RenderScene rejects a recomposition while the result is borrowed");
  Check(coordinator->recompose_rejected_count() == 1, "the rejected recomposition is counted");

  bool set_threw = false;
  try {
    coordinator->SetMappingTarget(coordinator->mapping_target());
  } catch (const std::logic_error &) {
    set_threw = true;
  }
  Check(set_threw, "SetMappingTarget is rejected while the result is borrowed");
  bool canvas_threw = false;
  try {
    coordinator->SetCanvasSize(w + 1, h);
  } catch (const std::logic_error &) {
    canvas_threw = true;
  }
  Check(canvas_threw, "SetCanvasSize is rejected while the result is borrowed");

  Check(!coordinator->ReconfigurePrecision(EYEQ::WorkPrecision::F32), "a precision change while borrowed is deferred");
  Check(coordinator->has_pending_precision(), "the precision change is pending");

  Check(DriveController(*controller) > 0, "the request drains");
  Check(!controller->result_borrowed(), "the borrow is released after draining");
  // The deferred precision change is applied on the next composition.
  coordinator->RenderScene(changed);
  Check(!coordinator->has_pending_precision(), "the pending precision change was applied after release");
  Check(coordinator->work_precision() == EYEQ::WorkPrecision::F32, "the precision is now F32");

  controller->Close(1000);
  pl_tex_destroy(context.gpu(), &desktop.tex);
}

// ---------------------------------------------------------------------------
// Cancel, sink failure and close through the controller.
// ---------------------------------------------------------------------------
void TestCancelFailureClose(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[cancel, sink failure, close]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0);

  {
    // Cancel: the request never reaches DisplayStill.
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
    EYEQ::test::FakeFrameSink *fake_ptr = fake.get();
    auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
    scripted->ScriptStill(EYEQ::StillOutcome::Busy, 5);
    auto controller = std::make_unique<EYEQ::MonitorController>(context, *coordinator, std::move(scripted), resolved);
    EYEQ::DisplayState state = MakeState({0}, 0);
    Feed(*coordinator, stream, w, h);
    auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    coordinator->PresentTo(desktop.frame, state);
    controller->Tick();
    Check(controller->in_flight(), "cancel case: the request is in flight");
    controller->Cancel();
    Check(DriveController(*controller) > 0, "cancel case: the request drains");
    Check(controller->status() == EYEQ::MonitorOutput::Status::Cancelled, "cancel case: the request is cancelled");
    Check(!controller->result_borrowed(), "cancel case: the borrow is released");
    Check(fake_ptr->still_display_count() == 0, "cancel case: DisplayStill was never reached");
    controller->Close(1000);
    pl_tex_destroy(context.gpu(), &desktop.tex);
  }

  {
    // Sink failure at DisplayStill: terminal, observable, never auto-restarted.
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
    auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
    scripted->ScriptStill(EYEQ::StillOutcome::DeviceError, 1);
    auto controller = std::make_unique<EYEQ::MonitorController>(context, *coordinator, std::move(scripted), resolved);
    EYEQ::DisplayState state = MakeState({0}, 0);
    Feed(*coordinator, stream, w, h);
    auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    coordinator->PresentTo(desktop.frame, state);
    Check(DriveController(*controller) > 0, "sink failure case: the request drains");
    Check(controller->status() == EYEQ::MonitorOutput::Status::Failed, "sink failure case: the failure is published");
    Check(controller->failed(), "sink failure case: the controller stops scheduling");
    Check(!controller->result_borrowed(), "sink failure case: the borrow is released");
    const uint64_t submits = controller->submit_count();
    controller->Tick();
    controller->Tick();
    Check(controller->submit_count() == submits, "sink failure case: no automatic restart");
    controller->Close(1000);
    pl_tex_destroy(context.gpu(), &desktop.tex);
  }

  {
    // Close deadline: a still-busy sink keeps the borrow and reports the timeout.
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
    auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
    scripted->ScriptStill(EYEQ::StillOutcome::Busy, 5);
    auto controller = std::make_unique<EYEQ::MonitorController>(context, *coordinator, std::move(scripted), resolved);
    EYEQ::DisplayState state = MakeState({0}, 0);
    Feed(*coordinator, stream, w, h);
    auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    coordinator->PresentTo(desktop.frame, state);
    controller->Tick();
    Check(controller->in_flight(), "close case: the request is in flight");
    // The still-busy sink still owns the request's staging slot, so the packer
    // cannot drain within the deadline and the borrow must be retained.
    const EYEQ::CloseStatus status = controller->Close(0);
    Check(status == EYEQ::CloseStatus::DrainTimeout, "close case: the deadline is reported as DrainTimeout");
    Check(controller->result_borrowed(), "close case: the borrow is retained after a drain timeout");
    pl_tex_destroy(context.gpu(), &desktop.tex);
  }
}

// ---------------------------------------------------------------------------
// Constructor rollback: an invalid resolved output must not close the borrowed
// sink and must acquire nothing that survives.
// ---------------------------------------------------------------------------
void TestInitFailureRollback(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[constructor rollback]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0);
  resolved.layout.width = w + 1; // inconsistent with the resolved mode
  auto coordinator =
      MakeCoordinator(context, EYEQ::DisplayMode::Fill, BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0));
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
  EYEQ::test::FakeFrameSink *fake_ptr = fake.get();
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  bool threw = false;
  try {
    EYEQ::MonitorController controller(context, *coordinator, std::move(scripted), resolved);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  Check(threw, "an invalid resolved output is rejected at construction");
  Check(fake_ptr->state() == EYEQ::test::FakeFrameSink::State::Running,
        "the construction rollback did not close the borrowed sink");
}

} // namespace

int main() {
  g_failures = 0;
  g_checks = 0;

  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    SourceOwner owner;

    TestSharedResultAndResubmit(context, owner.stream);
    TestBorrowRejectsRecompose(context, owner.stream);
    TestCancelFailureClose(context, owner.stream);
    TestInitFailureRollback(context);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  std::fprintf(stdout, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
