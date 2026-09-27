// Coordination-layer tests: the DisplayCoordinator owns the single FrameRenderer,
// the fixed mapping target, the canvas and the scene/render identity, and the ICC
// / precision / NaN-Inf policies live here. Headless Vulkan device, no window, no
// DeckLink SDK.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
}
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/canvas_geometry.hpp"
#include "display/display_coordinator.hpp"
#include "display/output_encode.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"

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

void Skip(const std::string &what) {
  ++g_skipped;
  std::fprintf(stderr, "  SKIP %s\n", what.c_str());
}

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

using Color = std::array<float, 3>;
using PixelFunc = std::function<Color(int, int)>;

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
      Color c = pixel(x, y);
      g[x] = c[1];
      b[x] = c[2];
      r[x] = c[0];
    }
  }
  frame->opaque = stream;
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

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

EYEQ::MappingTarget MakeMappingTarget(enum pl_color_primaries primaries, float peak_nits) {
  EYEQ::MappingTarget mt;
  mt.primaries = primaries;
  mt.peak_nits = peak_nits;
  mt.black_nits = 0.0f;
  mt.sdr_white_nits = PL_COLOR_SDR_WHITE;
  return mt;
}

std::unique_ptr<EYEQ::DisplayCoordinator> MakeCoordinator(EYEQ::RenderContext &context, EYEQ::DisplayMode mode,
                                                          int canvas_w, int canvas_h, const EYEQ::MappingTarget &target,
                                                          EYEQ::IccMode icc_mode = EYEQ::IccMode::Enabled,
                                                          bool validate_difference = true, bool present_dither = true) {
  EYEQ::DisplayCoordinator::Config cfg;
  cfg.mode = mode;
  cfg.scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.plane_scale_method = EYEQ::ScaleMethod::Bicubic;
  cfg.pixel_density = 1.0f;
  cfg.enable_osd = false;
  cfg.colorspace_hint = false;
  cfg.icc_mode = icc_mode;
  cfg.work_precision = EYEQ::WorkPrecision::F16;
  cfg.present_dither = present_dither;
  cfg.mapping_target = target;
  cfg.canvas_w = canvas_w;
  cfg.canvas_h = canvas_h;
  cfg.validate_difference_result = validate_difference;
  return std::make_unique<EYEQ::DisplayCoordinator>(context, cfg, nullptr);
}

struct OffscreenTarget {
  pl_tex tex = nullptr;
  struct pl_frame frame{};
  int width = 0;
  int height = 0;
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
      .w = width,
      .h = height,
      .format = format,
      .sampleable = true,
      .renderable = true,
      .host_readable = true,
  };
  OffscreenTarget target;
  target.tex = pl_tex_create(gpu, &params);
  if (!target.tex) {
    throw std::runtime_error("offscreen target creation failed");
  }
  target.width = width;
  target.height = height;
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

std::vector<float> DownloadTexture(pl_gpu gpu, pl_tex tex) {
  const std::size_t w = tex->params.w;
  const std::size_t h = tex->params.h;
  std::vector<float> pixels(w * h * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = tex,
      .row_pitch = w * 4 * sizeof(float),
      .ptr = pixels.data(),
  };
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("texture download failed");
  }
  return pixels;
}

std::vector<std::filesystem::path> IccProfileCandidates() {
  return {
      "/System/Library/ColorSync/Profiles/Display P3.icc",
      "/System/Library/ColorSync/Profiles/sRGB Profile.icc",
      "/System/Library/ColorSync/Profiles/AdobeRGB1998.icc",
  };
}

std::vector<uint8_t> ReadFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return {};
  }
  const auto size = file.tellg();
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> data(static_cast<std::size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(data.data()), size);
  return data;
}

