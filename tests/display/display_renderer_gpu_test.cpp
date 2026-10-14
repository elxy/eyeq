// Offscreen display/rendering tests. These drive the real RenderContext and
// FrameRenderer with a headless Vulkan device (no DeckLink SDK, no window) and
// check pixel/geometry results, so a regression is reported as a failed
// assertion rather than just a crash.
//
// The test intentionally reuses the production display sources (listed in
// tests/display/CMakeLists.txt) instead of a mock so the resource boundaries
// under test are the ones the player uses.

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
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/frame_renderer.hpp"
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

// Records an optional subtest that could not run because of an environment
// precondition. It is reported separately and never counted as a passing check.
void Skip(const std::string &what) {
  ++g_skipped;
  std::fprintf(stderr, "  SKIP %s\n", what.c_str());
}

using Color = std::array<float, 3>;
using PixelFunc = std::function<Color(int, int)>;

// Build a full-range GBRPF32LE software frame. GBR planar data is stored as
// G/B/R in planes 0/1/2.
std::shared_ptr<AVFrame> MakeFrame(int width, int height, const PixelFunc &pixel) {
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
  frame->color_trc = AVCOL_TRC_BT709; // libplacebo maps this to BT.1886
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
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

// Allocate a frame of an arbitrary pixel format without filling meaningful
// data. Used to exercise the unsupported-format failure path.
std::shared_ptr<AVFrame> MakeRawFrame(int width, int height, AVPixelFormat format) {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("av_frame_alloc failed");
  }
  frame->format = format;
  frame->width = width;
  frame->height = height;
  frame->colorspace = AVCOL_SPC_RGB;
  frame->color_range = AVCOL_RANGE_JPEG;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("av_frame_get_buffer failed for unsupported-format probe");
  }
  return std::shared_ptr<AVFrame>(frame, [](AVFrame *f) { av_frame_free(&f); });
}

struct OffscreenTarget {
  pl_tex tex = nullptr;
  struct pl_frame frame{};
  int width = 0;
  int height = 0;
};

OffscreenTarget CreateTarget(pl_gpu gpu, int width, int height, enum pl_color_primaries primaries,
                             enum pl_color_transfer transfer, bool sampleable = true) {
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
      .sampleable = sampleable,
      .renderable = true,
      .host_readable = true,
  };
  OffscreenTarget target;
  target.tex = pl_tex_create(gpu, &params);
  if (!target.tex) {
    throw std::runtime_error("offscreen target texture creation failed");
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
  // Single-plane floating point RGB target: full range and RGB, with no
  // video-range compression and no extra quantization.
  target.frame.repr = pl_color_repr_rgb;
  target.frame.repr.sys = PL_COLOR_SYSTEM_RGB;
  target.frame.repr.levels = PL_COLOR_LEVELS_FULL;
  target.frame.color.primaries = primaries;
  target.frame.color.transfer = transfer;
  return target;
}

bool DestroyTarget(pl_gpu gpu, OffscreenTarget &target) {
  if (!target.tex) {
    return false;
  }
  pl_tex_destroy(gpu, &target.tex);
  return target.tex == nullptr;
}

std::vector<float> DownloadTarget(pl_gpu gpu, const OffscreenTarget &target) {
  std::vector<float> pixels(static_cast<std::size_t>(target.width) * target.height * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = target.tex,
      .row_pitch = static_cast<std::size_t>(target.width) * 4 * sizeof(float),
      .ptr = pixels.data(),
  };
  if (!pl_tex_download(gpu, &transfer)) {
    throw std::runtime_error("pl_tex_download failed");
  }
  return pixels;
}

float HalfToFloat(uint16_t v) {
  const uint32_t sign = (v >> 15) & 1u;
  const uint32_t exponent = (v >> 10) & 0x1Fu;
  const uint32_t mantissa = v & 0x3FFu;
  uint32_t bits = 0;
  if (exponent == 0) {
    bits = (sign << 31) | ((mantissa == 0) ? 0u : (static_cast<uint32_t>(127 - 15 + 1) << 23)) | (mantissa << 13);
  } else if (exponent == 0x1Fu) {
    bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
  } else {
    bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

// Downloads the display-linear composition (16F or 32F) of the last Render().
std::vector<float> DownloadLinearResult(pl_gpu gpu, EYEQ::FrameRenderer &renderer, int &width, int &height) {
  pl_tex tex = renderer.GetLinearResultTexture();
  width = tex->params.w;
  height = tex->params.h;
  const bool half = tex->params.format->component_depth[0] == 16;
  std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4);
  if (half) {
    std::vector<uint16_t> raw(static_cast<std::size_t>(width) * height * 4);
    struct pl_tex_transfer_params transfer = {
        .tex = tex,
        .row_pitch = static_cast<std::size_t>(width) * 4 * sizeof(uint16_t),
        .ptr = raw.data(),
    };
    if (!pl_tex_download(gpu, &transfer)) {
      throw std::runtime_error("linear result download failed");
    }
    for (std::size_t i = 0; i < raw.size(); ++i) {
      pixels[i] = HalfToFloat(raw[i]);
    }
  } else {
    struct pl_tex_transfer_params transfer = {
        .tex = tex,
        .row_pitch = static_cast<std::size_t>(width) * 4 * sizeof(float),
        .ptr = pixels.data(),
    };
    if (!pl_tex_download(gpu, &transfer)) {
      throw std::runtime_error("linear result download failed");
    }
  }
  return pixels;
}

Color At(const std::vector<float> &pixels, int width, int x, int y) {
  const float *p = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
  return {p[0], p[1], p[2]};
}

bool Close(const Color &a, const Color &b, float tolerance) {
  // Reject NaN/Inf on both sides and an invalid tolerance: a NaN operand would
  // otherwise make every |a-b| comparison false and appear "equal".
  if (!std::isfinite(tolerance) || tolerance < 0.0f) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
      return false;
    }
    if (std::fabs(a[i] - b[i]) > tolerance) {
      return false;
    }
  }
  return true;
}

bool AllClose(const std::vector<float> &a, const std::vector<float> &b, float tolerance) {
  if (!std::isfinite(tolerance) || tolerance < 0.0f || a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
      return false;
    }
    if (std::fabs(a[i] - b[i]) > tolerance) {
      return false;
    }
  }
  return true;
}

// ST2084 PQ code for absolute luminance in cd/m^2 (10000 = peak).
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

