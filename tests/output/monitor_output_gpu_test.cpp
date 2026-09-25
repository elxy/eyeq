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

#include "display/render.hpp"
#include "display/render_context.hpp"
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

MonitorOutput::StepResult Drive(MonitorOutput &monitor, int max_pumps = 50000) {
  for (int i = 0; i < max_pumps; ++i) {
    const MonitorOutput::StepResult step = monitor.Pump();
    if (step.status != MonitorOutput::Status::Pending) {
      return step;
    }
  }
  throw std::runtime_error("monitor did not finish within the pump bound");
}

struct SinkPair {
  EYEQ::test::FakeFrameSink fake{1};
  eyeq_test::ScriptedFrameSink scripted{fake};
  EYEQ::FrameSink &sink() { return scripted; }
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
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 96, 64, [](int x, int y) { return QuadrantPixel(x, y, 96, 64); });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 101;
    request.info.source_frame_serial = 5001;

    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "SDR r210 request accepted");
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Displayed, "SDR r210 request displayed");
    Check(sinks.fake.still_display_count() == 1, "sink displayed exactly once");
    Check(monitor.diagnostics().render_count == 1 && monitor.diagnostics().pack_submit_count == 1,
          "one render and one packer submit");
    Check(sinks.scripted.last_info().submit_id == 101, "SubmissionInfo identity preserved");

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
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 96, 54, [&](int, int) { return Color{pq100, pq100, pq100}; });
    frame->color_primaries = AVCOL_PRI_BT2020;
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    SetSourceMasteringPeak(frame.get(), 1000.0);

    MonitorOutput::Request request;
    request.frames[9] = frame;
    request.main_source_id = 9;
    request.state = MakeState({9}, 9);
    request.info.submit_id = 202;

    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "PQ v210 request accepted");
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Displayed, "PQ v210 request displayed");

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
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Slide, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto left = MakeFrame(stream, 96, 48, [](int, int) { return Color{1.0f, 0.0f, 0.0f}; });
    auto right = MakeFrame(stream, 96, 48, [](int, int) { return Color{0.0f, 0.0f, 1.0f}; });
    MonitorOutput::Request request;
    request.frames[5] = left;
    request.frames[9] = right;
    request.main_source_id = 5;
    request.state = MakeState({5, 9}, 5);
    request.info.submit_id = 303;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "Slide request accepted");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "Slide request displayed");
    const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
    Check(Close(AtRgb(rgb, 96, 24, 24), {1.0f, 0.0f, 0.0f}, 0.03f), "Slide left half is red");
    Check(Close(AtRgb(rgb, 96, 72, 24), {0.0f, 0.0f, 1.0f}, 0.03f), "Slide right half is blue");
    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 303);
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Slide closed");
  }
  {
    SinkPair sinks;
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto dis = MakeFrame(stream, 96, 48, [](int, int) { return Color{0.4f, 0.4f, 0.4f}; });
    auto ref = MakeFrame(stream, 96, 48, [](int, int) { return Color{0.2f, 0.2f, 0.2f}; });
    MonitorOutput::Request request;
    request.frames[5] = dis;
    request.frames[9] = ref;
    request.main_source_id = 5;
    request.state = MakeState({5, 9}, 5);
    request.state.ref_id = 9;
    request.state.amplify = 2.0f;
    request.info.submit_id = 304;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "amplify request accepted");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "amplify request displayed");
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
  MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Grid, EYEQ::ScaleMethod::Bicubic,
                        EYEQ::ScaleMethod::Bicubic);
  auto red = MakeFrame(stream, 48, 48, [](int, int) { return Color{1.0f, 0.0f, 0.0f}; });
  auto blue = MakeFrame(stream, 48, 48, [](int, int) { return Color{0.0f, 0.0f, 1.0f}; });
  MonitorOutput::Request request;
  request.frames[5] = red;
  request.frames[9] = blue;
  request.main_source_id = 5;
  request.state = MakeState({5, 9}, 5);
  request.state.grid.cols = 2;
  request.state.grid.rows = 1;
  request.info.submit_id = 405;
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "Grid request accepted");
  Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "Grid request displayed");
  const std::vector<float> rgb = DownloadTargetRgb(gpu, monitor);
  Check(Close(AtRgb(rgb, 96, 24, 24), {1.0f, 0.0f, 0.0f}, 0.03f), "Grid first cell is red");
  Check(Close(AtRgb(rgb, 96, 72, 24), {0.0f, 0.0f, 1.0f}, 0.03f), "Grid second cell is blue");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 405);
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Grid closed");
}

