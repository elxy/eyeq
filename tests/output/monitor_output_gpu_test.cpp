// Monitor composition-layer tests. They drive the real MonitorOutput ->
// FrameRenderer -> FramePacker -> FrameSink::DisplayStill chain on a headless
// Vulkan device with a scripted fake sink. No DeckLink SDK, no window, no device.
//
// Packed bytes are checked against the independent CPU packing oracle using the
// float RGB values actually read back from the monitor's fixed target, and the
// target's known colour/luminance anchors are checked separately so the assertions
// are not a self-comparison of the same output path.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/display_coordinator.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"
#include "display/rendered_frame.hpp"
#include "fake_frame_sink.hpp"
#include "output/monitor_output.hpp"
#include "output/output_profile.hpp"
#include "pack_compare.hpp"
#include "scripted_frame_sink.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;
int g_skipped = 0;

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
using EYEQ::MonitorOutput;

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

// Full-range GBRPF32LE software frame (planes G/B/R).
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

// A frame the GPU mapper cannot consume: a valid AVFrame with a pixel format that
// libplacebo does not map, which is what FeedFrames reports per-video. (Using
// AV_PIX_FMT_NONE is not safe: pl_frame_from_avframe dereferences a null descriptor.)
std::shared_ptr<AVFrame> MakeBrokenFrame(AVStream *stream, int width, int height) {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("av_frame_alloc failed");
  }
  frame->format = AV_PIX_FMT_PAL8;
  frame->width = width;
  frame->height = height;
  frame->opaque = stream;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("av_frame_get_buffer failed");
  }
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

Color QuadrantPixel(int x, int y, int width, int height) {
  const bool right = x >= width / 2;
  const bool bottom = y >= height / 2;
  if (!bottom && !right) {
    return {1.0f, 0.0f, 0.0f};
  }
  if (!bottom && right) {
    return {0.0f, 1.0f, 0.0f};
  }
  if (bottom && !right) {
    return {0.0f, 0.0f, 1.0f};
  }
  return {1.0f, 1.0f, 1.0f};
}

// Publish a known source mastering peak so a PQ source/target pair has a
// well-defined transform instead of relying on a default nominal peak.
void SetSourceMasteringPeak(AVFrame *frame, double max_nits) {
  AVFrameSideData *side_data =
      av_frame_new_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sizeof(AVMasteringDisplayMetadata));
  if (!side_data || !side_data->data) {
    throw std::runtime_error("av_frame_new_side_data(MASTERING) failed");
  }
  auto *mdm = reinterpret_cast<AVMasteringDisplayMetadata *>(side_data->data);
  std::memset(mdm, 0, sizeof(*mdm));
  mdm->has_luminance = 1;
  mdm->max_luminance = av_d2q(max_nits, 1000000);
  mdm->min_luminance = av_d2q(0.0, 1000000);
}

float PqCodeForNits(float nits) {
  const float m1 = 0.1593017578125f;
  const float m2 = 78.84375f;
  const float c1 = 0.8359375f;
  const float c2 = 18.8515625f;
  const float c3 = 18.6875f;
  const float y = nits / 10000.0f;
  const float ym = std::pow(y, m1);
  return std::pow((c1 + c2 * ym) / (1.0f + c3 * ym), m2);
}

EYEQ::ResolvedOutput BuildResolved(int width, int height, EYEQ::PixelFormat format, bool hdr, double target_nits,
                                   int extra_row_bytes) {
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
  resolved.layout.row_bytes = EYEQ::MinimalRowBytes(format, width) + extra_row_bytes;
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

// Builds the coordination layer that owns the single FrameRenderer. The monitor
// output no longer renders: it consumes the common result this produces. The
// mapping target mirrors the resolved output so the transfer-only encode is
// valid (primaries must match).
std::unique_ptr<EYEQ::DisplayCoordinator> MakeCoordinator(EYEQ::RenderContext &context, EYEQ::DisplayMode mode,
                                                          const EYEQ::ResolvedOutput &resolved) {
  EYEQ::DisplayCoordinator::Config cfg;
  cfg.mode = mode;
  cfg.scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.plane_scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.pixel_density = 1.0f;
  cfg.enable_osd = false;
  cfg.colorspace_hint = false;
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

// Feeds the scene through the coordinator and wraps the composed result into a
// monitor request. The returned frame is borrowed from the coordinator and stays
// valid while the coordinator is not composed again.
EYEQ::MonitorOutput::Request MakeRequest(EYEQ::DisplayCoordinator &coordinator,
                                         const std::map<int, std::shared_ptr<AVFrame>> &frames,
                                         const EYEQ::DisplayState &state, int main_id, uint64_t submit_id,
                                         uint64_t serial = 0) {
  coordinator.SetMainSource(main_id);
  coordinator.FeedFrames(frames);
  EYEQ::MonitorOutput::Request request;
  request.frame = coordinator.RenderScene(state);
  request.info.submit_id = submit_id;
  request.info.source_frame_serial = serial;
  return request;
}

std::vector<float> DownloadTargetRgb(pl_gpu gpu, MonitorOutput &monitor) {
  const struct pl_frame &frame = monitor.target_frame();
  const int w = frame.planes[0].texture->params.w;
  const int h = frame.planes[0].texture->params.h;
  std::vector<float> rgba(static_cast<std::size_t>(w) * h * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = frame.planes[0].texture,
      .row_pitch = static_cast<std::size_t>(w) * 4 * sizeof(float),
      .ptr = rgba.data(),
  };
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("monitor target download failed");
  }
  std::vector<float> rgb(static_cast<std::size_t>(w) * h * 3);
  for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  return rgb;
}

Color AtRgb(const std::vector<float> &rgb, int width, int x, int y) {
  const float *p = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
  return {p[0], p[1], p[2]};
}

bool Close(const Color &a, const Color &b, float tolerance) {
  if (!std::isfinite(tolerance) || tolerance < 0.0f) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > tolerance) {
      return false;
    }
  }
  return true;
}