// A source with four clearly separated quadrants (TL red, TR green, BL blue,
// BR white) so an incorrect crop, scale or placement changes a checked pixel.
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

// Mutation tests for the pixel comparators themselves: they must reject NaN and
// Inf on either side, invalid tolerances and out-of-tolerance values.
void TestComparators() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  Check(!Close({nan, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.01f), "comparator rejects NaN on the left");
  Check(!Close({1.0f, 0.0f, 0.0f}, {nan, 0.0f, 0.0f}, 0.01f), "comparator rejects NaN on the right");
  Check(!Close({inf, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.01f), "comparator rejects +Inf");
  Check(!Close({-inf, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.01f), "comparator rejects -Inf");
  Check(!Close({1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, -0.01f), "comparator rejects a negative tolerance");
  Check(!Close({1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, nan), "comparator rejects a NaN tolerance");
  Check(!Close({1.0f, 0.0f, 0.0f}, {1.1f, 0.0f, 0.0f}, 0.01f), "comparator detects an out-of-tolerance value");
  Check(Close({1.0f, 0.0f, 0.0f}, {1.005f, 0.0f, 0.0f}, 0.01f), "comparator accepts a within-tolerance value");
  Check(!AllClose({nan}, {1.0f}, 0.01f), "AllClose rejects NaN on the left");
  Check(!AllClose({1.0f}, {nan}, 0.01f), "AllClose rejects NaN on the right");
  Check(!AllClose({inf}, {1.0f}, 0.01f), "AllClose rejects +Inf");
  Check(!AllClose({-inf}, {1.0f}, 0.01f), "AllClose rejects -Inf");
  Check(!AllClose({1.0f, 2.0f}, {1.0f, 2.0f, 3.0f}, 0.01f), "AllClose rejects a size mismatch");
  Check(!AllClose({1.0f, 2.0f}, {1.0f, 2.5f}, 0.01f), "AllClose detects an out-of-tolerance value");
  Check(!AllClose({1.0f, 2.0f}, {1.0f, 2.0f}, -1.0f), "AllClose rejects a negative tolerance");
  Check(AllClose({1.0f, 2.0f}, {1.0f, 2.0f}, 0.0f), "AllClose accepts identical vectors");
}

std::string ColorString(const Color &c) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "(%.4f, %.4f, %.4f)", c[0], c[1], c[2]);
  return buf;
}

std::string Fmt(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6f", v);
  return buf;
}

EYEQ::DisplayState MakeBaseState(const std::vector<int> &ids, int main_id) {
  EYEQ::DisplayState state{};
  state.ids = ids;
  state.scale = 1.0f;
  state.offset_x = 0.0f;
  state.offset_y = 0.0f;
  state.fill.id = main_id;
  state.slide.split_pos = 0.5f;
  state.slide.left_id = ids.empty() ? 0 : ids[0];
  state.slide.right_id = ids.size() > 1 ? ids[1] : state.slide.left_id;
  state.grid.cols = static_cast<int>(ids.size());
  state.grid.rows = 1;
  return state;
}

AVPixelFormat FindUnsupportedFormat(pl_gpu gpu) {
  const AVPixelFormat candidates[] = {AV_PIX_FMT_Y210LE, AV_PIX_FMT_BGR8, AV_PIX_FMT_RGB8, AV_PIX_FMT_XYZ12LE,
                                      AV_PIX_FMT_GBRAP16BE};
  for (AVPixelFormat format : candidates) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
    if (!desc || desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
      continue;
    }
    if (!pl_test_pixfmt(gpu, format)) {
      return format;
    }
  }
  return AV_PIX_FMT_NONE;
}

} // namespace