void TestStillBusy(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 64, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  sinks.scripted.ScriptStill(EYEQ::StillOutcome::Busy, 3);
  MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                        EYEQ::ScaleMethod::Bicubic);
  auto frame = MakeFrame(stream, 64, 64, [](int x, int y) { return QuadrantPixel(x, y, 64, 64); });
  MonitorOutput::Request request;
  request.frames[5] = frame;
  request.main_source_id = 5;
  request.state = MakeState({5}, 5);
  request.info.submit_id = 506;
  request.info.source_frame_serial = 77;
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "Busy test request accepted");

  // A second request while the first is in flight must be Busy and must not touch
  // the new input.
  auto other = MakeFrame(stream, 64, 64, [](int, int) { return Color{0.1f, 0.2f, 0.3f}; });
  AVFrame *other_raw = other.get();
  uint8_t *other_data = other_raw->data[0];
  const int other_format = other_raw->format;
  MonitorOutput::Request second;
  second.frames[9] = other;
  second.main_source_id = 9;
  second.state = MakeState({9}, 9);
  second.info.submit_id = 507;
  Check(monitor.Submit(second) == MonitorOutput::SubmitOutcome::Busy, "second submit while in flight is Busy");
  Check(other_raw->data[0] == other_data && other_raw->format == other_format,
        "Busy submit does not modify the new input");
  Check(monitor.diagnostics().render_count == 1 && monitor.diagnostics().pack_submit_count == 1,
        "Busy submit did not render or pack again");

  // Pump until the three scripted Busy outcomes have been consumed; the earliest
  // Pumps may only observe the packer as NotReady.
  int guard = 0;
  while (monitor.diagnostics().still_busy < 3 && guard < 10000) {
    const auto step = monitor.Pump();
    ++guard;
    if (step.status != MonitorOutput::Status::Pending) {
      break;
    }
  }
  Check(sinks.fake.still_display_count() == 0, "sink consumed nothing while Busy");
  Check(monitor.diagnostics().still_busy == 3, "three Still Busy outcomes observed");
  Check(sinks.scripted.still_calls() == 3, "three DisplayStill attempts while Busy");
  Check(sinks.scripted.still_views_consistent(), "the same bytes were offered on every Busy retry");
  Check(sinks.scripted.last_info().submit_id == 506, "the same SubmissionInfo was retried");

  const auto step = Drive(monitor);
  Check(step.status == MonitorOutput::Status::Displayed, "request displays after the sink becomes free");
  Check(sinks.fake.still_display_count() == 1 && monitor.diagnostics().displayed == 1, "exactly one display");
  Check(monitor.diagnostics().pack_submit_count == 1, "only one packer submit for the whole request");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 506);

  // The next request is accepted and reuses the packer ring; the first sink copy
  // must remain its own independent snapshot.
  const std::vector<uint8_t> first_copy = *sinks.fake.copy(506);
  MonitorOutput::Request third;
  third.frames[9] = other;
  third.main_source_id = 9;
  third.state = MakeState({9}, 9);
  third.info.submit_id = 508;
  Check(monitor.Submit(third) == MonitorOutput::SubmitOutcome::Accepted, "next request accepted after completion");
  Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "next request displayed");
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
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 601;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "cancel test request accepted");
    monitor.Cancel();
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Cancelled, "pending request becomes Cancelled");
    Check(sinks.fake.still_display_count() == 0, "a cancelled request is never displayed");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "cancel test closed");
  }
  {
    // Cancel while the sink is Busy: the Ready slot must be reclaimed without any
    // further DisplayStill call.
    SinkPair sinks;
    sinks.scripted.ScriptStill(EYEQ::StillOutcome::Busy, 100);
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 602;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "cancel-busy request accepted");
    // Pump until the sink has been offered the bytes (scripted Busy).
    int guard = 0;
    while (sinks.scripted.still_calls() == 0 && guard < 10000) {
      const auto step = monitor.Pump();
      ++guard;
      if (step.status != MonitorOutput::Status::Pending) {
        break;
      }
    }
    Check(sinks.scripted.still_calls() >= 1, "the sink was offered the bytes while Busy");
    const int calls_before_cancel = sinks.scripted.still_calls();
    monitor.Cancel();
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Cancelled, "Busy request cancels logically");
    Check(sinks.scripted.still_calls() == calls_before_cancel, "cancel does not call DisplayStill again");
    Check(sinks.fake.still_display_count() == 0, "cancelled Busy request was never displayed");
    // The module is reusable after a logical cancel.
    MonitorOutput::Request next;
    next.frames[5] = frame;
    next.main_source_id = 5;
    next.state = MakeState({5}, 5);
    next.info.submit_id = 603;
    Check(monitor.Submit(next) == MonitorOutput::SubmitOutcome::Accepted, "request accepted after cancel");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "request displayed after cancel");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "cancel-busy test closed");
  }
}