std::string ColorString(const Color &c) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "(%.4f, %.4f, %.4f)", c[0], c[1], c[2]);
  return buf;
}

// Pump() is deliberately non-blocking, so a tight spin can exhaust a test's
// iteration bound before the GPU readback completes. Yield between polls.
void YieldToGpu() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

MonitorOutput::StepResult Drive(MonitorOutput &monitor, int max_pumps = 50000) {
  for (int i = 0; i < max_pumps; ++i) {
    const MonitorOutput::StepResult step = monitor.Pump();
    if (step.outcome != MonitorOutput::Outcome::None) {
      return step;
    }
    YieldToGpu();
  }
  throw std::runtime_error("monitor did not finish within the pump bound");
}

struct SinkPair {
  EYEQ::test::FakeFrameSink fake{1};
  std::unique_ptr<eyeq_test::ScriptedFrameSink> owned = std::make_unique<eyeq_test::ScriptedFrameSink>(fake);
  // Observation handle for the scripted wrapper; stays valid while the
  // MonitorOutput that took ownership of @p owned is alive.
  eyeq_test::ScriptedFrameSink *scripted = owned.get();
  // Hands the owned sink to MonitorOutput; @p fake and @p scripted stay valid for
  // inspection while the MonitorOutput owning it is alive.
  std::unique_ptr<EYEQ::FrameSink> take() { return std::move(owned); }
};

void CheckPackedAgainstOracle(SinkPair &sinks, MonitorOutput &monitor, pl_gpu gpu, const EYEQ::ResolvedOutput &resolved,
                              uint64_t submit_id) {
  const std::vector<uint8_t> *bytes = sinks.fake.copy(submit_id);
  Check(bytes != nullptr, "sink stored a copy for submit " + std::to_string(submit_id));
  if (bytes == nullptr) {
    return;
  }
  Check(bytes->size() == resolved.layout.image_bytes, "packed byte count matches image_bytes");
  const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
  const auto stats =
      eyeq_test::CompareFrame(bytes->data(), resolved.layout.width, resolved.layout.height, resolved.layout.row_bytes,
                              rgb, resolved.layout.pixel_format, resolved.color.encoding.primaries);
  Check(stats.mismatch == 0, "bytes match the independent CPU packing oracle (mismatches=" +
                                 std::to_string(stats.mismatch) + ", exact=" + std::to_string(stats.exact) + ")");
  Check(stats.exact > 0, "oracle comparison actually compared real fields");
}

void TestSdrR210(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const EYEQ::ResolvedOutput resolved = BuildResolved(96, 64, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  {
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 96, 64, [](int x, int y) { return QuadrantPixel(x, y, 96, 64); });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 101, 5001);

    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "SDR r210 request accepted");
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Displayed, "SDR r210 request displayed");
    Check(sinks.fake.still_display_count() == 1, "sink displayed exactly once");
    Check(monitor.diagnostics().render_count == 1 && monitor.diagnostics().pack_submit_count == 1,
          "one render and one packer submit");
    Check(sinks.scripted->last_info().submit_id == 101, "SubmissionInfo identity preserved");

    const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
    Check(Close(AtRgb(rgb, 96, 24, 16), {1.0f, 0.0f, 0.0f}, 0.02f),
          "SDR target top-left is red, got " + ColorString(AtRgb(rgb, 96, 24, 16)));
    Check(Close(AtRgb(rgb, 96, 72, 16), {0.0f, 1.0f, 0.0f}, 0.02f), "SDR target top-right is green");
    Check(Close(AtRgb(rgb, 96, 24, 48), {0.0f, 0.0f, 1.0f}, 0.02f), "SDR target bottom-left is blue");
    Check(Close(AtRgb(rgb, 96, 72, 48), {1.0f, 1.0f, 1.0f}, 0.02f), "SDR target bottom-right is white");

    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 101);
    Check(frame->data[0] != nullptr && frame->format == AV_PIX_FMT_GBRPF32LE, "source AVFrame is unchanged");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "SDR r210 closed");
  }
}

void TestPqV210(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const EYEQ::ResolvedOutput resolved = BuildResolved(96, 54, EYEQ::PixelFormat::V210, true, 1000.0, 16);
  const float pq100 = PqCodeForNits(100.0f);
  SinkPair sinks;
  {
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 96, 54, [&](int, int) { return Color{pq100, pq100, pq100}; });
    frame->color_primaries = AVCOL_PRI_BT2020;
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    SetSourceMasteringPeak(frame.get(), 1000.0);

    EYEQ::DisplayState request_state = MakeState({9}, 9);
    auto request = MakeRequest(*coordinator, {{9, frame}}, request_state, 9, 202, 0);

    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "PQ v210 request accepted");
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Displayed, "PQ v210 request displayed");

    const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
    const Color center = AtRgb(rgb, 96, 48, 27);
    Check(Close(center, Color{pq100, pq100, pq100}, 0.02f),
          "PQ 100-nit code survives to the PQ target, got " + ColorString(center));
    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 202);
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "PQ v210 closed");
  }
}