int main() {
  g_failures = 0;
  g_checks = 0;

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 2;
  }

  try {
    // The pixel comparators are part of the test harness, so mutation-test them
    // before any GPU work.
    TestComparators();

    // ---------------------------------------------------------------- context
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    Check(context.HasDevice(), "headless RenderContext created a Vulkan device");
    Check(context.gpu() != nullptr && context.vulkan() != nullptr, "context exposes gpu/vulkan");
    Check(context.log() != nullptr, "context exposes the placebo log");

    {
      bool rejected = false;
      try {
        context.CreateHeadlessDevice();
      } catch (const std::logic_error &) {
        rejected = true;
      }
      Check(rejected, "second device creation on one RenderContext is rejected");
    }
    {
      bool rejected = false;
      try {
        context.CreatePresentDevice(VK_NULL_HANDLE);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      Check(rejected, "CreatePresentDevice rejects a null surface");
    }

    pl_gpu gpu = context.gpu();
    EYEQ::FrameRenderer::Config fill_config;
    fill_config.mode = EYEQ::DisplayMode::Fill;
    fill_config.pixel_density = 1.0f;
    fill_config.enable_osd = false;

    // --------------------------------------------------- constructor failure
    {
      bool rejected = false;
      try {
        EYEQ::FrameRenderer::Config bad = fill_config;
        bad.mode = static_cast<EYEQ::DisplayMode>(99);
        EYEQ::FrameRenderer renderer(context, bad);
      } catch (const std::exception &) {
        rejected = true;
      }
      Check(rejected, "FrameRenderer rejects an unsupported display mode");
    }

    // Track the lifetime of the AVStream the frames reference; the renderer
    // must not outlive it.
    {
      EYEQ::FrameRenderer renderer(context, fill_config);
      Check(renderer.mode() == EYEQ::DisplayMode::Fill, "FrameRenderer keeps the requested mode");
    }

    // --------------------------- Fill spatial markers, crop, target resize
    {
      const int src_w = 64, src_h = 48;
      auto frame =
          MakeFrame(src_w, src_h, [&](int x, int y) { return QuadrantPixel(x, y, src_w, src_h); });
      frame->crop_left = 8;
      frame->crop_top = 4;
      frame->crop_right = 8;
      frame->crop_bottom = 4;

      AVFrame *raw = frame.get();
      AVPixelFormat format_before = static_cast<AVPixelFormat>(raw->format);
      uint8_t *data0_before = raw->data[0];
      int64_t pts_before = raw->pts;

      EYEQ::FrameRenderer renderer(context, fill_config);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = frame;
      auto failed = renderer.FeedFrames(frames);
      Check(failed.empty(), "valid Fill frame is accepted");

      const struct pl_frame *mapped = renderer.GetSourceFrame(0);
      Check(mapped != nullptr, "source frame is mapped");
      pl_rect2df crop_before = mapped->crop;
      pl_color_space color_before = mapped->color;
      int rotation_before = mapped->rotation;
      Check(crop_before.x0 == 8.0f && crop_before.y0 == 4.0f && crop_before.x1 == 56.0f && crop_before.y1 == 44.0f,
            "non-trivial AVFrame crop is carried into the pl_frame");

      const Color tl{1.0f, 0.0f, 0.0f};
      const Color tr{0.0f, 1.0f, 0.0f};
      const Color bl{0.0f, 0.0f, 1.0f};
      const Color br{1.0f, 1.0f, 1.0f};

      // The source is 64x48 with four quadrant colors and a non-trivial crop of
      // (8,4)-(56,44). Scale 1.5 fills a 96x72 target; each quadrant center must
      // land at the corresponding target quadrant, so a wrong crop/scale/offset
      // changes a checked pixel.
      auto target = CreateTarget(gpu, 96, 72, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto state = MakeBaseState({0}, 0);
      state.scale = 1.5f;
      renderer.Render(target.frame, state);
      std::vector<float> pixels = DownloadTarget(gpu, target);
      Check(Close(At(pixels, target.width, 24, 18), tl, 0.03f),
            "Fill 96x72: top-left quadrant is red, got " + ColorString(At(pixels, target.width, 24, 18)));
      Check(Close(At(pixels, target.width, 72, 18), tr, 0.03f),
            "Fill 96x72: top-right quadrant is green, got " + ColorString(At(pixels, target.width, 72, 18)));
      Check(Close(At(pixels, target.width, 24, 54), bl, 0.03f),
            "Fill 96x72: bottom-left quadrant is blue, got " + ColorString(At(pixels, target.width, 24, 54)));
      Check(Close(At(pixels, target.width, 72, 54), br, 0.03f),
            "Fill 96x72: bottom-right quadrant is white, got " + ColorString(At(pixels, target.width, 72, 54)));

      // Same renderer, different target size: the quadrant layout must follow the
      // target dimensions without re-instantiating the renderer.
      auto small = CreateTarget(gpu, 48, 36, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto small_state = MakeBaseState({0}, 0);
      small_state.scale = 0.75f;
      renderer.Render(small.frame, small_state);
      std::vector<float> small_pixels = DownloadTarget(gpu, small);
      Check(Close(At(small_pixels, small.width, 12, 9), tl, 0.03f),
            "target resize 48x36: top-left quadrant is red, got " + ColorString(At(small_pixels, small.width, 12, 9)));
      Check(Close(At(small_pixels, small.width, 36, 9), tr, 0.03f),
            "target resize 48x36: top-right quadrant is green, got " +
                ColorString(At(small_pixels, small.width, 36, 9)));
      Check(Close(At(small_pixels, small.width, 12, 27), bl, 0.03f),
            "target resize 48x36: bottom-left quadrant is blue, got " +
                ColorString(At(small_pixels, small.width, 12, 27)));
      Check(Close(At(small_pixels, small.width, 36, 27), br, 0.03f),
            "target resize 48x36: bottom-right quadrant is white, got " +
                ColorString(At(small_pixels, small.width, 36, 27)));

      // The source AVFrame, its mapped pl_frame properties and its crop must be
      // untouched by Feed/Render.
      Check(raw->format == format_before && raw->data[0] == data0_before && raw->pts == pts_before,
            "input AVFrame format/data/pts are unchanged");
      Check(mapped->crop.x0 == crop_before.x0 && mapped->crop.y0 == crop_before.y0 &&
                mapped->crop.x1 == crop_before.x1 && mapped->crop.y1 == crop_before.y1,
            "source pl_frame crop is unchanged after Fill render");
      Check(mapped->rotation == rotation_before && mapped->color.primaries == color_before.primaries &&
                mapped->color.transfer == color_before.transfer,
            "source pl_frame color/rotation are unchanged after Fill render");

      Check(DestroyTarget(gpu, small), "resize target texture destroyed");
      Check(DestroyTarget(gpu, target), "Fill target texture destroyed");
    }

    // ---------------------------------------------------------- Fill amplify
    {
      // Difference amplification is defined in the display-linear domain:
      // ref + gain * (test - ref), hard-clipped to the target range. The old
      // gamma/PQ-domain expectation (0.6 code at gain 2) is deliberately not
      // reproduced (V2.1 D1).
      auto dis = MakeFrame(32, 32, [](int, int) { return Color{0.4f, 0.4f, 0.4f}; });
      auto ref = MakeFrame(32, 32, [](int, int) { return Color{0.2f, 0.2f, 0.2f}; });
      EYEQ::FrameRenderer renderer(context, fill_config);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = dis;
      frames[1] = ref;
      renderer.FeedFrames(frames);

      auto target = CreateTarget(gpu, 64, 64, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto state = MakeBaseState({0, 1}, 0);
      state.scale = 2.0f;

      // Map the test source alone, then the reference alone, then amplify. The
      // linear values come from the same renderer, so the assertion is
      // independent of the exact transfer/tone-mapping details.
      int lw = 0, lh = 0;
      auto linear_center = [&](const EYEQ::DisplayState &s) {
        renderer.Render(target.frame, s);
        std::vector<float> linear = DownloadLinearResult(gpu, renderer, lw, lh);
        return At(linear, lw, lw / 2, lh / 2);
      };
      Color dis_linear = linear_center(state);
      auto ref_state = state;
      ref_state.show_ref = true;
      ref_state.ref_id = 1;
      Color ref_linear = linear_center(ref_state);
      auto amp_state = state;
      amp_state.ref_id = 1;
      amp_state.amplify = 2.0f;
      Color amp_linear = linear_center(amp_state);

      Color expected{ref_linear[0] + 2.0f * (dis_linear[0] - ref_linear[0]),
                     ref_linear[1] + 2.0f * (dis_linear[1] - ref_linear[1]),
                     ref_linear[2] + 2.0f * (dis_linear[2] - ref_linear[2])};
      Check(Close(amp_linear, expected, 0.01f), "Fill amplify = ref + gain*(test-ref) in display linear: got " +
                                                    ColorString(amp_linear) + " expected " + ColorString(expected));
      Check(amp_linear[0] > dis_linear[0] && dis_linear[0] > ref_linear[0],
            "linear amplify of a brighter test raises the value above the test");

      renderer.Render(target.frame, amp_state);
      std::vector<float> pixels = DownloadTarget(gpu, target);
      Color center = At(pixels, target.width, target.width / 2, target.height / 2);
      Check(!Close(center, Color{0.6f, 0.6f, 0.6f}, 0.02f),
            "the gamma-domain amplification result is not produced, got " + ColorString(center));
      Check(DestroyTarget(gpu, target), "amplify target texture destroyed");
    }

    // ----------------------------------------------------------------- Slide
    {
      auto left = MakeFrame(32, 16, [](int, int) { return Color{1.0f, 0.0f, 0.0f}; });
      auto right = MakeFrame(32, 16, [](int, int) { return Color{0.0f, 0.0f, 1.0f}; });
      EYEQ::FrameRenderer::Config config = fill_config;
      config.mode = EYEQ::DisplayMode::Slide;
      EYEQ::FrameRenderer renderer(context, config);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = left;
      frames[1] = right;
      renderer.FeedFrames(frames);

      auto target = CreateTarget(gpu, 96, 48, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto state = MakeBaseState({0, 1}, 0);
      state.scale = 3.0f;
      renderer.Render(target.frame, state);
      std::vector<float> pixels = DownloadTarget(gpu, target);
      Color l = At(pixels, target.width, target.width / 4, target.height / 2);
      Color r = At(pixels, target.width, target.width * 3 / 4, target.height / 2);
      Check(Close(l, Color{1.0f, 0.0f, 0.0f}, 0.03f), "Slide: left half is the left source, got " + ColorString(l));
      Check(Close(r, Color{0.0f, 0.0f, 1.0f}, 0.03f), "Slide: right half is the right source, got " + ColorString(r));
      Check(DestroyTarget(gpu, target), "slide target texture destroyed");
    }

    // ------------------------------------------------------------------ Grid
    {
      auto red = MakeFrame(32, 32, [](int, int) { return Color{1.0f, 0.0f, 0.0f}; });
      auto blue = MakeFrame(32, 32, [](int, int) { return Color{0.0f, 0.0f, 1.0f}; });
      red->crop_left = 4;
      red->crop_top = 4;
      red->crop_right = 4;
      red->crop_bottom = 4;

      EYEQ::FrameRenderer::Config config = fill_config;
      config.mode = EYEQ::DisplayMode::Grid;
      EYEQ::FrameRenderer renderer(context, config);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = red;
      frames[1] = blue;
      renderer.FeedFrames(frames);

      const struct pl_frame *mapped0 = renderer.GetSourceFrame(0);
      pl_rect2df crop_before = mapped0->crop;

      auto target = CreateTarget(gpu, 96, 48, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto state = MakeBaseState({0, 1}, 0);
      state.grid.cols = 2;
      state.grid.rows = 1;
      renderer.Render(target.frame, state);
      std::vector<float> pixels = DownloadTarget(gpu, target);
      Color c0 = At(pixels, target.width, target.width / 4, target.height / 2);
      Color c1 = At(pixels, target.width, target.width * 3 / 4, target.height / 2);
      Check(Close(c0, Color{1.0f, 0.0f, 0.0f}, 0.03f), "Grid: first cell shows its source, got " + ColorString(c0));
      Check(Close(c1, Color{0.0f, 0.0f, 1.0f}, 0.03f), "Grid: second cell shows its source, got " + ColorString(c1));

      mapped0 = renderer.GetSourceFrame(0);
      Check(mapped0->crop.x0 == crop_before.x0 && mapped0->crop.y0 == crop_before.y0 &&
                mapped0->crop.x1 == crop_before.x1 && mapped0->crop.y1 == crop_before.y1,
            "Grid does not modify the shared source crop");
      Check(DestroyTarget(gpu, target), "grid target texture destroyed");
    }

    // ------------------------------------------- two renderers, interleaved
    {
      auto source = MakeFrame(80, 45,
                              [](int x, int y) { return Color{static_cast<float>(x % 2), (y % 3) / 2.0f, 0.25f}; });

      auto targetA = CreateTarget(gpu, 160, 90, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto targetB = CreateTarget(gpu, 128, 96, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);

      // Renderer A lives across the destruction of renderer B, so the survival
      // claim below is about the same instance that produced the baseline.
      EYEQ::FrameRenderer renderer_a(context, fill_config);
      std::map<int, std::shared_ptr<AVFrame>> frames_a;
      frames_a[0] = source;
      renderer_a.SetMainSource(0);
      Check(renderer_a.FeedFrames(frames_a).empty(), "renderer A accepts the source");
      auto state_a = MakeBaseState({0}, 0);
      renderer_a.Render(targetA.frame, state_a);
      std::vector<float> a1 = DownloadTarget(gpu, targetA);

      {
        EYEQ::FrameRenderer renderer_b(context, fill_config);
        std::map<int, std::shared_ptr<AVFrame>> frames_b;
        frames_b[0] = source;
        renderer_b.SetMainSource(0);
        Check(renderer_b.FeedFrames(frames_b).empty(), "renderer B accepts the source");

        auto state_b = MakeBaseState({0}, 0);
        renderer_b.Render(targetB.frame, state_b);
        std::vector<float> b = DownloadTarget(gpu, targetB);

        // B has rendered and been read back while still alive, so this A
        // read-back really tests that B did not disturb A's state.
        renderer_a.Render(targetA.frame, state_a);
        std::vector<float> a2 = DownloadTarget(gpu, targetA);

        Check(AllClose(a1, a2, 1e-5f), "renderer A output is not affected by renderer B while both are alive");
        Check(b.size() == static_cast<std::size_t>(targetB.width) * targetB.height * 4,
              "renderer B produced a full PQ frame");
        bool b_finite = true;
        for (float value : b) {
          if (!std::isfinite(value)) {
            b_finite = false;
            break;
          }
        }
        Check(b_finite, "renderer B PQ output is finite");
        Check(source->data[0] != nullptr && source->format == AV_PIX_FMT_GBRPF32LE,
              "shared source frame is unchanged after two renderers used it");
      } // renderer B destroyed here

      renderer_a.Render(targetA.frame, state_a);
      std::vector<float> a3 = DownloadTarget(gpu, targetA);
      Check(AllClose(a1, a3, 1e-5f), "the same renderer A still renders identically after renderer B was destroyed");

      Check(DestroyTarget(gpu, targetA), "interleave target A destroyed");
      Check(DestroyTarget(gpu, targetB), "interleave target B destroyed");
    }

    // ------------------------------ first-construction failure must not leak
    {
      AVPixelFormat unsupported = FindUnsupportedFormat(gpu);
      if (unsupported == AV_PIX_FMT_NONE) {
        Skip("first-construction failure leak test: no unsupported software pixel format on this GPU");
      } else {
        auto bad = MakeRawFrame(32, 32, unsupported);
        int refs_before = av_buffer_get_ref_count(bad->buf[0]);
        {
          // A failing constructor must release the private AVFrame and the
          // pixel references it already acquired, even though ~VideoFrame does
          // not run for a throwing constructor.
          EYEQ::FrameRenderer renderer(context, fill_config);
          for (int i = 0; i < 8; ++i) {
            std::map<int, std::shared_ptr<AVFrame>> frames;
            frames[0] = bad;
            auto failed = renderer.FeedFrames(frames);
            Check(failed.size() == 1 && failed[0] == 0,
                  "first-construction failure is reported (attempt " + std::to_string(i) + ")");
          }
          Check(!renderer.HasFrames(), "no frame is retained after repeated first-construction failures");
        }
        int refs_after = av_buffer_get_ref_count(bad->buf[0]);
        Check(refs_after == refs_before, "repeated first-construction failures leak no pixel references (" +
                                             std::to_string(refs_before) + " -> " + std::to_string(refs_after) + ")");
      }
    }

    // ------------------------------------------ failed frame update recovery
    {
      auto good = MakeFrame(32, 32, [](int, int) { return Color{0.3f, 0.3f, 0.3f}; });
      int good_refs_before = av_buffer_get_ref_count(good->buf[0]);
      AVPixelFormat unsupported = FindUnsupportedFormat(gpu);

      {
        EYEQ::FrameRenderer renderer(context, fill_config);
        auto target = CreateTarget(gpu, 64, 64, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
        auto state = MakeBaseState({0}, 0);

        std::map<int, std::shared_ptr<AVFrame>> frames;
        frames[0] = good;
        Check(renderer.FeedFrames(frames).empty(), "valid frame fed before the failure");
        renderer.Render(target.frame, state);
        std::vector<float> baseline = DownloadTarget(gpu, target);

        if (unsupported == AV_PIX_FMT_NONE) {
          Skip("failed-frame-update test: no unsupported software pixel format on this GPU");
        } else {
          auto bad = MakeRawFrame(32, 32, unsupported);
          AVFrame *bad_raw = bad.get();
          AVPixelFormat bad_format_before = static_cast<AVPixelFormat>(bad_raw->format);
          uint8_t *bad_data0_before = bad_raw->data[0];

          std::map<int, std::shared_ptr<AVFrame>> bad_frames;
          bad_frames[0] = bad;
          auto failed = renderer.FeedFrames(bad_frames);
          Check(failed.size() == 1 && failed[0] == 0, "unsupported format is reported as a failed id");

          const struct pl_frame *still_mapped = renderer.GetSourceFrame(0);
          Check(still_mapped != nullptr && still_mapped->num_planes > 0 && still_mapped->planes[0].texture != nullptr,
                "previous mapping survives a failed update (no dangling view)");
          Check(bad_raw->format == bad_format_before && bad_raw->data[0] == bad_data0_before,
                "rejected input AVFrame is unchanged");

          // Render immediately after the failed update, before feeding a valid
          // frame again: the retained old mapping must still produce the image.
          renderer.Render(target.frame, state);
          std::vector<float> after_failure = DownloadTarget(gpu, target);
          Check(AllClose(baseline, after_failure, 1e-5f), "old image still renders after a failed update");

          std::map<int, std::shared_ptr<AVFrame>> good_frames;
          good_frames[0] = good;
          Check(renderer.FeedFrames(good_frames).empty(), "valid frame is accepted again after the failure");
          renderer.Render(target.frame, state);
          std::vector<float> recovered = DownloadTarget(gpu, target);
          Check(AllClose(baseline, recovered, 1e-5f), "rendering recovers to the baseline after a failed update");
        }
        Check(DestroyTarget(gpu, target), "failure-recovery target destroyed");
      }

      Check(av_buffer_get_ref_count(good->buf[0]) == good_refs_before,
            "no pixel references are leaked for the valid frame");
    }

    // -------------------------------------------------- target-local HDR nits
    {
      // A carries the default mastering peak, B does not. Feeding the same PQ frame to both
      // must not change B's mapped metadata: the override is target-local.
      EYEQ::FrameRenderer renderer_a(context, fill_config);
      EYEQ::FrameRenderer renderer_b(context, fill_config);
      renderer_a.SetDefaultMasteringNits(800.0);
      renderer_a.SetMainSource(0);
      renderer_b.SetMainSource(0);

      auto pq_frame = MakeFrame(32, 32, [](int, int) { return Color{0.5f, 0.5f, 0.5f}; });
      pq_frame->color_primaries = AVCOL_PRI_BT2020;
      pq_frame->color_trc = AVCOL_TRC_SMPTE2084;

      std::map<int, std::shared_ptr<AVFrame>> frames_a;
      std::map<int, std::shared_ptr<AVFrame>> frames_b;
      frames_a[0] = pq_frame;
      frames_b[0] = pq_frame;
      Check(renderer_a.FeedFrames(frames_a).empty(), "HDR frame accepted by renderer A");
      Check(renderer_b.FeedFrames(frames_b).empty(), "HDR frame accepted by renderer B");
      const struct pl_frame *mapped_a = renderer_a.GetSourceFrame(0);
      const struct pl_frame *mapped_b = renderer_b.GetSourceFrame(0);
      Check(mapped_a != nullptr && std::fabs(mapped_a->color.hdr.max_luma - 800.0f) < 0.5f,
            "the default mastering peak fills a missing max_luma on renderer A");
      Check(mapped_b != nullptr && std::fabs(mapped_b->color.hdr.max_luma - 800.0f) > 0.5f,
            "renderer B without a default keeps its own max_luma");

      auto sdr = MakeFrame(32, 32, [](int, int) { return Color{0.5f, 0.5f, 0.5f}; });
      std::map<int, std::shared_ptr<AVFrame>> sdr_frames;
      sdr_frames[0] = sdr;
      renderer_a.FeedFrames(sdr_frames);
      const struct pl_frame *mapped_sdr = renderer_a.GetSourceFrame(0);
      Check(mapped_sdr != nullptr && std::fabs(mapped_sdr->color.hdr.max_luma - 800.0f) > 0.5f,
            "the default mastering peak is not applied to SDR content");
    }

    // ------------------------------------------- PQ known luminance anchor
    {
      // Pass a known ST2084 code through to a PQ BT.2020 target: the PQ code for
      // 100 nits must survive so a wrong transfer/scale is a checked failure.
      const float pq_100 = PqCodeForNits(100.0f);
      Check(std::fabs(pq_100 - 0.5081f) < 0.001f, "ST2084 code for 100 nits matches the known value");

      auto pq_source = MakeFrame(32, 32, [&](int, int) { return Color{pq_100, pq_100, pq_100}; });
      pq_source->color_primaries = AVCOL_PRI_BT2020;
      pq_source->color_trc = AVCOL_TRC_SMPTE2084;

      EYEQ::FrameRenderer renderer(context, fill_config);
      // Give the mapping target the PQ source's own nominal peak so this anchor
      // isolates the mapping+encode round trip with no tone-map compression.
      EYEQ::MappingTarget mapping_target;
      mapping_target.primaries = PL_COLOR_PRIM_BT_2020;
      mapping_target.peak_nits = 10000.0f;
      mapping_target.black_nits = 0.0f;
      mapping_target.sdr_white_nits = 203.0f;
      renderer.SetMappingTarget(mapping_target, EYEQ::WorkPrecision::F32);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = pq_source;
      renderer.SetMainSource(0);
      renderer.FeedFrames(frames);

      auto target = CreateTarget(gpu, 64, 64, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
      auto state = MakeBaseState({0}, 0);
      state.scale = 2.0f;
      renderer.Render(target.frame, state);
      std::vector<float> pixels = DownloadTarget(gpu, target);
      Color center = At(pixels, target.width, target.width / 2, target.height / 2);
      Check(Close(center, Color{pq_100, pq_100, pq_100}, 0.02f),
            "PQ 100-nit code passes through to a PQ target, got " + ColorString(center));
      Check(DestroyTarget(gpu, target), "PQ anchor target destroyed");
    }

    // -------------------------------------------------------------------- OSD
    {
      // FontAvailable must pair its TTF_Init with TTF_Quit (SDL_ttf tracks a
      // reference count) and must not consume a caller-owned reference.
      {
        int before0 = TTF_WasInit();
        bool available = EYEQ::OsdManager::FontAvailable();
        int after0 = TTF_WasInit();
        Check(after0 == before0, "FontAvailable balances TTF_Init/TTF_Quit without caller ownership (" +
                                     std::to_string(before0) + " -> " + std::to_string(after0) + ")");
        Check(TTF_Init(), "caller can initialize SDL_ttf");
        int before1 = TTF_WasInit();
        bool available_with_owner = EYEQ::OsdManager::FontAvailable();
        int after1 = TTF_WasInit();
        Check(after1 == before1, "FontAvailable leaves an existing TTF init reference intact (" +
                                     std::to_string(before1) + " -> " + std::to_string(after1) + ")");
        Check(available == available_with_owner, "FontAvailable result does not depend on caller ownership");
        TTF_Quit();
      }

      auto source = MakeFrame(64, 48, [](int, int) { return Color{0.2f, 0.2f, 0.2f}; });
      auto target = CreateTarget(gpu, 96, 72, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto state = MakeBaseState({0}, 0);
      EYEQ::OsdInfo info;
      info.filename = "clip.mkv";
      info.video_id = 0;
      info.pict_type = 'I';
      info.current_time_s = 1.0f;
      info.frame_serial = 10;
      info.total_frames = 100;
      state.osd_infos[0] = info;
      state.total_duration_s = 10.0f;
      state.mouse_time_s = 2.0f;
      state.playback_progress = 0.2f;

      // OSD disabled: show_osd must be ignored and no manager exists.
      {
        EYEQ::FrameRenderer renderer(context, fill_config);
        Check(renderer.GetOsdManager() == nullptr, "OSD-disabled renderer has no OSD manager");
        std::map<int, std::shared_ptr<AVFrame>> frames;
        frames[0] = source;
        renderer.FeedFrames(frames);
        auto off = state;
        off.show_osd = false;
        renderer.Render(target.frame, off);
        std::vector<float> without = DownloadTarget(gpu, target);
        auto on = state;
        on.show_osd = true;
        renderer.Render(target.frame, on);
        std::vector<float> with = DownloadTarget(gpu, target);
        Check(AllClose(without, with, 1e-5f), "OSD-disabled renderer ignores show_osd");
      }

      // OSD enabled: font availability is an explicit environment precondition.
      // When it holds, failures from construction/Feed/Render/Download propagate
      // to the top-level handler and fail the test; nothing is swallowed here.
      if (!EYEQ::OsdManager::FontAvailable()) {
        Skip("OSD-enabled subtest: no usable system font");
      } else {
        EYEQ::FrameRenderer::Config config = fill_config;
        config.enable_osd = true;
        EYEQ::FrameRenderer renderer(context, config);
        Check(renderer.GetOsdManager() != nullptr, "OSD-enabled renderer creates an OSD manager");
        std::map<int, std::shared_ptr<AVFrame>> frames;
        frames[0] = source;
        Check(renderer.FeedFrames(frames).empty(), "OSD-enabled renderer accepts the source");
        auto off = state;
        off.show_osd = false;
        renderer.Render(target.frame, off);
        std::vector<float> without = DownloadTarget(gpu, target);
        auto on = state;
        on.show_osd = true;
        renderer.Render(target.frame, on);
        std::vector<float> with = DownloadTarget(gpu, target);
        Check(!AllClose(without, with, 1e-5f), "OSD-enabled renderer changes the image when show_osd is set");

        // Regression: the desktop presentation target (the swapchain image) is
        // wrapped with COLOR_ATTACHMENT | TRANSFER_DST only, so it is never
        // sampleable. The OSD blend must stage the encoded result instead of
        // sampling the target: libplacebo only logs the failed validation (it
        // does not fail the dispatch), so the old code silently copied the
        // stale scratch over the frame and turned the screen pink.
        //
        // A dedicated renderer is used, and the non-sampleable target is drawn
        // first, so a stale blend from an earlier subtest cannot mask the bug.
        {
          EYEQ::FrameRenderer::Config nonsampleable_config = fill_config;
          nonsampleable_config.enable_osd = true;
          EYEQ::FrameRenderer nonsampleable_renderer(context, nonsampleable_config);
          std::map<int, std::shared_ptr<AVFrame>> osd_frames;
          osd_frames[0] = source;
          Check(nonsampleable_renderer.FeedFrames(osd_frames).empty(), "OSD regression source accepted");
          auto osd_on = state;
          osd_on.show_osd = true;

          auto nonsampleable_target =
              CreateTarget(gpu, 96, 72, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886, /*sampleable=*/false);
          Check(!nonsampleable_target.frame.planes[0].texture->params.sampleable,
                "swapchain-like target is not sampleable");
          nonsampleable_renderer.Render(nonsampleable_target.frame, osd_on);
          std::vector<float> nonsampleable_out = DownloadTarget(gpu, nonsampleable_target);

          auto sampleable_target = CreateTarget(gpu, 96, 72, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
          nonsampleable_renderer.Render(sampleable_target.frame, osd_on);
          std::vector<float> sampleable_out = DownloadTarget(gpu, sampleable_target);

          Check(AllClose(sampleable_out, nonsampleable_out, 1e-5f),
                "OSD text blends identically onto a non-sampleable target");
          Check(DestroyTarget(gpu, sampleable_target), "sampleable OSD target destroyed");
          Check(DestroyTarget(gpu, nonsampleable_target), "non-sampleable OSD target destroyed");
        }
      }
      Check(DestroyTarget(gpu, target), "OSD target destroyed");
    }
    // ------------------------------- SDR/HDR mixed comparison
    {
      // One SDR and one HDR source are mapped to the same display target and
      // composed. The SDR half must be identical to its SDR-only mapping (no
      // cross-contamination), and the HDR half must be finite.
      auto sdr = MakeFrame(32, 16, [](int, int) { return Color{0.5f, 0.5f, 0.5f}; });
      const float pq_100 = PqCodeForNits(100.0f);
      auto hdr = MakeFrame(32, 16, [&](int, int) { return Color{pq_100, pq_100, pq_100}; });
      hdr->color_primaries = AVCOL_PRI_BT2020;
      hdr->color_trc = AVCOL_TRC_SMPTE2084;

      EYEQ::FrameRenderer::Config config = fill_config;
      config.mode = EYEQ::DisplayMode::Slide;
      EYEQ::FrameRenderer renderer(context, config);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = sdr;
      frames[1] = hdr;
      renderer.FeedFrames(frames);

      auto target = CreateTarget(gpu, 96, 48, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
      auto mixed = MakeBaseState({0, 1}, 0);
      mixed.scale = 3.0f;
      renderer.Render(target.frame, mixed);
      int mw = 0, mh = 0;
      std::vector<float> mixed_linear = DownloadLinearResult(gpu, renderer, mw, mh);
      Color sdr_half = At(mixed_linear, mw, mw / 4, mh / 2);
      Color hdr_half = At(mixed_linear, mw, mw * 3 / 4, mh / 2);

      // SDR-only mapping through the same renderer and target.
      EYEQ::FrameRenderer sdr_only(context, config);
      std::map<int, std::shared_ptr<AVFrame>> sdr_frames;
      sdr_frames[0] = sdr;
      sdr_only.FeedFrames(sdr_frames);
      auto sdr_only_state = MakeBaseState({0}, 0);
      sdr_only_state.scale = 3.0f;
      sdr_only_state.fill.id = 0;
      sdr_only.Render(target.frame, sdr_only_state);
      int sw = 0, sh = 0;
      std::vector<float> sdr_linear = DownloadLinearResult(gpu, sdr_only, sw, sh);
      // Sample away from the centre so the comparison split line is not read.
      Color sdr_reference = At(sdr_linear, sw, sw / 4, sh / 2);

      Check(Close(sdr_half, sdr_reference, 0.01f), "the SDR half of an SDR/HDR mix equals its SDR-only mapping: got " +
                                                       ColorString(sdr_half) + " expected " +
                                                       ColorString(sdr_reference));
      Check(std::isfinite(hdr_half[0]) && std::isfinite(hdr_half[1]) && std::isfinite(hdr_half[2]),
            "the HDR half of the mix maps to a finite display-linear value");
      Check(DestroyTarget(gpu, target), "SDR/HDR mix target destroyed");
    }

    // ------------------------------- display-linear precision pair (16F/32F)
    {
      // The same scene is rendered with the same mapping target but a different
      // work precision. The 32F reference is the correctness baseline; the 16F
      // candidate must stay within an explicit linear error bound.
      auto source = MakeFrame(64, 48, [](int x, int) {
        const float base = static_cast<float>(x) / 63.0f;
        return Color{base * 0.02f, base * 0.5f, 2.0f + (x == 32 ? 1e-4f : 0.0f)};
      });
      auto target = CreateTarget(gpu, 128, 96, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
      auto state = MakeBaseState({0}, 0);
      state.scale = 2.0f;

      EYEQ::MappingTarget mt;
      mt.primaries = PL_COLOR_PRIM_BT_709;
      mt.peak_nits = 1000.0f;
      mt.black_nits = 0.0f;
      mt.sdr_white_nits = 203.0f;

      auto render_at = [&](EYEQ::WorkPrecision precision) {
        EYEQ::FrameRenderer renderer(context, fill_config);
        renderer.SetMappingTarget(mt, precision);
        std::map<int, std::shared_ptr<AVFrame>> frames;
        frames[0] = source;
        renderer.FeedFrames(frames);
        renderer.Render(target.frame, state);
        int w = 0, h = 0;
        return DownloadLinearResult(gpu, renderer, w, h);
      };
      std::vector<float> ref32 = render_at(EYEQ::WorkPrecision::F32);
      std::vector<float> cand16 = render_at(EYEQ::WorkPrecision::F16);
      float max_err = 0.0f;
      for (std::size_t i = 0; i < ref32.size(); ++i) {
        max_err = std::max(max_err, std::fabs(ref32[i] - cand16[i]));
      }
      std::fprintf(stdout, "  linear work 32F vs 16F max |diff| = %s\n", Fmt(max_err).c_str());
      Check(max_err <= 0.01f, "16F work texture stays within 1e-2 of the 32F reference (max=" + Fmt(max_err) + ")");
      bool finite = true;
      for (float value : ref32) {
        if (!std::isfinite(value)) {
          finite = false;
          break;
        }
      }
      Check(finite, "the 32F linear reference is finite");

      // Performance data, reported separately from correctness. This is the
      // host-side wall clock of the Render() submit path (dispatch + submit),
      // NOT GPU execution time: no GPU timer is used, so queue/execute time is
      // not separated here. Memory is the one target-sized RGBA work texture.
      auto host_submit_ms = [&](EYEQ::WorkPrecision precision, int iterations) {
        EYEQ::FrameRenderer renderer(context, fill_config);
        renderer.SetMappingTarget(mt, precision);
        std::map<int, std::shared_ptr<AVFrame>> frames;
        frames[0] = source;
        renderer.FeedFrames(frames);
        renderer.Render(target.frame, state);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
          renderer.Render(target.frame, state);
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        return static_cast<double>(elapsed) / iterations / 1000.0;
      };
      const double submit_ms_16 = host_submit_ms(EYEQ::WorkPrecision::F16, 20);
      const double submit_ms_32 = host_submit_ms(EYEQ::WorkPrecision::F32, 20);
      const std::size_t texels = static_cast<std::size_t>(target.width) * target.height;
      std::fprintf(stdout, "  host Render() submit ms/iter: 16F=%.3f 32F=%.3f; work tex bytes: 16F=%zu 32F=%zu\n",
                   submit_ms_16, submit_ms_32, texels * 8, texels * 16);

      Check(DestroyTarget(gpu, target), "precision-pair target destroyed");
    }

    // --------------------------- public result excludes the text OSD
    {
      if (!EYEQ::OsdManager::FontAvailable()) {
        Skip("public-result OSD exclusion: no usable system font");
      } else {
        EYEQ::FrameRenderer::Config config = fill_config;
        config.enable_osd = true;
        EYEQ::FrameRenderer renderer(context, config);
        auto source = MakeFrame(64, 48, [](int, int) { return Color{0.3f, 0.4f, 0.5f}; });
        std::map<int, std::shared_ptr<AVFrame>> frames;
        frames[0] = source;
        renderer.FeedFrames(frames);
        auto target = CreateTarget(gpu, 128, 96, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
        auto state = MakeBaseState({0}, 0);
        EYEQ::OsdInfo info;
        info.filename = "clip.mkv";
        info.video_id = 0;
        info.pict_type = 'I';
        info.current_time_s = 1.0f;
        info.frame_serial = 10;
        info.total_frames = 100;
        state.osd_infos[0] = info;
        state.total_duration_s = 10.0f;

        int off_w = 0, off_h = 0;
        state.show_osd = false;
        renderer.Render(target.frame, state);
        std::vector<float> linear_off = DownloadLinearResult(gpu, renderer, off_w, off_h);
        std::vector<float> target_off = DownloadTarget(gpu, target);

        int on_w = 0, on_h = 0;
        state.show_osd = true;
        renderer.Render(target.frame, state);
        std::vector<float> linear_on = DownloadLinearResult(gpu, renderer, on_w, on_h);
        std::vector<float> target_on = DownloadTarget(gpu, target);

        Check(off_w == on_w && off_h == on_h && AllClose(linear_off, linear_on, 0.0f),
              "the display-linear public result is identical with and without the text OSD");
        Check(!AllClose(target_off, target_on, 1e-5f), "the desktop target does change when the text OSD is shown");
        Check(DestroyTarget(gpu, target), "OSD-exclusion target destroyed");
      }
    }

    // -------------------- linear amplify: 100/110 nits x10 -> 200 nits
    {
      const float pq_100 = PqCodeForNits(100.0f);
      const float pq_110 = PqCodeForNits(110.0f);
      auto dis = MakeFrame(32, 32, [&](int, int) { return Color{pq_110, pq_110, pq_110}; });
      auto ref = MakeFrame(32, 32, [&](int, int) { return Color{pq_100, pq_100, pq_100}; });
      dis->color_primaries = AVCOL_PRI_BT2020;
      dis->color_trc = AVCOL_TRC_SMPTE2084;
      ref->color_primaries = AVCOL_PRI_BT2020;
      ref->color_trc = AVCOL_TRC_SMPTE2084;

      EYEQ::FrameRenderer renderer(context, fill_config);
      EYEQ::MappingTarget mt;
      mt.primaries = PL_COLOR_PRIM_BT_2020;
      mt.peak_nits = 10000.0f;
      mt.black_nits = 0.0f;
      mt.sdr_white_nits = 203.0f;
      renderer.SetMappingTarget(mt, EYEQ::WorkPrecision::F32);
      std::map<int, std::shared_ptr<AVFrame>> frames;
      frames[0] = dis;
      frames[1] = ref;
      renderer.FeedFrames(frames);

      auto target = CreateTarget(gpu, 64, 64, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
      auto state = MakeBaseState({0, 1}, 0);
      state.scale = 2.0f;
      state.ref_id = 1;
      state.amplify = 10.0f;
      renderer.Render(target.frame, state);
      int w = 0, h = 0;
      std::vector<float> linear = DownloadLinearResult(gpu, renderer, w, h);
      const Color center = At(linear, w, w / 2, h / 2);
      const float expected_norm = 200.0f / 203.0f;
      std::fprintf(stdout, "  100/110 nits x10 linear center = %s (expected norm %s)\n", ColorString(center).c_str(),
                   Fmt(expected_norm).c_str());
      Check(Close(center, Color{expected_norm, expected_norm, expected_norm}, 0.01f),
            "100/110 nits x10 amplifies to the 200-nit display-linear value");
      Check(renderer.GetDisPrecision() == EYEQ::WorkPrecision::F32 &&
                renderer.GetRefPrecision() == EYEQ::WorkPrecision::F32,
            "the amplify source/reference work textures are 32F before the subtraction");

      // The old PQ-domain amplification would give ~253.5 nits; the linear path
      // differs by design (V2.1 D1) and the two must not be conflated.
      const float pq_domain_nits = EYEQ::PqDecodeNits(pq_100 + 10.0f * (pq_110 - pq_100));
      std::fprintf(stdout, "  PQ-domain equivalent = %s nits\n", Fmt(pq_domain_nits).c_str());
      Check(std::fabs(pq_domain_nits - 253.5f) < 3.0f, "the old PQ-domain amplification gives ~253.5 nits");
      Check(std::fabs(200.0f - pq_domain_nits) > 40.0f, "linear and PQ-domain amplification differ as expected");
      Check(DestroyTarget(gpu, target), "100/110-nit target destroyed");
    }

  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  SDL_Quit();

  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