void TestSinkFailures(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  {
    SinkPair sinks;
    sinks.scripted.ScriptStill(EYEQ::StillOutcome::DeviceError, 1);
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 701;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "device-error request accepted");
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Failed && step.stage == "still",
          "sink DeviceError surfaces as Failed at stage still");
    Check(!step.reason.empty(), "failure carries a reason");
    Check(sinks.fake.still_display_count() == 0, "DeviceError never reports Displayed");
    Check(!monitor.AcceptsRequests(), "a terminal sink failure stops accepting requests");
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Failed, "further submit reports Failed");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "device-error path still closes");
  }
  {
    SinkPair sinks;
    sinks.scripted.ScriptStill(EYEQ::StillOutcome::Closed, 1);
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 702;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "sink-closed request accepted");
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Failed, "sink Closed surfaces as Failed, not Displayed");
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
    sinks.scripted.ScriptEvent(event);
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 703;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "error-event request accepted");
    const auto step = Drive(monitor);
    Check(step.status == MonitorOutput::Status::Failed && step.stage == "event",
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
    sinks.scripted.ScriptStill(EYEQ::StillOutcome::Busy, 100);
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 801;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "close-pending request accepted");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "Close reclaims the Ready/Busy slot and closes");
    Check(sinks.fake.still_display_count() == 0, "close-time cancel never displayed");
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Closed, "submit after close is rejected");
  }
  {
    // Close timeout then retry: the first Close reports DrainTimeout because the
    // scripted sink refuses to close.
    SinkPair sinks;
    sinks.scripted.ScriptClose(EYEQ::CloseStatus::DrainTimeout, 1);
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    auto frame = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
    MonitorOutput::Request request;
    request.frames[5] = frame;
    request.main_source_id = 5;
    request.state = MakeState({5}, 5);
    request.info.submit_id = 802;
    Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "close-timeout request accepted");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "close-timeout request displayed");
    Check(monitor.Close(1000) == EYEQ::CloseStatus::DrainTimeout, "first Close reports DrainTimeout");
    Check(!monitor.close_reason().empty(), "DrainTimeout carries a reason");
    Check(monitor.Close(1000) == EYEQ::CloseStatus::Closed, "second Close completes");
    Check(monitor.Close(1000) == EYEQ::CloseStatus::Closed, "repeated Close stays Closed");
  }
}