void TestSlideAndAmplify(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const EYEQ::ResolvedOutput resolved = BuildResolved(96, 48, EYEQ::PixelFormat::R210, false, 203.0, 0);
  {
    SinkPair sinks;
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Slide, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto left = MakeFrame(stream, 96, 48, [](int, int) { return Color{1.0f, 0.0f, 0.0f}; });
    auto right = MakeFrame(stream, 96, 48, [](int, int) { return Color{0.0f, 0.0f, 1.0f}; });
    EYEQ::DisplayState request_state = MakeState({5, 9}, 5);
    auto request = MakeRequest(*coordinator, {{5, left}, {9, right}}, request_state, 5, 303, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "Slide request accepted");
    Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "Slide request displayed");
    const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
    Check(Close(AtRgb(rgb, 96, 24, 24), {1.0f, 0.0f, 0.0f}, 0.03f), "Slide left half is red");
    Check(Close(AtRgb(rgb, 96, 72, 24), {0.0f, 0.0f, 1.0f}, 0.03f), "Slide right half is blue");
    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 303);
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Slide closed");
  }
  {
    SinkPair sinks;
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto dis = MakeFrame(stream, 96, 48, [](int, int) { return Color{0.4f, 0.4f, 0.4f}; });
    auto ref = MakeFrame(stream, 96, 48, [](int, int) { return Color{0.2f, 0.2f, 0.2f}; });
    EYEQ::DisplayState request_state = MakeState({5, 9}, 5);
    request_state.ref_id = 9;
    request_state.amplify = 2.0f;
    auto request = MakeRequest(*coordinator, {{5, dis}, {9, ref}}, request_state, 5, 304, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "amplify request accepted");
    Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "amplify request displayed");
    const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
    // Amplification is defined in the display-linear domain (V2.1 D1). For the
    // BT.1886 codes 0.4/0.2 at gain 2 the measured linear value is
    // 0.0215 + 2*(0.1117-0.0215) = 0.2020, which the SDR transfer encodes to
    // ~0.5135. The old gamma-domain 0.6 is deliberately not reproduced.
    const Color center = AtRgb(rgb, 96, 48, 24);
    Check(Close(center, {0.5135f, 0.5135f, 0.5135f}, 0.02f),
          "amplified target center is the display-linear ref+(dis-ref)*amp result, got " + ColorString(center));
    Check(!Close(center, {0.6f, 0.6f, 0.6f}, 0.02f), "the gamma-domain amplification result is not produced");
    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 304);
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "amplify closed");
  }
}

void TestGrid(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const EYEQ::ResolvedOutput resolved = BuildResolved(96, 48, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Grid, resolved);
  MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
  auto red = MakeFrame(stream, 48, 48, [](int, int) { return Color{1.0f, 0.0f, 0.0f}; });
  auto blue = MakeFrame(stream, 48, 48, [](int, int) { return Color{0.0f, 0.0f, 1.0f}; });
  EYEQ::DisplayState request_state = MakeState({5, 9}, 5);
  request_state.grid.cols = 2;
  request_state.grid.rows = 1;
  auto request = MakeRequest(*coordinator, {{5, red}, {9, blue}}, request_state, 5, 405, 0);
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "Grid request accepted");
  Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "Grid request displayed");
  const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
  Check(Close(AtRgb(rgb, 96, 24, 24), {1.0f, 0.0f, 0.0f}, 0.03f), "Grid first cell is red");
  Check(Close(AtRgb(rgb, 96, 72, 24), {0.0f, 0.0f, 1.0f}, 0.03f), "Grid second cell is blue");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 405);
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Grid closed");
}

void TestStillBusy(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 64, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  sinks.scripted->ScriptStill(EYEQ::StillOutcome::Busy, 3);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
  auto frame = MakeFrame(stream, 64, 64, [](int x, int y) { return QuadrantPixel(x, y, 64, 64); });
  EYEQ::DisplayState request_state = MakeState({5}, 5);
  auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 506, 77);
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "Busy test request accepted");

  // A second request while the first is in flight must be Busy and must not touch
  // the new input. It deliberately reuses the first result instead of asking the
  // coordinator to compose again: recomposing would recycle the very texture the
  // in-flight request is still packing (the borrow contract).
  MonitorOutput::Request second;
  second.frame = request.frame;
  second.info.submit_id = 507;
  const uint64_t second_render_id = second.frame.identity.render_id;
  Check(monitor.Submit(second) == MonitorOutput::SubmitOutcome::Busy, "second submit while in flight is Busy");
  Check(second.frame.identity.render_id == second_render_id && second.frame.texture != nullptr,
        "Busy submit does not consume or modify the new result");
  Check(monitor.diagnostics().render_count == 1 && monitor.diagnostics().pack_submit_count == 1,
        "Busy submit did not render or pack again");

  // Pump until the three scripted Busy outcomes have been consumed; the earliest
  // Pumps may only observe the packer as NotReady.
  int guard = 0;
  while (monitor.diagnostics().still_busy < 3 && guard < 10000) {
    const auto step = monitor.Pump();
    ++guard;
    if (step.outcome != MonitorOutput::Outcome::None) {
      break;
    }
    YieldToGpu();
  }
  Check(sinks.fake.still_display_count() == 0, "sink consumed nothing while Busy");
  Check(monitor.diagnostics().still_busy == 3, "three Still Busy outcomes observed");
  Check(sinks.scripted->still_calls() == 3, "three DisplayStill attempts while Busy");
  Check(sinks.scripted->still_views_consistent(), "the same bytes were offered on every Busy retry");
  Check(sinks.scripted->last_info().submit_id == 506, "the same SubmissionInfo was retried");

  const auto step = Drive(monitor);
  Check(step.outcome == MonitorOutput::Outcome::Displayed, "request displays after the sink becomes free");
  Check(sinks.fake.still_display_count() == 1 && monitor.diagnostics().displayed == 1, "exactly one display");
  Check(monitor.diagnostics().pack_submit_count == 1, "only one packer submit for the whole request");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 506);

  // The next request is accepted and reuses the packer ring; the first sink copy
  // must remain its own independent snapshot.
  const std::vector<uint8_t> first_copy = *sinks.fake.copy(506);
  auto other = MakeFrame(stream, 64, 64, [](int, int) { return Color{0.1f, 0.2f, 0.3f}; });
  EYEQ::DisplayState third_state = MakeState({9}, 9);
  auto third = MakeRequest(*coordinator, {{9, other}}, third_state, 9, 508, 0);
  Check(monitor.Submit(third) == MonitorOutput::SubmitOutcome::Accepted, "next request accepted after completion");
  Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "next request displayed");
  Check(sinks.fake.copy(506) != nullptr && *sinks.fake.copy(506) == first_copy,
        "the earlier sink copy is unchanged after reuse");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Busy test closed");
}