// 8-bit UNORM target, like an sRGB desktop swapchain (a transfer the explicit
// wire encoder does not handle, so the color-managed dither hop is used).
OffscreenTarget CreateTarget8(pl_gpu gpu, int width, int height, enum pl_color_primaries primaries,
                              enum pl_color_transfer transfer) {
  pl_fmt format = pl_find_fmt(
      gpu, PL_FMT_UNORM, 4, 8, 8,
      static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
  if (!format) {
    throw std::runtime_error("no rgba8 texture format available");
  }
  struct pl_tex_params params = {
      .w = width, .h = height, .format = format, .sampleable = true, .renderable = true, .host_readable = true};
  OffscreenTarget target;
  target.tex = pl_tex_create(gpu, &params);
  if (!target.tex) {
    throw std::runtime_error("rgba8 target creation failed");
  }
  target.width = width;
  target.height = height;
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

std::vector<uint8_t> DownloadTexture8(pl_gpu gpu, pl_tex tex) {
  const std::size_t w = tex->params.w;
  const std::size_t h = tex->params.h;
  std::vector<uint8_t> pixels(w * h * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = tex,
      .row_pitch = w * 4,
      .ptr = pixels.data(),
  };
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("8-bit texture download failed");
  }
  return pixels;
}

// ---------------------------------------------------------------------------
// Single renderer + scene cache
// ---------------------------------------------------------------------------
void TestSceneCacheAndIdentity(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[single renderer and scene cache]\n");
  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_2020, 1000.0f);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 64, 48, mt);
  auto source = MakeFrame(stream, 64, 48, [](int, int) { return Color{0.4f, 0.4f, 0.4f}; });

  coordinator->SetMainSource(5);
  Check(coordinator->FeedFrames({{5, source}}).empty(), "feed accepted");
  Check(coordinator->feed_count() == 1, "feed counter increments once");

  EYEQ::DisplayState state = MakeState({5}, 5);
  const EYEQ::RenderedFrame &first = coordinator->RenderScene(state);
  Check(first.IsValid(), "the first RenderScene produces a valid result");
  Check(coordinator->scene_render_count() == 1, "the first RenderScene composed once");
  Check(first.width == 64 && first.height == 48, "the result is the fixed canvas size");
  Check(first.identity.render_id == 1 && first.identity.scene_revision == 1, "the first result identity is 1/1");

  // Re-composing the same scene is a no-op (UI-only changes must not re-run the
  // source mapping).
  const EYEQ::RenderedFrame &again = coordinator->RenderScene(state);
  Check(coordinator->scene_render_count() == 1, "an unchanged scene is not recomposed");
  Check(&again == &first && again.identity.render_id == first.identity.render_id,
        "the cached result is returned unchanged");

  // A geometry change is a new scene.
  state.offset_x = 3.0f;
  const EYEQ::RenderedFrame &moved = coordinator->RenderScene(state);
  Check(coordinator->scene_render_count() == 2, "a geometry change recomposes");
  Check(moved.identity.render_id == 2 && moved.identity.scene_revision == 2, "the new result gets a new identity");

  // A new feed is a new scene even with the same state.
  auto source2 = MakeFrame(stream, 64, 48, [](int, int) { return Color{0.6f, 0.6f, 0.6f}; });
  coordinator->FeedFrames({{5, source2}});
  const EYEQ::RenderedFrame &fed = coordinator->RenderScene(state);
  Check(coordinator->scene_render_count() == 3 && fed.identity.render_id == 3, "a new feed recomposes the scene");
}