void TestSceneValidation(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  SinkPair sinks;
  MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                        EYEQ::ScaleMethod::Bicubic);
  auto stale = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.9f, 0.9f, 0.9f}; });
  auto fresh = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.1f, 0.1f, 0.1f}; });

  // Unknown main source id.
  MonitorOutput::Request bad_main;
  bad_main.frames[9] = fresh;
  bad_main.main_source_id = 42;
  bad_main.state = MakeState({9}, 9);
  bad_main.info.submit_id = 901;
  Check(monitor.Submit(bad_main) == MonitorOutput::SubmitOutcome::Failed, "unknown main_source_id is rejected");
  Check(monitor.last_result().stage == "config" && !monitor.last_result().reason.empty(),
        "config failure reports stage and reason");

  // First request establishes the renderer's frame map.
  MonitorOutput::Request first;
  first.frames[5] = stale;
  first.main_source_id = 5;
  first.state = MakeState({5}, 5);
  first.info.submit_id = 902;
  Check(monitor.Submit(first) == MonitorOutput::SubmitOutcome::Accepted, "first scene accepted with id 5");
  Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "first scene displayed");
  const uint64_t renders_after_first = monitor.diagnostics().render_count;

  // Second request only supplies id 9 but its DisplayState still points at id 5.
  // The stale frame must not be reused to stand in for the missing input.
  MonitorOutput::Request stale_scene;
  stale_scene.frames[9] = fresh;
  stale_scene.main_source_id = 9;
  stale_scene.state = MakeState({9}, 9);
  stale_scene.state.fill.id = 5;
  stale_scene.info.submit_id = 903;
  Check(monitor.Submit(stale_scene) == MonitorOutput::SubmitOutcome::Failed,
        "a scene referencing an id outside the submitted frame set is rejected");
  Check(monitor.diagnostics().render_count == renders_after_first, "rejected scene was not rendered");

  // Grid scene referencing a missing cell id, using a Grid monitor so the Grid
  // validation branch is the one exercised.
  {
    SinkPair grid_sinks;
    MonitorOutput grid_monitor(context, resolved, grid_sinks.sink(), EYEQ::DisplayMode::Grid,
                               EYEQ::ScaleMethod::Bicubic, EYEQ::ScaleMethod::Bicubic);
    MonitorOutput::Request grid_missing;
    grid_missing.frames[9] = fresh;
    grid_missing.main_source_id = 9;
    grid_missing.state = MakeState({9, 5}, 9);
    grid_missing.state.grid.cols = 2;
    grid_missing.state.grid.rows = 1;
    grid_missing.info.submit_id = 904;
    Check(grid_monitor.Submit(grid_missing) == MonitorOutput::SubmitOutcome::Failed,
          "Grid scene with a missing cell id is rejected");
    Check(grid_monitor.Close(2000) == EYEQ::CloseStatus::Closed, "grid scene-validation monitor closed");
  }
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "scene-validation monitor closed");
}