void TestCancel(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  {
    // Cancel while the packer is still working.
    SinkPair sinks;
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 601, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "cancel test request accepted");
    monitor.Cancel();
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Cancelled, "pending request becomes Cancelled");
    Check(sinks.fake.still_display_count() == 0, "a cancelled request is never displayed");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "cancel test closed");
  }
  {
    // Cancel while the sink is Busy: the Ready slot must be reclaimed without any
    // further DisplayStill call.
    SinkPair sinks;
    sinks.scripted->ScriptStill(EYEQ::StillOutcome::Busy, 100);
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 602, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "cancel-busy request accepted");
    // Pump until the sink has been offered the bytes (scripted Busy). Pump() is
    // non-blocking, so yield to the GPU between polls; a tight spin can exhaust
    // its bound before the readback completes.
    int guard = 0;
    while (sinks.scripted->still_calls() == 0 && guard < 20000) {
      const auto step = monitor.Pump();
      ++guard;
      if (step.outcome != MonitorOutput::Outcome::None) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(sinks.scripted->still_calls() >= 1, "the sink was offered the bytes while Busy");
    const int calls_before_cancel = sinks.scripted->still_calls();
    monitor.Cancel();
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Cancelled, "Busy request cancels logically");
    Check(sinks.scripted->still_calls() == calls_before_cancel, "cancel does not call DisplayStill again");
    Check(sinks.fake.still_display_count() == 0, "cancelled Busy request was never displayed");
    // The module is reusable after a logical cancel.
    EYEQ::DisplayState next_state = MakeState({5}, 5);
    auto next = MakeRequest(*coordinator, {{5, frame}}, next_state, 5, 603, 0);
    Check(monitor.Submit(next) == MonitorOutput::SubmitOutcome::Accepted, "request accepted after cancel");
    Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "request displayed after cancel");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "cancel-busy test closed");
  }
}

void TestSinkFailures(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  {
    SinkPair sinks;
    sinks.scripted->ScriptStill(EYEQ::StillOutcome::DeviceError, 1);
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 701, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "device-error request accepted");
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Failed && step.stage == "still",
          "sink DeviceError surfaces as Failed at stage still");
    Check(!step.reason.empty(), "failure carries a reason");
    Check(sinks.fake.still_display_count() == 0, "DeviceError never reports Displayed");
    Check(!monitor.AcceptsRequests(), "a terminal sink failure stops accepting requests");
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Failed, "further submit reports Failed");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "device-error path still closes");
  }
  {
    SinkPair sinks;
    sinks.scripted->ScriptStill(EYEQ::StillOutcome::Closed, 1);
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 702, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "sink-closed request accepted");
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Failed, "sink Closed surfaces as Failed, not Displayed");
    Check(sinks.fake.still_display_count() == 0, "Closed sink consumes nothing");
    monitor.Close(2000);
  }
  {
    // A queued Error event must be handled before any DisplayStill attempt.
    SinkPair sinks;
    EYEQ::OutputEvent event{};
    event.kind = EYEQ::OutputEvent::Kind::Error;
    event.stage = EYEQ::OutputEvent::Stage::DisplayStill;
    std::snprintf(event.message, sizeof(event.message), "%s", "scripted sink error");
    sinks.scripted->ScriptEvent(event);
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 703, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "error-event request accepted");
    const auto step = Drive(monitor);
    Check(step.outcome == MonitorOutput::Outcome::Failed && step.stage == "event",
          "queued Error event fails the request before any display");
    Check(sinks.fake.still_display_count() == 0, "queued Error event prevents any display");
    monitor.Close(2000);
  }
}

void TestClose(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  {
    // Close with a still-pending request: cancel, drain and close.
    SinkPair sinks;
    sinks.scripted->ScriptStill(EYEQ::StillOutcome::Busy, 100);
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 801, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "close-pending request accepted");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Close reclaims the Ready/Busy slot and closes");
    Check(sinks.fake.still_display_count() == 0, "close-time cancel never displayed");
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Closed, "submit after close is rejected");
  }
  {
    // Close timeout then retry: the first Close reports DrainTimeout because the
    // scripted sink refuses to close.
    SinkPair sinks;
    sinks.scripted->ScriptClose(EYEQ::CloseStatus::DrainTimeout, 1);
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    EYEQ::DisplayState request_state = MakeState({5}, 5);
    auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 802, 0);
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "close-timeout request accepted");
    Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "close-timeout request displayed");
    Check(monitor.Close(1000) == EYEQ::CloseStatus::DrainTimeout, "first Close reports DrainTimeout");
    Check(!monitor.close_reason().empty(), "DrainTimeout carries a reason");
    Check(monitor.Close(1000) == EYEQ::CloseStatus::Closed, "second Close completes");
    Check(monitor.Close(1000) == EYEQ::CloseStatus::Closed, "repeated Close stays Closed");
  }
}