// ---------------------------------------------------------------------------
// Fixed mapping target + resize does not re-run the source mapping
// ---------------------------------------------------------------------------
void TestFixedMappingTargetAndResize(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[fixed mapping target and resize]\n");
  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_2020, 1000.0f);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 64, 48, mt);
  auto source = MakeFrame(stream, 64, 48, [](int, int) { return Color{0.5f, 0.5f, 0.5f}; });
  coordinator->SetMainSource(5);
  coordinator->FeedFrames({{5, source}});
  EYEQ::DisplayState state = MakeState({5}, 5);

  const EYEQ::RenderedFrame &result = coordinator->RenderScene(state);
  Check(result.color.primaries == PL_COLOR_PRIM_BT_2020, "the common result uses the fixed mapping target primaries");
  Check(result.color.transfer == PL_COLOR_TRC_LINEAR, "the common result is display-linear");

  // Present to two differently-sized targets with a target primaries that
  // differs from the mapping target. The common result (and its primaries) must
  // not drift with the presentation target.
  auto small = CreateTarget(context.gpu(), 64, 48, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  auto large = CreateTarget(context.gpu(), 200, 120, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  const uint64_t renders_before = coordinator->scene_render_count();
  coordinator->PresentTo(small.frame, state);
  coordinator->PresentTo(large.frame, state);
  Check(coordinator->present_count() == 2, "both presentations were counted");
  Check(coordinator->scene_render_count() == renders_before,
        "a presentation resize does not re-run the source mapping/HDR work");
  const EYEQ::RenderedFrame &after = coordinator->result();
  Check(after.color.primaries == PL_COLOR_PRIM_BT_2020,
        "the common result primaries still follow the fixed mapping target after presenting");

  // The letterboxed preview: the large target is wider than the canvas aspect, so
  // the top rows are black bars and the center is the image.
  std::vector<float> pixels = DownloadTexture(context.gpu(), large.tex);
  const auto at = [&](int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * large.width + x) * 4;
    return Color{pixels[i], pixels[i + 1], pixels[i + 2]};
  };
  const Color bar = at(2, 60);
  Check(std::fabs(bar[0]) < 0.02f && std::fabs(bar[1]) < 0.02f && std::fabs(bar[2]) < 0.02f,
        "the letterbox bar is black");
  const Color center = at(100, 60);
  Check(center[0] > 0.2f && std::fabs(center[0] - center[1]) < 0.02f && std::fabs(center[0] - center[2]) < 0.02f,
        "the preview center shows the neutral grey image");

  pl_tex_destroy(context.gpu(), &small.tex);
  pl_tex_destroy(context.gpu(), &large.tex);
}

// ---------------------------------------------------------------------------
// V2-1 handoff debt #1: the plain-desktop pixel behavior of the new chain
// (coordinator compose + presenter hop) against the migrated baseline chain
// (FrameRenderer::Render, the fused V2-1 path) on the same scene and target.
// Differences are reported, not assumed equal. Dither is disabled on both sides
// so only the structural path is compared; the ICC case is compared too.
// ---------------------------------------------------------------------------
void CompareOldAndNew(EYEQ::RenderContext &context, AVStream *stream, const std::vector<uint8_t> *icc,
                      const std::string &icc_source, const char *label) {
  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_709, 203.0f);
  const int w = 96;
  const int h = 48;
  auto source = MakeFrame(stream, w, h, [](int x, int y) {
    const float v = (x + y) / static_cast<float>(192);
    return Color{v, 0.5f * v, 1.0f - v};
  });

  EYEQ::FrameRenderer::Config old_cfg;
  old_cfg.mode = EYEQ::DisplayMode::Fill;
  old_cfg.enable_osd = false;
  old_cfg.pixel_density = 1.0f;
  old_cfg.colorspace_hint = false;
  old_cfg.present_dither = false;
  EYEQ::FrameRenderer old_renderer(context, old_cfg);
  old_renderer.SetMappingTarget(mt, EYEQ::WorkPrecision::F16);
  old_renderer.SetMainSource(5);
  old_renderer.FeedFrames({{5, source}});
  if (icc != nullptr) {
    old_renderer.SetICCProfile(*icc, icc_source);
  }

  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, w, h, mt, EYEQ::IccMode::Enabled, false, false);
  coordinator->SetMainSource(5);
  coordinator->FeedFrames({{5, source}});
  if (icc != nullptr) {
    coordinator->LoadICCProfile(*icc, icc_source);
  }

  auto old_target = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  auto new_target = CreateTarget(context.gpu(), w, h, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
  EYEQ::DisplayState state = MakeState({5}, 5);
  old_renderer.Render(old_target.frame, state);
  coordinator->RenderScene(state);
  coordinator->PresentTo(new_target.frame, state);

  const std::vector<float> a = DownloadTexture(context.gpu(), old_target.tex);
  const std::vector<float> b = DownloadTexture(context.gpu(), new_target.tex);
  float max_diff = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    max_diff = std::max(max_diff, std::fabs(a[i] - b[i]));
  }
  std::fprintf(stdout, "  %s: migrated-baseline vs V2-2 max |diff| = %.6f\n", label, max_diff);
  Check(max_diff <= 0.02f, std::string(label) + ": the new chain matches the migrated baseline within 0.02");

  pl_tex_destroy(context.gpu(), &new_target.tex);
  pl_tex_destroy(context.gpu(), &old_target.tex);
}

