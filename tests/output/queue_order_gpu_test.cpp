// Queue-order invariant GPU sentinel test (refactor R0).
//
// It validates the assumption that lets the monitor output drop the borrow
// protocol: on one pl_gpu there is a single queue, and the encode dispatched by
// MonitorOutput::Submit is submitted before any later recomposition. Therefore a
// recomposition that immediately overwrites the borrowed common result texture
// cannot corrupt an already-dispatched encode.
//
// The test composes scene A, submits it to the real MonitorOutput (which encodes
// and starts packing it) and, without pumping or waiting, immediately recomposes a
// different scene B into the very same common result texture. Both requests must
// still reach the sink with bytes that match the independent CPU packing oracle
// for their own scene: A must be A and B must be B, byte for byte.
//
// Headless Vulkan device, real FrameRenderer/FramePacker, scripted fake sink. No
// DeckLink SDK, no window, no device.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
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

// Composes @p frames as a new scene and returns a monitor request around the
// borrowed common result.
EYEQ::MonitorOutput::Request ComposeRequest(EYEQ::DisplayCoordinator &coordinator,
                                            const std::map<int, std::shared_ptr<AVFrame>> &frames,
                                            const EYEQ::DisplayState &state, int main_id, uint64_t submit_id) {
  coordinator.SetMainSource(main_id);
  coordinator.FeedFrames(frames);
  EYEQ::MonitorOutput::Request request;
  request.frame = coordinator.RenderScene(state);
  request.info.submit_id = submit_id;
  request.info.source_frame_serial = 0;
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

// Pump() is deliberately non-blocking; yield so a bounded poll loop actually
// gives the GPU time to finish instead of spinning out its iteration bound.
MonitorOutput::StepResult Drive(MonitorOutput &monitor, int max_pumps = 50000) {
  for (int i = 0; i < max_pumps; ++i) {
    const MonitorOutput::StepResult step = monitor.Pump();
    if (step.outcome != MonitorOutput::Outcome::None) {
      return step;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("monitor did not finish within the pump bound");
}

struct SinkPair {
  EYEQ::test::FakeFrameSink fake{1};
  std::unique_ptr<eyeq_test::ScriptedFrameSink> scripted = std::make_unique<eyeq_test::ScriptedFrameSink>(fake);
  // Hands the owned sink to MonitorOutput; @p fake stays alive for inspection.
  std::unique_ptr<EYEQ::FrameSink> take() { return std::move(scripted); }
};

// Compares the sink's stored bytes for @p submit_id against the independent CPU
// packing oracle of the RGB values currently in the monitor target.
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

// ---------------------------------------------------------------------------
// The invariant under test: an encode dispatched by Submit is ordered before a
// recomposition that immediately overwrites the borrowed common result texture.
// ---------------------------------------------------------------------------
void TestEncodeBeforeImmediateRecompose(pl_gpu gpu, EYEQ::RenderContext &context, AVStream *stream) {
  const int w = 96;
  const int h = 64;
  const EYEQ::ResolvedOutput resolved = BuildResolved(w, h, EYEQ::PixelFormat::R210, false, 203.0);
  SinkPair sinks;
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, resolved);
  MonitorOutput monitor(context, *coordinator, sinks.take(), resolved);

  const Color scene_a{0.8f, 0.15f, 0.1f};
  const Color scene_b{0.1f, 0.15f, 0.8f};

  // Scene A is composed and submitted (encoded into the monitor target) first.
  auto frame_a = MakeFrame(stream, w, h, [&](int, int) { return scene_a; });
  EYEQ::DisplayState state = MakeState({5}, 5);
  EYEQ::MonitorOutput::Request request_a = ComposeRequest(*coordinator, {{5, frame_a}}, state, 5, 101);
  const pl_tex borrowed = request_a.frame.texture;
  Check(borrowed != nullptr, "scene A produced a valid common result texture");
  Check(monitor.Submit(request_a) == MonitorOutput::SubmitOutcome::Accepted, "scene A request accepted");

  // Immediately (no pump, no wait) compose a different scene B. pl_tex_recreate
  // reuses the same work texture, so this overwrites the texture scene A still
  // refers to. The encode dispatched by Submit must already have read A.
  auto frame_b = MakeFrame(stream, w, h, [&](int, int) { return scene_b; });
  EYEQ::MonitorOutput::Request request_b = ComposeRequest(*coordinator, {{5, frame_b}}, state, 5, 102);
  Check(request_b.frame.texture == borrowed,
        "the recomposition reused the borrowed common result texture (otherwise the test would be vacuous)");
  Check(request_b.frame.identity.render_id != request_a.frame.identity.render_id,
        "scene B is a real recomposition with a new render identity");

  // Scene A must survive the immediate overwrite: drive it to the sink and check
  // its bytes against an oracle built from the target that encode A produced.
  const auto step_a = Drive(monitor);
  Check(step_a.outcome == MonitorOutput::Outcome::Displayed, "scene A request displayed after the overwrite");
  {
    const std::vector<float> rgb_a = DownloadTargetRgb(gpu, monitor);
    Check(Close(AtRgb(rgb_a, w, w / 2, h / 2), scene_a, 0.05f),
          "scene A target still carries scene A, got " + ColorString(AtRgb(rgb_a, w, w / 2, h / 2)));
    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 101);
  }
  const std::vector<uint8_t> bytes_a = *sinks.fake.copy(101);

  // Scene B is submitted and displayed with its own bytes.
  Check(monitor.Submit(request_b) == MonitorOutput::SubmitOutcome::Accepted, "scene B request accepted");
  const auto step_b = Drive(monitor);
  Check(step_b.outcome == MonitorOutput::Outcome::Displayed, "scene B request displayed");
  {
    const std::vector<float> rgb_b = DownloadTargetRgb(gpu, monitor);
    Check(Close(AtRgb(rgb_b, w, w / 2, h / 2), scene_b, 0.05f),
          "scene B target carries scene B, got " + ColorString(AtRgb(rgb_b, w, w / 2, h / 2)));
    CheckPackedAgainstOracle(sinks, monitor, gpu, resolved, 102);
  }
  const std::vector<uint8_t> bytes_b = *sinks.fake.copy(102);

  Check(bytes_a != bytes_b, "the two outputs are distinct, so the byte checks are not vacuous");
  Check(monitor.Close(2000) == EYEQ::CloseStatus::Closed, "queue-order monitor closed");
}

} // namespace

int main() {
  g_failures = 0;
  g_checks = 0;

  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    pl_gpu gpu = context.gpu();
    SourceOwner owner;

    TestEncodeBeforeImmediateRecompose(gpu, context, owner.stream);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  std::fprintf(stdout, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