// F2 regression: every source a mode actually samples must come from THIS request,
// including the amplify reference in the non-ref view, and show_ref must carry a
// ref_id. The warm-up request leaves id 42 in the renderer cache so a stale reuse
// would be visible as a successful display.
void TestSceneDependencies(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  auto base = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
  auto other = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.6f, 0.6f, 0.6f}; });
  auto reference = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.15f, 0.15f, 0.15f}; });

  const EYEQ::DisplayMode modes[] = {EYEQ::DisplayMode::Slide, EYEQ::DisplayMode::Grid};
  const char *names[] = {"Slide", "Grid"};
  for (int m = 0; m < 2; ++m) {
    SinkPair sinks;
    MonitorOutput monitor(context, resolved, sinks.sink(), modes[m], EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);

    auto configure_grid = [&](MonitorOutput::Request &r) {
      if (modes[m] == EYEQ::DisplayMode::Grid) {
        r.state.grid.cols = 2;
        r.state.grid.rows = 1;
      }
    };

    // Warm the renderer cache with 5/9/42.
    MonitorOutput::Request warm;
    warm.frames[5] = base;
    warm.frames[9] = other;
    warm.frames[42] = reference;
    warm.main_source_id = 5;
    warm.state = MakeState({5, 9}, 5);
    configure_grid(warm);
    warm.info.submit_id = 1200 + m;
    Check(monitor.Submit(warm) == MonitorOutput::SubmitOutcome::Accepted,
          std::string(names[m]) + " warm-up request accepted");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed,
          std::string(names[m]) + " warm-up request displayed");

    // Only 5/9 are fed, but amplify + ref_id 42 are set: the cached 42 must not be
    // borrowed to satisfy the request.
    MonitorOutput::Request stale;
    stale.frames[5] = base;
    stale.frames[9] = other;
    stale.main_source_id = 5;
    stale.state = MakeState({5, 9}, 5);
    configure_grid(stale);
    stale.state.ref_id = 42;
    stale.state.amplify = 2.0f;
    stale.info.submit_id = 1210 + m;
    Check(monitor.Submit(stale) == MonitorOutput::SubmitOutcome::Failed,
          std::string(names[m]) + " amplify reference missing from this request is rejected");
    Check(monitor.last_result().stage == "config", std::string(names[m]) + " missing reference is a config failure");
    Check(monitor.diagnostics().displayed == 1,
          std::string(names[m]) + " rejected stale-reference request never displayed");

    // show_ref without ref_id is a config error, not a renderer exception.
    MonitorOutput::Request no_ref;
    no_ref.frames[5] = base;
    no_ref.main_source_id = 5;
    no_ref.state = MakeState({5}, 5);
    no_ref.state.show_ref = true;
    no_ref.info.submit_id = 1220 + m;
    Check(monitor.Submit(no_ref) == MonitorOutput::SubmitOutcome::Failed,
          std::string(names[m]) + " show_ref without ref_id is rejected");

    // show_ref with a ref_id that is not in this request is rejected too.
    MonitorOutput::Request ref_missing;
    ref_missing.frames[5] = base;
    ref_missing.main_source_id = 5;
    ref_missing.state = MakeState({5}, 5);
    ref_missing.state.show_ref = true;
    ref_missing.state.ref_id = 42;
    ref_missing.info.submit_id = 1230 + m;
    Check(monitor.Submit(ref_missing) == MonitorOutput::SubmitOutcome::Failed,
          std::string(names[m]) + " show_ref ref_id outside this request is rejected");

    // Positive control: the same amplify request succeeds when 42 is present.
    MonitorOutput::Request good;
    good.frames[5] = base;
    good.frames[9] = other;
    good.frames[42] = reference;
    good.main_source_id = 5;
    good.state = MakeState({5, 9}, 5);
    configure_grid(good);
    good.state.ref_id = 42;
    good.state.amplify = 2.0f;
    good.info.submit_id = 1240 + m;
    Check(monitor.Submit(good) == MonitorOutput::SubmitOutcome::Accepted,
          std::string(names[m]) + " amplify request with the reference present is accepted");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed,
          std::string(names[m]) + " amplify request displays");

    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, std::string(names[m]) + " dependency monitor closed");
  }

  // Fill amplify reference missing from this request.
  {
    SinkPair sinks;
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);
    MonitorOutput::Request stale;
    stale.frames[5] = base;
    stale.main_source_id = 5;
    stale.state = MakeState({5}, 5);
    stale.state.ref_id = 42;
    stale.state.amplify = 2.0f;
    stale.info.submit_id = 1250;
    Check(monitor.Submit(stale) == MonitorOutput::SubmitOutcome::Failed,
          "Fill amplify reference missing from this request is rejected");
    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "fill dependency monitor closed");
  }

  // Grid structure and null-frame preconditions.
  {
    SinkPair sinks;
    MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Grid, EYEQ::ScaleMethod::Bicubic,
                          EYEQ::ScaleMethod::Bicubic);

    // A 2x2 grid with only 3 ids is legal: GridRender skips the trailing empty cell.
    // The composition layer must accept it so it goes through the same renderer path
    // the player uses instead of imposing a stricter grid contract.
    MonitorOutput::Request partial;
    partial.frames[5] = base;
    partial.frames[9] = other;
    partial.frames[42] = reference;
    partial.main_source_id = 5;
    partial.state = MakeState({5, 9, 42}, 5);
    partial.state.grid.cols = 2;
    partial.state.grid.rows = 2;
    partial.info.submit_id = 1260;
    Check(monitor.Submit(partial) == MonitorOutput::SubmitOutcome::Accepted,
          "2x2 grid with 3 ids (trailing empty cell) is accepted");
    Check(Drive(monitor).status == MonitorOutput::Status::Displayed,
          "2x2 grid with 3 ids displays like the player renderer");
    Check(monitor.diagnostics().displayed == 1, "the partial grid displayed exactly once");

    // Empty ids, non-positive dimensions (in BOTH the normal and reference view) and
    // null entries are rejected before any mapping/feed.
    MonitorOutput::Request empty_ids;
    empty_ids.frames[5] = base;
    empty_ids.main_source_id = 5;
    empty_ids.state = MakeState({5}, 5);
    empty_ids.state.ids.clear();
    empty_ids.state.grid.cols = 2;
    empty_ids.state.grid.rows = 2;
    empty_ids.info.submit_id = 1261;
    Check(monitor.Submit(empty_ids) == MonitorOutput::SubmitOutcome::Failed, "grid with empty ids is rejected");

    MonitorOutput::Request zero_grid;
    zero_grid.frames[5] = base;
    zero_grid.main_source_id = 5;
    zero_grid.state = MakeState({5}, 5);
    zero_grid.state.grid.cols = 0;
    zero_grid.state.grid.rows = 1;
    zero_grid.info.submit_id = 1262;
    Check(monitor.Submit(zero_grid) == MonitorOutput::SubmitOutcome::Failed, "grid with zero columns is rejected");

    MonitorOutput::Request ref_zero_grid;
    ref_zero_grid.frames[5] = base;
    ref_zero_grid.main_source_id = 5;
    ref_zero_grid.state = MakeState({5}, 5);
    ref_zero_grid.state.show_ref = true;
    ref_zero_grid.state.ref_id = 5;
    ref_zero_grid.state.grid.cols = 0;
    ref_zero_grid.state.grid.rows = 1;
    ref_zero_grid.info.submit_id = 1263;
    Check(monitor.Submit(ref_zero_grid) == MonitorOutput::SubmitOutcome::Failed,
          "reference grid with zero columns is rejected");

    MonitorOutput::Request null_cell;
    null_cell.frames[5] = base;
    null_cell.frames[9] = nullptr;
    null_cell.main_source_id = 5;
    null_cell.state = MakeState({5, 9}, 5);
    null_cell.state.grid.cols = 2;
    null_cell.state.grid.rows = 1;
    null_cell.info.submit_id = 1264;
    Check(monitor.Submit(null_cell) == MonitorOutput::SubmitOutcome::Failed, "grid null cell frame is rejected");

    // The renderer maps EVERY submitted entry, so an unreferenced null entry must be
    // rejected too (it used to crash av_frame_ref inside FeedFrames).
    MonitorOutput::Request null_unreferenced;
    null_unreferenced.frames[5] = base;
    null_unreferenced.frames[99] = nullptr;
    null_unreferenced.main_source_id = 5;
    null_unreferenced.state = MakeState({5}, 5);
    null_unreferenced.info.submit_id = 1265;
    Check(monitor.Submit(null_unreferenced) == MonitorOutput::SubmitOutcome::Failed,
          "an unreferenced null frame entry is rejected before Feed");
    Check(monitor.diagnostics().displayed == 1, "no rejected grid reached the sink");

    MonitorOutput::Request null_main;
    null_main.frames[5] = nullptr;
    null_main.main_source_id = 5;
    null_main.state = MakeState({5}, 5);
    null_main.info.submit_id = 1266;
    Check(monitor.Submit(null_main) == MonitorOutput::SubmitOutcome::Failed, "null main source frame is rejected");

    Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "grid structure monitor closed");
  }
}