void TestMigratedBaselineCompatibility(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[V2-1 migrated-baseline compatibility]\n");
  CompareOldAndNew(context, stream, nullptr, "", "no ICC");
  std::vector<uint8_t> icc_data;
  std::string icc_source;
  for (const auto &candidate : IccProfileCandidates()) {
    icc_data = ReadFile(candidate);
    if (!icc_data.empty()) {
      icc_source = candidate.string();
      break;
    }
  }
  if (icc_data.empty()) {
    Skip("ICC compatibility: no system ICC profile found");
  } else {
    CompareOldAndNew(context, stream, &icc_data, icc_source, "global ICC");
  }
}

// ---------------------------------------------------------------------------
// Performance / resource record (F16 vs F32). Correctness is asserted
// elsewhere; this only prints data so the report can quote it.
// ---------------------------------------------------------------------------
void TestPerformanceRecording(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[performance and resource record]\n");
  pl_gpu gpu = context.gpu();
  const int w = 1920;
  const int h = 1080;
  const std::size_t f16_bytes = static_cast<std::size_t>(w) * h * 8;  // RGBA16F
  const std::size_t f32_bytes = static_cast<std::size_t>(w) * h * 16; // RGBA32F
  std::fprintf(stdout, "  canvas RGBA16F = %zu bytes (%.1f MiB); RGBA32F = %zu bytes (%.1f MiB)\n", f16_bytes,
               f16_bytes / 1048576.0, f32_bytes, f32_bytes / 1048576.0);

  pl_fmt fmt16 = pl_find_named_fmt(gpu, "rgba16f");
  pl_fmt fmt32 = pl_find_named_fmt(gpu, "rgba32f");
  if (!fmt16 || !fmt32) {
    Skip("performance record: rgba16f/rgba32f not available");
    return;
  }
  auto make_tex = [&](pl_fmt fmt, bool sampleable) {
    struct pl_tex_params params = {.w = w, .h = h, .format = fmt, .sampleable = sampleable, .renderable = !sampleable};
    return pl_tex_create(gpu, &params);
  };
  pl_tex src16 = make_tex(fmt16, true);
  pl_tex src32 = make_tex(fmt32, true);
  pl_tex dst = make_tex(fmt32, false);
  if (!src16 || !src32 || !dst) {
    Skip("performance record: texture allocation failed");
    return;
  }
  EYEQ::OutputEncoder encoder(context.log(), gpu);
  EYEQ::EncodeParams params;
  params.transfer = EYEQ::OutputTransfer::PQ;
  const int iterations = 20;
  auto record = [&](pl_tex src, const char *name) {
    pl_timer timer = pl_timer_create(gpu);
    pl_gpu_finish(gpu);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
      encoder.Encode(dst, src, params, EYEQ::EncodeRect{}, timer);
    }
    pl_gpu_finish(gpu);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
    uint64_t gpu_ns = 0;
    for (int i = 0; i < iterations; ++i) {
      const uint64_t result = pl_timer_query(gpu, timer);
      if (result == 0) {
        break;
      }
      gpu_ns += result;
    }
    // Anything left after the loop is whatever the implementation retained.
    uint64_t tail = 0;
    while (uint64_t result = pl_timer_query(gpu, timer)) {
      tail += result;
    }
    gpu_ns += tail;
    pl_timer_destroy(gpu, &timer);
    const double host_us = elapsed.count() / 1000.0 / iterations;
    std::fprintf(stdout, "  %s 1920x1080 PQ encode: host(incl. submit+finish) %.1f us/pass; GPU timer %.1f us/pass%s\n",
                 name, host_us, gpu_ns > 0 ? gpu_ns / 1000.0 / iterations : 0.0,
                 gpu_ns > 0 ? "" : " (no GPU timestamps available)");
  };
  record(src16, "16F->32F");
  record(src32, "32F->32F");
  pl_tex_destroy(gpu, &dst);
  pl_tex_destroy(gpu, &src32);
  pl_tex_destroy(gpu, &src16);
}