void TestIsolation(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  // A second, independent consumer (its own coordinator/renderer) with a
  // different target must not affect the monitor output. This only shows resource
  // isolation, not a UI viewport.
  EYEQ::DisplayCoordinator::Config desktop_config;
  desktop_config.mode = EYEQ::DisplayMode::Fill;
  desktop_config.pixel_density = 1.0f;
  desktop_config.enable_osd = false;
  desktop_config.mapping_target.primaries = PL_COLOR_PRIM_BT_709;
  desktop_config.mapping_target.peak_nits = 203.0f;
  desktop_config.mapping_target.black_nits = 0.0f;
  desktop_config.mapping_target.sdr_white_nits = PL_COLOR_SDR_WHITE;
  desktop_config.canvas_w = 32;
  desktop_config.canvas_h = 32;
  EYEQ::DisplayCoordinator desktop(context, desktop_config);
  auto desktop_source = MakeFrame(stream, 32, 32, [](int, int) { return Color{0.7f, 0.2f, 0.1f}; });
  std::map<int, std::shared_ptr<AVFrame>> desktop_frames;
  desktop_frames[1] = desktop_source;
  desktop.SetMainSource(1);
  Check(desktop.FeedFrames(desktop_frames).empty(), "desktop consumer accepted its own frame");
  pl_fmt desktop_format = pl_find_named_fmt(gpu, "rgba32f");
  struct pl_tex_params desktop_params = {
      .w = 32,
      .h = 32,
      .format = desktop_format,
      .sampleable = true,
      .renderable = true,
  };
  pl_tex desktop_tex = pl_tex_create(gpu, &desktop_params);
  Check(desktop_tex != nullptr, "desktop renderer target created");
  struct pl_frame desktop_target{};
  desktop_target.num_planes = 1;
  desktop_target.planes[0].texture = desktop_tex;
  desktop_target.planes[0].components = 4;
  desktop_target.planes[0].component_mapping[0] = 0;
  desktop_target.planes[0].component_mapping[1] = 1;
  desktop_target.planes[0].component_mapping[2] = 2;
  desktop_target.planes[0].component_mapping[3] = 3;
  desktop_target.repr = pl_color_repr_rgb;
  desktop_target.color.primaries = PL_COLOR_PRIM_BT_709;
  desktop_target.color.transfer = PL_COLOR_TRC_BT_1886;
  EYEQ::DisplayState desktop_state = MakeState({1}, 1);
  desktop.RenderScene(desktop_state);
  desktop.PresentTo(desktop_target, desktop_state);

  const EYEQ::ResolvedOutput resolved = BuildResolved(96, 64, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
  auto frame = MakeFrame(stream, 96, 64, [](int x, int y) { return QuadrantPixel(x, y, 96, 64); });
  EYEQ::DisplayState request_state = MakeState({5}, 5);
  auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 1001, 0);
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "isolated monitor request accepted");
  Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "isolated monitor request displayed");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 1001);
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "isolation monitor closed");
  pl_tex_destroy(gpu, &desktop_tex);
}

void TestUhdOnce(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const int width = 3840;
  const int height = 2160;
  const EYEQ::ResolvedOutput resolved = BuildResolved(width, height, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);
  auto frame = MakeFrame(stream, width, height, [](int x, int y) { return QuadrantPixel(x, y, 3840, 2160); });
  EYEQ::DisplayState request_state = MakeState({5}, 5);
  auto request = MakeRequest(*coordinator, {{5, frame}}, request_state, 5, 1101, 0);
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "UHD request accepted");
  Check(Drive(monitor).outcome == MonitorOutput::Outcome::Displayed, "UHD request displayed");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 1101);
  Check(monitor.Close(5000) == EYEQ::CloseStatus::Closed, "UHD monitor closed");
}

// ---------------------------------------------------------------------------
// Merged MonitorController coverage: MonitorOutput now owns the sink and drives
// the coordinator through Tick(), so these integration tests live here.
// ---------------------------------------------------------------------------

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

Color Ramp(int x, int y, int w) {
  const float v = (x + y) / static_cast<float>(2 * w);
  return {v, 1.0f - v, 0.5f * v};
}

void Feed(EYEQ::DisplayCoordinator &coordinator, AVStream *stream, int w, int h) {
  std::map<int, std::shared_ptr<AVFrame>> frames;
  frames[0] = MakeFrame(stream, w, h, [w](int x, int y) { return Ramp(x, y, w); });
  coordinator.FeedFrames(frames);
}

int DriveController(MonitorOutput &controller, int max_ticks = 100000) {
  for (int i = 0; i < max_ticks; ++i) {
    controller.Tick();
    if (!controller.in_flight()) {
      return i + 1;
    }
    YieldToGpu();
  }
  return -1;
}

// The GPU-side finite gate replaces the full-frame readback on the playback/monitor
// path: it detects NaN/Inf (hard error) while only reading back one flag word. The
// full-frame readback counter must stay at zero throughout.
void TestGpuFiniteGate(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[GPU finite gate]\n");
  pl_gpu gpu = context.gpu();
  const auto caps =
      static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
  pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
  if (format == nullptr) {
    throw std::runtime_error("no rgba32f host-writable format for the finite-gate test");
  }
  struct pl_tex_params params = {.w = 4, .h = 1, .format = format, .sampleable = true, .host_writable = true};
  pl_tex tex = pl_tex_create(gpu, &params);
  if (tex == nullptr) {
    throw std::runtime_error("finite-gate probe texture creation failed");
  }

  auto upload = [&](const std::vector<float> &rgba) {
    struct pl_tex_transfer_params transfer = {
        .tex = tex,
        .row_pitch = 4 * 4 * sizeof(float),
        .ptr = const_cast<float *>(rgba.data()),
    };
    if (!pl_tex_upload(gpu, &transfer)) {
      throw std::runtime_error("finite-gate probe upload failed");
    }
  };

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  EYEQ::ResetLinearTextureReadbackCount();
  {
    EYEQ::FiniteGate gate(context.log(), gpu);
    std::vector<float> finite(16, 0.25f);
    upload(finite);
    bool ok = true;
    try {
      gate.Validate(tex);
    } catch (const std::exception &) {
      ok = false;
    }
    Check(ok, "the GPU gate accepts a finite texture");

    for (float poison : {nan, inf, -inf}) {
      std::vector<float> bad(16, 0.25f);
      bad[4] = poison;
      upload(bad);
      bool threw = false;
      try {
        gate.Validate(tex);
      } catch (const std::runtime_error &) {
        threw = true;
      }
      Check(threw, std::isnan(poison) ? "the GPU gate rejects NaN" : "the GPU gate rejects Inf");
      upload(finite); // restore for the next iteration
    }
  }
  Check(EYEQ::LinearTextureReadbackCount() == 0, "the GPU gate never triggers a full-frame readback");
  pl_tex_destroy(gpu, &tex);
}