// F3 regression: a failing Submit must publish its own failure state, never leave
// the previous Displayed result describing the new failure.
void TestFeedFailure(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  (void)gpu;
  const EYEQ::ResolvedOutput resolved = BuildResolved(64, 54, EYEQ::PixelFormat::R210, false, 203.0, 0);
  SinkPair sinks;
  MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                        EYEQ::ScaleMethod::Bicubic);
  auto good = MakeFrame(stream, 64, 54, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
  MonitorOutput::Request ok;
  ok.frames[5] = good;
  ok.main_source_id = 5;
  ok.state = MakeState({5}, 5);
  ok.info.submit_id = 400;
  Check(monitor.Submit(ok) == MonitorOutput::SubmitOutcome::Accepted, "feed-failure first request accepted");
  Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "feed-failure first request displayed");
  Check(monitor.last_result().status == MonitorOutput::Status::Displayed && monitor.last_result().submit_id == 400,
        "the successful result is published before the failing submit");

  auto broken = MakeBrokenFrame(stream, 64, 54);
  MonitorOutput::Request bad;
  bad.frames[9] = broken;
  bad.main_source_id = 9;
  bad.state = MakeState({9}, 9);
  bad.info.submit_id = 401;
  Check(monitor.Submit(bad) == MonitorOutput::SubmitOutcome::Failed, "an unsupported frame format fails the submit");
  Check(monitor.status() == MonitorOutput::Status::Failed, "status reflects the feed failure");
  Check(monitor.last_result().status == MonitorOutput::Status::Failed && monitor.last_result().stage == "feed",
        "last_result reports the new feed failure, not the old Displayed result");
  Check(monitor.last_result().submit_id == 401, "failure identity is the failing submit");
  Check(!monitor.last_result().reason.empty(), "feed failure carries a reason");
  Check(sinks.fake.still_display_count() == 1, "the failed feed never reached the sink");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "feed-failure monitor closed");
}