// ---------------------------------------------------------------------------
// sRGB desktop final hop and dither (closure b)
// ---------------------------------------------------------------------------
void TestPresentDither(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[sRGB desktop final hop and dither]\n");
  pl_gpu gpu = context.gpu();
  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_709, 203.0f);
  EYEQ::FrameRenderer::Config cfg;
  cfg.mode = EYEQ::DisplayMode::Fill;
  cfg.enable_osd = false;
  cfg.pixel_density = 1.0f;
  cfg.colorspace_hint = false;
  cfg.present_dither = true;
  EYEQ::FrameRenderer renderer(context, cfg);
  renderer.SetMappingTarget(mt, EYEQ::WorkPrecision::F16);
  // A near-flat horizontal gradient: a smooth 8-bit ramp is exactly where
  // quantization banding shows up.
  auto source = MakeFrame(stream, 256, 32, [](int x, int) {
    const float v = static_cast<float>(x + 1) / 257.0f;
    return Color{v, v, v};
  });
  renderer.SetMainSource(0);
  renderer.FeedFrames({{0, source}});
  EYEQ::DisplayState state = MakeState({0}, 0);

  auto target = CreateTarget8(gpu, 256, 32, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_SRGB);
  auto max_run = [&]() {
    renderer.Render(target.frame, state);
    const std::vector<uint8_t> px = DownloadTexture8(gpu, target.tex);
    int best = 1;
    int run = 1;
    for (int x = 1; x < 256; ++x) {
      const std::size_t i = (static_cast<std::size_t>(16) * 256 + x) * 4;
      if (px[i] == px[i - 4]) {
        ++run;
        best = std::max(best, run);
      } else {
        run = 1;
      }
    }
    return best;
  };
  renderer.SetPresentDitherEnabled(false);
  const int run_off = max_run();
  renderer.SetPresentDitherEnabled(true);
  const int run_on = max_run();
  Check(run_off >= 2, "the undithered 8-bit hop shows flat bands (control)");
  Check(run_on < run_off, "dithering breaks up the 8-bit banding of a smooth gradient");
  pl_tex_destroy(gpu, &target.tex);
}