// ---------------------------------------------------------------------------
// Paced playback (continuous monitor output) tests.
// ---------------------------------------------------------------------------

bool DriveTicks(MonitorOutput &monitor, const std::function<bool()> &done, int max_ticks = 200000) {
  for (int i = 0; i < max_ticks; ++i) {
    monitor.Tick();
    if (done()) {
      return true;
    }
    YieldToGpu();
  }
  return false;
}

// Composes a solid-color scene (the feed generation changes, so every call produces a
// new render identity).
void ComposeSolid(EYEQ::DisplayCoordinator &coordinator, AVStream *stream, int w, int h, const Color &c) {
  auto frame = MakeFrame(stream, w, h, [c](int, int) { return c; });
  coordinator.SetMainSource(0);
  coordinator.FeedFrames({{0, frame}});
  EYEQ::DisplayState state = MakeState({0}, 0);
  coordinator.RenderScene(state);
}

// Continuous paced submissions must reach the sink in composition order, at strictly
// increasing display ticks, with bytes that match the independent CPU packing oracle
// of the frame's own target, and without any full-frame readback on the way.
void TestPacedPlaybackOrderAndBytes(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[paced playback: order, display ticks, bytes vs oracle]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(8);
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  eyeq_test::ScriptedFrameSink *sink = scripted.get();
  MonitorOutput monitor(context, *coordinator, std::move(scripted), resolved);

  EYEQ::ResetLinearTextureReadbackCount();
  monitor.SetPlayback(true);

  const int kFrames = 5;
  std::vector<std::vector<float>> frame_rgb;
  std::vector<std::vector<uint8_t>> frame_bytes;
  for (int i = 0; i < kFrames; ++i) {
    ComposeSolid(*coordinator, stream, w, h, Color{0.05f + 0.15f * i, 0.1f + 0.1f * i, 0.2f});
    const std::size_t schedules_before = sink->schedule_calls().size();
    monitor.Tick(); // enqueue the new composition
    const bool ok = DriveTicks(monitor, [&]() { return sink->schedule_calls().size() > schedules_before; });
    Check(ok, "paced frame " + std::to_string(i) + " is scheduled");
    frame_bytes.push_back(sink->schedule_calls().back().bytes);
    // The target still holds this frame: no later composition ran yet.
    frame_rgb.push_back(DownloadTargetRgb(context.gpu(), monitor));
  }

  Check(sink->schedule_calls().size() == static_cast<std::size_t>(kFrames), "every paced frame reached the sink");
  bool order_ok = true;
  bool ticks_ok = true;
  for (std::size_t i = 0; i < sink->schedule_calls().size(); ++i) {
    const auto &call = sink->schedule_calls()[i];
    if (call.info.submit_id != static_cast<uint64_t>(i + 1)) {
      order_ok = false;
    }
    if (call.display_duration != resolved.mode.frame_duration) {
      order_ok = false;
    }
    if (i > 0 && call.display_time <= sink->schedule_calls()[i - 1].display_time) {
      ticks_ok = false;
    }
  }
  Check(order_ok, "paced frames are scheduled in composition order with the mode frame duration");
  Check(ticks_ok, "display ticks strictly increase");
  for (int i = 0; i < kFrames; ++i) {
    const auto stats = eyeq_test::CompareFrame(frame_bytes[static_cast<std::size_t>(i)].data(), w, h,
                                               resolved.layout.row_bytes, frame_rgb[static_cast<std::size_t>(i)],
                                               resolved.layout.pixel_format, resolved.color.encoding.primaries);
    Check(stats.mismatch == 0, "paced frame " + std::to_string(i) + " bytes match the CPU oracle");
  }
  Check(monitor.diagnostics().paced_submits == static_cast<uint64_t>(kFrames), "every composition was paced-submitted");
  Check(monitor.diagnostics().finite_checks == static_cast<uint64_t>(kFrames),
        "the GPU finite gate ran on every paced frame");
  Check(EYEQ::LinearTextureReadbackCount() == 0, "paced playback never does a full-frame readback");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "paced monitor closed");
}

// When the sink cannot schedule (Busy), the bounded queue overflows by dropping the
// oldest unscheduled frame; the newest frames survive and still reach the sink.
void TestPacedOverflowDropsOldest(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[paced playback: queue overflow drops the oldest unscheduled frame]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(8);
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  eyeq_test::ScriptedFrameSink *sink = scripted.get();
  MonitorOutput monitor(context, *coordinator, std::move(scripted), resolved);

  sink->ScriptSchedule(EYEQ::ScheduleOutcome::Busy, 1000);
  monitor.SetPlayback(true);

  const int kFrames = 6; // more than the ring capacity
  for (int i = 0; i < kFrames; ++i) {
    ComposeSolid(*coordinator, stream, w, h, Color{0.05f + 0.12f * i, 0.3f, 0.1f});
    monitor.Tick();
    YieldToGpu();
  }
  const std::size_t capacity = static_cast<std::size_t>(EYEQ::FramePacker::kRingCapacity);
  Check(monitor.paced_pending() == capacity, "the paced queue is bounded by the packer ring");
  Check(monitor.diagnostics().paced_dropped == static_cast<uint64_t>(kFrames) - capacity,
        "overflow dropped exactly the oldest unscheduled frames");
  Check(monitor.diagnostics().paced_submits == static_cast<uint64_t>(kFrames),
        "every composition was enqueued before the drops");

  const std::vector<float> newest_rgb = DownloadTargetRgb(context.gpu(), monitor);
  sink->ClearScheduleScript();
  const bool drained = DriveTicks(monitor, [&]() { return monitor.paced_pending() == 0; });
  Check(drained, "the retained paced frames drain once the sink accepts");
  Check(monitor.diagnostics().paced_scheduled == static_cast<uint64_t>(capacity),
        "exactly the retained frames reached the sink");
  const auto stats =
      eyeq_test::CompareFrame(sink->schedule_calls().back().bytes.data(), w, h, resolved.layout.row_bytes, newest_rgb,
                              resolved.layout.pixel_format, resolved.color.encoding.primaries);
  Check(stats.mismatch == 0, "the newest frame reaches the sink (and matches its oracle)");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "overflow monitor closed");
}