void TestIsolation(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  // A second, independent renderer with a different target must not affect the
  // monitor output. This only shows resource isolation, not a UI viewport.
  EYEQ::FrameRenderer::Config desktop_config;
  desktop_config.mode = EYEQ::DisplayMode::Fill;
  desktop_config.pixel_density = 1.0f;
  desktop_config.enable_osd = false;
  EYEQ::FrameRenderer desktop(context, desktop_config);
  auto desktop_source = MakeFrame(stream, 32, 32, [](int, int) { return Color{0.7f, 0.2f, 0.1f}; });
  std::map<int, std::shared_ptr<AVFrame>> desktop_frames;
  desktop_frames[1] = desktop_source;
  desktop.SetMainSource(1);
  Check(desktop.FeedFrames(desktop_frames).empty(), "desktop renderer accepted its own frame");
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
  desktop.Render(desktop_target, desktop_state);

  const EYEQ::ResolvedOutput resolved = BuildResolved(96, 64, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                        EYEQ::ScaleMethod::Bicubic);
  auto frame = MakeFrame(stream, 96, 64, [](int x, int y) { return QuadrantPixel(x, y, 96, 64); });
  MonitorOutput::Request request;
  request.frames[5] = frame;
  request.main_source_id = 5;
  request.state = MakeState({5}, 5);
  request.info.submit_id = 1001;
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "isolated monitor request accepted");
  Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "isolated monitor request displayed");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 1001);
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "isolation monitor closed");
  pl_tex_destroy(gpu, &desktop_tex);
}

void TestUhdOnce(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const int width = 3840;
  const int height = 2160;
  const EYEQ::ResolvedOutput resolved = BuildResolved(width, height, EYEQ::PixelFormat::R210, false, 203.0, 16);
  SinkPair sinks;
  MonitorOutput monitor(context, resolved, sinks.sink(), EYEQ::DisplayMode::Fill, EYEQ::ScaleMethod::Bicubic,
                        EYEQ::ScaleMethod::Bicubic);
  auto frame = MakeFrame(stream, width, height, [](int x, int y) { return QuadrantPixel(x, y, 3840, 2160); });
  MonitorOutput::Request request;
  request.frames[5] = frame;
  request.main_source_id = 5;
  request.state = MakeState({5}, 5);
  request.info.submit_id = 1101;
  Check(monitor.Submit(request) == MonitorOutput::SubmitOutcome::Accepted, "UHD request accepted");
  Check(Drive(monitor).status == MonitorOutput::Status::Displayed, "UHD request displayed");
  CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 1101);
  Check(monitor.Close(5000) == EYEQ::CloseStatus::Closed, "UHD monitor closed");
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
    TestSceneValidation(gpu, context, owner.stream);
    TestSceneDependencies(gpu, context, owner.stream);
    TestFeedFailure(gpu, context, owner.stream);
    TestIsolation(gpu, context, owner.stream);
    TestUhdOnce(gpu, context, owner.stream);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