// ---------------------------------------------------------------------------
// ICC mode isolation
// ---------------------------------------------------------------------------
void TestIccModeIsolation(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[ICC mode isolation]\n");
  std::vector<uint8_t> icc_data;
  std::filesystem::path icc_path;
  for (const auto &candidate : IccProfileCandidates()) {
    icc_data = ReadFile(candidate);
    if (!icc_data.empty()) {
      icc_path = candidate;
      break;
    }
  }
  if (icc_data.empty()) {
    Skip("ICC mode isolation: no system ICC profile found in the candidate list");
    return;
  }

  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_709, 203.0f);
  auto source = MakeFrame(stream, 32, 32, [](int, int) { return Color{0.4f, 0.5f, 0.6f}; });

  {
    // Plain desktop: ICC stays available (global and per-video).
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 32, 32, mt, EYEQ::IccMode::Enabled);
    coordinator->SetMainSource(5);
    coordinator->FeedFrames({{5, source}});
    coordinator->LoadICCProfile(icc_data, icc_path.string());
    Check(coordinator->IsIccActive(), "plain desktop reports ICC active after a global profile");
  }
  {
    // Monitoring mode: every application ICC is disabled, and loading one is
    // reported, not silently applied.
    auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 32, 32, mt, EYEQ::IccMode::MonitoringDisabled);
    coordinator->SetMainSource(5);
    coordinator->FeedFrames({{5, source}});
    coordinator->LoadICCProfile(icc_data, icc_path.string());
    coordinator->LoadICCProfile(icc_data, icc_path.string(), 5);
    Check(!coordinator->IsIccActive(), "monitoring mode reports ICC inactive after global and per-video loads");
    struct pl_color_space icc_target{};
    Check(!coordinator->GetICCTargetColorSpace(&icc_target), "monitoring mode never reports an ICC target color space");
    EYEQ::DisplayState state = MakeState({5}, 5);
    const EYEQ::RenderedFrame &result = coordinator->RenderScene(state);
    Check(result.IsValid(), "monitoring mode still composes a valid result");
  }
  {
    // The same profile must change the result on the desktop and not change it in
    // monitoring mode (a behaviour separation, not a flag-only check).
    auto desktop = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 32, 32, mt, EYEQ::IccMode::Enabled);
    auto monitor = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 32, 32, mt, EYEQ::IccMode::MonitoringDisabled);
    EYEQ::DisplayState state = MakeState({5}, 5);
    desktop->SetMainSource(5);
    monitor->SetMainSource(5);
    desktop->FeedFrames({{5, source}});
    monitor->FeedFrames({{5, source}});
    desktop->RenderScene(state);
    monitor->RenderScene(state);
    const std::vector<float> before = DownloadTexture(context.gpu(), monitor->result().texture);
    desktop->LoadICCProfile(icc_data, icc_path.string());
    monitor->LoadICCProfile(icc_data, icc_path.string());
    // Re-feed so both compose again with the new ICC state.
    desktop->FeedFrames({{5, source}});
    monitor->FeedFrames({{5, source}});
    desktop->RenderScene(state);
    const EYEQ::RenderedFrame &monitor_result = monitor->RenderScene(state);
    const std::vector<float> after = DownloadTexture(context.gpu(), monitor_result.texture);
    bool identical = before.size() == after.size();
    for (std::size_t i = 0; identical && i < before.size(); ++i) {
      if (std::fabs(before[i] - after[i]) > 1e-5f) {
        identical = false;
      }
    }
    Check(identical, "monitoring-mode output is unchanged by an ICC profile that the desktop would apply");
  }
}