// Pause stops the scheduled session and freezes the last frame as a still; new scenes
// shown while paused stay stills; resuming jumps to the latest composed frame.
void TestPacedPauseResume(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[paced playback: pause freezes, resume jumps to the latest]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(8);
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  eyeq_test::ScriptedFrameSink *sink = scripted.get();
  MonitorOutput monitor(context, *coordinator, std::move(scripted), resolved);

  EYEQ::ResetLinearTextureReadbackCount();
  monitor.SetPlayback(true);
  ComposeSolid(*coordinator, stream, w, h, Color{0.9f, 0.1f, 0.1f});
  monitor.Tick();
  Check(DriveTicks(monitor, [&]() { return sink->schedule_calls().size() >= 1; }), "the first paced frame schedules");
  const std::size_t schedules_after_first = sink->schedule_calls().size();

  // Pause: the session stops and the last frame freezes as a still.
  monitor.SetPlayback(false);
  const bool stopped = DriveTicks(
      monitor, [&]() { return !monitor.playback_started() && !monitor.in_flight() && sink->still_calls() >= 1; });
  Check(stopped, "pause stops the scheduled session and freezes a still");
  Check(sink->schedule_calls().size() == schedules_after_first, "pause does not schedule new frames");

  // New scenes while paused are stills, never paced schedules.
  for (int i = 0; i < 2; ++i) {
    ComposeSolid(*coordinator, stream, w, h, Color{0.1f, 0.8f - 0.1f * i, 0.2f});
    monitor.Tick();
    Check(DriveTicks(monitor, [&]() { return !monitor.in_flight(); }), "paused still completes");
  }
  Check(sink->schedule_calls().size() == schedules_after_first, "no paced scheduling while paused");
  Check(sink->still_calls() >= 2, "paused scenes are shown as stills");

  // Latest composition before resume.
  ComposeSolid(*coordinator, stream, w, h, Color{0.2f, 0.2f, 0.9f});
  monitor.Tick();
  Check(DriveTicks(monitor, [&]() { return !monitor.in_flight(); }), "the latest paused still completes");
  const std::vector<float> latest_rgb = DownloadTargetRgb(context.gpu(), monitor);

  const std::size_t schedules_before_resume = sink->schedule_calls().size();
  monitor.SetPlayback(true);
  const bool resumed = DriveTicks(monitor, [&]() { return sink->schedule_calls().size() > schedules_before_resume; });
  Check(resumed, "playback resumes and schedules a frame");
  const auto stats =
      eyeq_test::CompareFrame(sink->schedule_calls().back().bytes.data(), w, h, resolved.layout.row_bytes, latest_rgb,
                              resolved.layout.pixel_format, resolved.color.encoding.primaries);
  Check(stats.mismatch == 0, "resume jumps to the latest composed frame");
  Check(EYEQ::LinearTextureReadbackCount() == 0, "pause/resume never does a full-frame readback");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "pause/resume monitor closed");
}

// Resuming playback right after a pause must reclaim the freeze still's packer
// slot: without the reclaim the still's undrained readback holds one ring slot for
// the whole playback session and the paced queue caps at ring-1.
void TestPacedResumeReclaimsStillSlot(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[paced playback: resume reclaims the freeze still's slot]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(8);
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  eyeq_test::ScriptedFrameSink *sink = scripted.get();
  MonitorOutput monitor(context, *coordinator, std::move(scripted), resolved);

  monitor.SetPlayback(true);
  ComposeSolid(*coordinator, stream, w, h, Color{0.9f, 0.1f, 0.1f});
  monitor.Tick();
  Check(DriveTicks(monitor, [&]() { return sink->schedule_calls().size() >= 1; }), "the first paced frame schedules");

  // Pause: the first tick issues StopScheduled, the second confirms it and
  // submits the freeze still. Do NOT drain the still's readback past this point.
  monitor.SetPlayback(false);
  monitor.Tick();
  monitor.Tick();
  Check(monitor.in_flight(), "the freeze still is in flight with its pack slot pending");

  // Resume immediately, then saturate the paced queue against a busy sink.
  monitor.SetPlayback(true);
  sink->ScriptSchedule(EYEQ::ScheduleOutcome::Busy, 1000);
  const int kFrames = 8; // comfortably more than the ring capacity
  for (int i = 0; i < kFrames; ++i) {
    ComposeSolid(*coordinator, stream, w, h, Color{0.05f + 0.12f * i, 0.3f, 0.1f});
    monitor.Tick();
    YieldToGpu();
  }
  const std::size_t capacity = static_cast<std::size_t>(EYEQ::FramePacker::kRingCapacity);
  Check(monitor.paced_pending() == capacity, "resume reclaimed the still slot: the paced queue reaches the full ring");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "reclaim monitor closed");
}

void TestSharedResultAndResubmit(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[shared result, resize/OSD stability, resubmission]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
  EYEQ::test::FakeFrameSink *fake_ptr = fake.get();
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  auto controller = std::make_unique<MonitorOutput>(context, *coordinator, std::move(scripted), resolved);

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
  Check(coordinator->result().texture == shared, "the monitor consumed the same common result texture");
  Check(!controller->in_flight(), "no request is in flight afterwards");

  // Pure-UI re-present (resize/OSD): the scene key is unchanged, so the render id
  // is stable and Tick() must not resubmit or change the packed bytes.
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
  controller->Tick();
  Check(controller->submit_count() == 2, "Tick after close does nothing");

  pl_tex_destroy(context.gpu(), &desktop.tex);
}