// ---------------------------------------------------------------------------
// Precision reconfiguration with the borrow guard
// ---------------------------------------------------------------------------
void TestPrecisionReconfigure(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[precision reconfiguration]\n");
  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_2020, 1000.0f);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 48, 32, mt);
  auto source = MakeFrame(stream, 48, 32, [](int, int) { return Color{0.5f, 0.5f, 0.5f}; });
  coordinator->SetMainSource(5);
  coordinator->FeedFrames({{5, source}});
  EYEQ::DisplayState state = MakeState({5}, 5);

  Check(coordinator->work_precision() == EYEQ::WorkPrecision::F16, "the initial precision is F16");
  const EYEQ::RenderedFrame &first = coordinator->RenderScene(state);
  Check(first.precision == EYEQ::WorkPrecision::F16, "the plain result is F16");

  // Amplify forces the high-precision path regardless of the plain preference.
  EYEQ::DisplayState amp_state = state;
  amp_state.ref_id = 5;
  amp_state.amplify = 2.0f;
  const EYEQ::RenderedFrame &amp = coordinator->RenderScene(amp_state);
  Check(amp.precision == EYEQ::WorkPrecision::F32, "the difference path result is F32 even with the F16 preference");

  // Reconfiguration applies immediately and invalidates the cached scene, so the
  // next composition uses the new precision.
  coordinator->ReconfigurePrecision(EYEQ::WorkPrecision::F32);
  Check(coordinator->precision_reconfigure_count() == 1, "the reconfiguration is counted");
  const EYEQ::RenderedFrame &reconfigured = coordinator->RenderScene(state);
  Check(coordinator->work_precision() == EYEQ::WorkPrecision::F32, "the new precision is applied immediately");
  Check(reconfigured.precision == EYEQ::WorkPrecision::F32, "the recomposed result uses the new precision");
  coordinator->ReconfigurePrecision(EYEQ::WorkPrecision::F16);
  Check(coordinator->precision_reconfigure_count() == 2, "a second reconfiguration is counted");
  const EYEQ::RenderedFrame &back = coordinator->RenderScene(state);
  Check(back.precision == EYEQ::WorkPrecision::F16, "switching back recomposes at F16");
}

// ---------------------------------------------------------------------------
// Target semantics: --source-max-luma is a source-only override and never moves
// the mapping target; --target-display-nits is the single mapping/monitor peak.
// ---------------------------------------------------------------------------
void TestTargetSemantics(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[target semantics]\n");
  const EYEQ::MappingTarget base = MakeMappingTarget(PL_COLOR_PRIM_BT_709, 600.0f);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 64, 48, base);
  coordinator->SetMainSource(5);
  coordinator->FeedFrames({{5, MakeFrame(stream, 32, 32, [](int, int) { return Color{0.5f, 0.5f, 0.5f}; })}});
  EYEQ::DisplayState state = MakeState({5}, 5);
  coordinator->RenderScene(state);

  coordinator->SetSourceMaxLuma(800.0);
  Check(std::fabs(coordinator->mapping_target().peak_nits - 600.0f) < 0.5f,
        "--source-max-luma does not change the mapping target peak");
  coordinator->SetSdrWhiteOnHdr(120.0f);
  Check(std::fabs(coordinator->mapping_target().sdr_white_nits - PL_COLOR_SDR_WHITE) < 0.5f,
        "--sdr-white-on-hdr does not change the mapping target SDR white");
  coordinator->SetTargetPeakNits(500.0f, 100.0f);
  Check(std::fabs(coordinator->mapping_target().peak_nits - 500.0f) < 0.5f,
        "SetTargetPeakNits sets the comparison-baseline peak");
  Check(std::fabs(coordinator->mapping_target().sdr_white_nits - 100.0f) < 0.5f,
        "SetTargetPeakNits sets the comparison-baseline SDR white");
}

// ---------------------------------------------------------------------------
// NaN/Inf hard-error policy
// ---------------------------------------------------------------------------
void TestNanInfPolicy(EYEQ::RenderContext &context) {
  std::fprintf(stdout, "[NaN/Inf policy]\n");
  pl_gpu gpu = context.gpu();

  // The CPU oracle refuses non-finite input instead of clamping it away.
  EYEQ::EncodeParams params;
  params.transfer = EYEQ::OutputTransfer::SDR_BT1886;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  bool cpu_threw = false;
  try {
    EYEQ::EncodeNormToCode(nan, params);
  } catch (const std::invalid_argument &) {
    cpu_threw = true;
  }
  Check(cpu_threw, "the CPU encode oracle fails fast on NaN");
  cpu_threw = false;
  try {
    EYEQ::DecodeCodeToNorm(inf, params);
  } catch (const std::invalid_argument &) {
    cpu_threw = true;
  }
  Check(cpu_threw, "the CPU decode oracle fails fast on Inf");

  // The GPU gate throws on a non-finite work texture.
  pl_fmt format = pl_find_fmt(
      gpu, PL_FMT_FLOAT, 4, 32, 32,
      static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
  struct pl_tex_params tex_params = {
      .w = 4,
      .h = 1,
      .format = format,
      .sampleable = true,
      .renderable = true,
      .host_readable = true,
      .host_writable = true,
  };
  pl_tex bad = pl_tex_create(gpu, &tex_params);
  Check(bad != nullptr, "non-finite probe texture created");
  const float values[4] = {nan, inf, -inf, 0.0f};
  std::vector<float> upload(16);
  for (int x = 0; x < 4; ++x) {
    upload[x * 4 + 0] = values[x];
    upload[x * 4 + 1] = values[x];
    upload[x * 4 + 2] = values[x];
    upload[x * 4 + 3] = 1.0f;
  }
  struct pl_tex_transfer_params up = {
      .tex = bad,
      .row_pitch = 4 * 4 * sizeof(float),
      .ptr = upload.data(),
  };
  Check(pl_tex_upload(gpu, &up), "non-finite probe texture uploaded");
  bool gpu_threw = false;
  try {
    EYEQ::ValidateLinearTextureFinite(gpu, bad);
  } catch (const std::runtime_error &) {
    gpu_threw = true;
  }
  Check(gpu_threw, "the GPU finite gate fails fast on a NaN/Inf work texture");
  pl_tex_destroy(gpu, &bad);

  // A finite texture passes the gate.
  pl_tex good = pl_tex_create(gpu, &tex_params);
  std::vector<float> finite(16, 0.25f);
  struct pl_tex_transfer_params up2 = {
      .tex = good,
      .row_pitch = 4 * 4 * sizeof(float),
      .ptr = finite.data(),
  };
  Check(pl_tex_upload(gpu, &up2), "finite probe texture uploaded");
  bool ok = true;
  try {
    EYEQ::ValidateLinearTextureFinite(gpu, good);
  } catch (const std::exception &) {
    ok = false;
  }
  Check(ok, "the GPU finite gate accepts a finite work texture");
  pl_tex_destroy(gpu, &good);
}

// ---------------------------------------------------------------------------
// Feed failure is reported, not silently accepted
// ---------------------------------------------------------------------------
void TestFeedFailure(EYEQ::RenderContext &context, AVStream *stream) {
  std::fprintf(stdout, "[feed failure]\n");
  const EYEQ::MappingTarget mt = MakeMappingTarget(PL_COLOR_PRIM_BT_709, 203.0f);
  auto coordinator = MakeCoordinator(context, EYEQ::DisplayMode::Fill, 32, 32, mt);
  auto broken = MakeBrokenFrame(stream, 32, 32);
  const uint64_t feeds_before = coordinator->feed_count();
  const std::vector<int> failed = coordinator->FeedFrames({{5, broken}});
  Check(failed.size() == 1 && failed.front() == 5, "a frame the mapper cannot consume is reported by id");
  Check(coordinator->feed_count() == feeds_before, "a failed feed does not advance the feed counter");
}

} // namespace

int main() {
  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    pl_gpu gpu = context.gpu();
    if (!gpu) {
      std::fprintf(stderr, "FATAL: no headless Vulkan device\n");
      return 1;
    }
    SourceOwner owner;

    TestSceneCacheAndIdentity(context, owner.stream);
    TestFixedMappingTargetAndResize(context, owner.stream);
    TestPresentDither(context, owner.stream);
    TestIccModeIsolation(context, owner.stream);
    TestMigratedBaselineCompatibility(context, owner.stream);
    TestPrecisionReconfigure(context, owner.stream);
    TestTargetSemantics(context, owner.stream);
    TestPerformanceRecording(context);
    TestNanInfPolicy(context);
    TestFeedFailure(context, owner.stream);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }
  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