void TestRecomposeWhileInFlight(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[recompose while a monitor request is in flight]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  // Keep the request in flight for a few pumps, then let the wrapped sink display.
  scripted->ScriptStill(EYEQ::StillOutcome::Busy, 3);
  auto controller = std::make_unique<MonitorOutput>(context, *coordinator, std::move(scripted), resolved);

  EYEQ::DisplayState state = MakeState({0}, 0);
  Feed(*coordinator, stream, w, h);
  auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  coordinator->PresentTo(desktop.frame, state);
  const uint64_t first_render = coordinator->result().identity.render_id;

  controller->Tick(); // submits and stays in flight
  Check(controller->in_flight(), "the request is in flight");

  // Without a borrow protocol the coordinator must accept a real recomposition
  // while the request is still in flight; the encode already copied the result
  // into the monitor's own target (queue_order_gpu sentinel test).
  EYEQ::DisplayState changed = state;
  changed.scale = 1.7f;
  changed.offset_x = 3.0f;
  changed.window_attached = false;
  coordinator->RenderScene(changed);
  Check(coordinator->result().identity.render_id != first_render,
        "a recomposition while a request is in flight is accepted");

  // The in-flight request still terminates, then the new scene is submitted.
  Check(DriveController(*controller) > 0, "the in-flight request drains");
  Check(DriveController(*controller) > 0, "the recomposed scene is submitted and drains");
  Check(controller->submit_count() == 2, "the recomposed scene was resubmitted");
  Check(controller->diagnostics().displayed == 2, "both requests were displayed");

  controller->Close(1000);
  pl_tex_destroy(context.gpu(), &desktop.tex);
}

void TestCancelFailureClose(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[cancel, sink failure, close]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);

  {
    // Cancel: the request never reaches DisplayStill.
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
    auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
    EYEQ::test::FakeFrameSink *fake_ptr = fake.get();
    auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
    scripted->ScriptStill(EYEQ::StillOutcome::Busy, 5);
    auto controller = std::make_unique<MonitorOutput>(context, *coordinator, std::move(scripted), resolved);
    EYEQ::DisplayState state = MakeState({0}, 0);
    Feed(*coordinator, stream, w, h);
    auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    coordinator->PresentTo(desktop.frame, state);
    controller->Tick();
    Check(controller->in_flight(), "cancel case: the request is in flight");
    controller->Cancel();
    Check(DriveController(*controller) > 0, "cancel case: the request drains");
    Check(controller->last_result().outcome == MonitorOutput::Outcome::Cancelled,
          "cancel case: the request is cancelled");
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
    auto controller = std::make_unique<MonitorOutput>(context, *coordinator, std::move(scripted), resolved);
    EYEQ::DisplayState state = MakeState({0}, 0);
    Feed(*coordinator, stream, w, h);
    auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    coordinator->PresentTo(desktop.frame, state);
    Check(DriveController(*controller) > 0, "sink failure case: the request drains");
    Check(controller->last_result().outcome == MonitorOutput::Outcome::Failed,
          "sink failure case: the failure is published");
    Check(controller->failed(), "sink failure case: the controller stops scheduling");
    Check(controller->terminal_failed(), "sink failure case: the failure is terminal");
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
    auto controller = std::make_unique<MonitorOutput>(context, *coordinator, std::move(scripted), resolved);
    EYEQ::DisplayState state = MakeState({0}, 0);
    Feed(*coordinator, stream, w, h);
    auto desktop = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
    coordinator->PresentTo(desktop.frame, state);
    controller->Tick();
    Check(controller->in_flight(), "close case: the request is in flight");
    // The still-busy sink still owns the request's staging slot, so the packer
    // cannot drain within the deadline.
    const EYEQ::CloseStatus status = controller->Close(0);
    Check(status == EYEQ::CloseStatus::DrainTimeout, "close case: the deadline is reported as DrainTimeout");
    pl_tex_destroy(context.gpu(), &desktop.tex);
  }
}

void TestInitFailureRollback(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[constructor rollback]\n");
  const int w = 64;
  const int h = 48;
  auto resolved = BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0);
  resolved.layout.width = w + 1; // inconsistent with the resolved mode
  auto coordinator =
      MakeCoordinator(context, EYEQ::DisplayMode::Fill, BuildResolved(w, h, EYEQ::PixelFormat::V210, false, 100.0, 0));
  auto fake = std::make_unique<EYEQ::test::FakeFrameSink>(2);
  EYEQ::test::FakeFrameSink *fake_ptr = fake.get();
  auto scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(*fake);
  bool threw = false;
  try {
    MonitorOutput controller(context, *coordinator, std::move(scripted), resolved);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  Check(threw, "an invalid resolved output is rejected at construction");
  Check(fake_ptr->state() == EYEQ::test::FakeFrameSink::State::Running,
        "the construction rollback did not close the wrapped sink");
}

} // namespace

int main() {
  g_failures = 0;
  g_checks = 0;
  g_skipped = 0;

  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    pl_gpu gpu = context.gpu();
    SourceOwner owner;

    TestSdrR210(gpu, context, owner.stream);
    TestPqV210(gpu, context, owner.stream);
    TestSlideAndAmplify(gpu, context, owner.stream);
    TestGrid(gpu, context, owner.stream);
    TestStillBusy(gpu, context, owner.stream);
    TestCancel(gpu, context, owner.stream);
    TestSinkFailures(gpu, context, owner.stream);
    TestClose(gpu, context, owner.stream);
    TestIsolation(gpu, context, owner.stream);
    TestUhdOnce(gpu, context, owner.stream);
    TestGpuFiniteGate(context);
    TestPacedPlaybackOrderAndBytes(context, owner.stream);
    TestPacedOverflowDropsOldest(context, owner.stream);
    TestPacedPauseResume(context, owner.stream);
    TestPacedResumeReclaimsStillSlot(context, owner.stream);
    TestSharedResultAndResubmit(context, owner.stream);
    TestRecomposeWhileInFlight(context, owner.stream);
    TestCancelFailureClose(context, owner.stream);
    TestInitFailureRollback(context);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
