// Present-target contract tests.
//
// The desktop presenter does not hand FrameRenderer a plain offscreen texture:
// libplacebo wraps the swapchain image with VK_IMAGE_USAGE_COLOR_ATTACHMENT |
// VK_IMAGE_USAGE_TRANSFER_DST only, so the wrapped pl_tex has sampleable ==
// false. Any pass that binds that texture as a sampled source fails libplacebo
// parameter validation, but pl_pass_run() only logs the failure (pl_require)
// and pl_dispatch_finish() still reports success. The renderer therefore keeps
// running and writes garbage (the "OSD turns the screen pink" bug).
//
// A swapchain cannot be created headlessly, so these tests mirror the wrap
// parameters instead (sampleable=false, renderable/blit_dst=true) and drive
// every presentation path against both that target and an equivalent
// sampleable offscreen target. Two properties are asserted per case:
//   1. No libplacebo error is logged while rendering (GpuErrorTrap). This is
//      the generic guard: a validation failure is otherwise invisible.
//   2. The non-sampleable target produces the same pixels as the sampleable
//      one, which catches corruption that a driver might not even log.
//
// The test intentionally reuses the production display sources (see
// tests/display/CMakeLists.txt) so the resource boundaries under test are the
// ones the player uses.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

// An environment precondition that is reported separately and never counted as
// a passing check.
void Skip(const std::string &what) {
  ++g_skipped;
  std::fprintf(stderr, "  SKIP %s\n", what.c_str());
}

/*
 * Records every libplacebo message at PL_LOG_ERR or above while it is
 * installed. This is the safety net for the whole bug class: libplacebo's
 * pl_require() prints "Validation failed: ..." and aborts the individual pass,
 * but the public dispatch/render calls still return success. Without a trap the
 * renderer writes garbage and every pixel assertion downstream is only a
 * coincidence away from passing.
 *
 * The trap must outlive the RenderContext it was installed on (the log's
 * callback points at it).
 */
class GpuErrorTrap {
public:
  void Install(pl_log log) {
    log_ = log;
    struct pl_log_params params = {
        .log_cb = &GpuErrorTrap::Trampoline,
        .log_priv = this,
        .log_level = PL_LOG_ERR,
    };
    pl_log_update(log_, &params);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
  }

  int Errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(messages_.size());
  }

  // Renders the accumulated messages into one line for the failure detail.
  std::string Summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const std::string &message : messages_) {
      out += message;
      out += " | ";
    }
    return out;
  }

private:
  static void Trampoline(void *priv, enum pl_log_level level, const char *msg) {
    static_cast<GpuErrorTrap *>(priv)->OnMessage(level, msg);
  }

  void OnMessage(enum pl_log_level level, const char *msg) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      messages_.emplace_back(msg ? msg : "(null)");
    }
    // Keep printing so a failure is diagnosable from the test log.
    pl_log_color(nullptr, level, msg);
  }

  pl_log log_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<std::string> messages_;
};

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

struct Target {
  pl_tex tex = nullptr;
  struct pl_frame frame{};
  int width = 0;
  int height = 0;
};

/*
 * @p sampleable mirrors the wrapped swapchain image (false) or a plain
 * offscreen target (true). blit_dst is set because the swapchain is requested
 * with TRANSFER_DST; host_readable is a test-only extra so the result can be
 * verified, and it does not affect the sampleability under test.
 */
Target CreateTarget(pl_gpu gpu, int width, int height, enum pl_color_primaries primaries,
                    enum pl_color_transfer transfer, bool sampleable) {
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
      .blit_dst = true,
      .host_readable = true,
  };
  Target target;
  target.tex = pl_tex_create(gpu, &params);
  if (!target.tex) {
    throw std::runtime_error("present target texture creation failed");
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

void DestroyTarget(pl_gpu gpu, Target &target) {
  if (target.tex) {
    pl_tex_destroy(gpu, &target.tex);
  }
}

std::vector<float> DownloadTarget(pl_gpu gpu, const Target &target) {
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

bool AllClose(const std::vector<float> &a, const std::vector<float> &b, float tolerance) {
  if (!std::isfinite(tolerance) || tolerance < 0.0f || a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > tolerance) {
      return false;
    }
  }
  return true;
}

bool AllFinite(const std::vector<float> &pixels) {
  for (float value : pixels) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

EYEQ::DisplayState MakeBaseState(const std::vector<int> &ids, int main_id) {
  EYEQ::DisplayState state{};
  state.ids = ids;
  state.scale = 1.0f;
  state.fill.id = main_id;
  state.slide.split_pos = 0.5f;
  state.slide.left_id = ids.empty() ? 0 : ids[0];
  state.slide.right_id = ids.size() > 1 ? ids[1] : state.slide.left_id;
  state.grid.cols = static_cast<int>(ids.size());
  state.grid.rows = 1;
  return state;
}

struct PresentCase {
  std::string name;
  EYEQ::FrameRenderer::Config config;
  std::vector<int> ids;
  int main_id = 0;
  enum pl_color_primaries primaries = PL_COLOR_PRIM_BT_709;
  enum pl_color_transfer transfer = PL_COLOR_TRC_BT_1886;
  // Sources to feed. The reference switch and the amplification need an extra
  // source that is not part of the grid.
  const std::map<int, std::shared_ptr<AVFrame>> *frames = nullptr;
  EYEQ::DisplayState state_modifier{};
  bool has_state_modifier = false;
  bool use_preview = false;
};

// One presentation path, run twice: once into a sampleable offscreen target and
// once into a target with the wrapped-swapchain capabilities. Both must render
// without a libplacebo error and must agree pixel for pixel.
void RunPresentParity(const PresentCase &test_case, EYEQ::RenderContext &context, pl_gpu gpu, int width, int height,
                      const std::map<int, std::shared_ptr<AVFrame>> &frames, GpuErrorTrap &trap) {
  EYEQ::FrameRenderer renderer(context, test_case.config);
  renderer.FeedFrames(frames);

  EYEQ::DisplayState state = MakeBaseState(test_case.ids, test_case.main_id);
  if (test_case.has_state_modifier) {
    state = test_case.state_modifier;
  }

  Target sampleable = CreateTarget(gpu, width, height, test_case.primaries, test_case.transfer, /*sampleable=*/true);
  Target swapchain = CreateTarget(gpu, width, height, test_case.primaries, test_case.transfer, /*sampleable=*/false);
  if (!(swapchain.tex->params.sampleable == false && sampleable.tex->params.sampleable == true)) {
    throw std::runtime_error("present target capability mirror is broken");
  }

  const EYEQ::EncodeRect preview =
      test_case.use_preview ? EYEQ::EncodeRect{0.1f, 0.1f, 0.8f, 0.8f} : EYEQ::EncodeRect{};

  auto render_into = [&](Target &target) {
    if (test_case.use_preview) {
      // The letterboxed desktop path: compose at the canvas size, then present
      // into the normalized preview rectangle.
      renderer.RenderScene(target.width, target.height, state);
      renderer.PresentToTarget(target.frame, state, preview);
    } else {
      renderer.Render(target.frame, state);
    }
  };

  // The non-sampleable target is rendered first: a fresh renderer's staging
  // texture is then uninitialized, so a silently skipped OSD blend cannot be
  // masked by a correct blend left over from the sampleable run.
  trap.Reset();
  render_into(swapchain);
  const int swapchain_errors = trap.Errors();
  const std::string swapchain_detail = trap.Summary();

  trap.Reset();
  render_into(sampleable);
  const int sampleable_errors = trap.Errors();
  const std::string sampleable_detail = trap.Summary();

  Check(sampleable_errors == 0, test_case.name + ": sampleable target logs no placebo error" +
                                    (sampleable_errors ? std::string(" [") + sampleable_detail + "]" : ""));
  Check(swapchain_errors == 0, test_case.name + ": swapchain-like target logs no placebo error" +
                                   (swapchain_errors ? std::string(" [") + swapchain_detail + "]" : ""));

  std::vector<float> sampleable_out = DownloadTarget(gpu, sampleable);
  std::vector<float> swapchain_out = DownloadTarget(gpu, swapchain);
  Check(AllFinite(sampleable_out) && AllFinite(swapchain_out), test_case.name + ": both targets stay finite");
  Check(AllClose(sampleable_out, swapchain_out, 1e-6f),
        test_case.name + ": non-sampleable target matches the sampleable one");

  DestroyTarget(gpu, sampleable);
  DestroyTarget(gpu, swapchain);
}

} // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 2;
  }

  // Declared before the context so the callback target outlives the log.
  GpuErrorTrap trap;

  try {
    EYEQ::RenderContext context(EYEQ::LoggingLevel::WARN, nullptr, nullptr, 0);
    context.CreateHeadlessDevice();
    Check(context.HasDevice(), "headless RenderContext created a Vulkan device");
    pl_gpu gpu = context.gpu();

    // Installed after device creation so the device-probing messages are not
    // mistaken for render errors.
    trap.Install(context.log());

    const int width = 96;
    const int height = 72;
    const float pq_100 = EYEQ::PqEncodeNits(100.0f);

    auto sdr_frame = MakeFrame(64, 48, [](int x, int y) {
      const float v = 0.1f + 0.6f * (static_cast<float>(x + y) / 110.0f);
      return Color{v, 0.5f * v + 0.2f, 1.0f - v};
    });
    auto hdr_frame = MakeFrame(64, 48, [&](int x, int y) {
      const float v = pq_100 * (0.5f + 0.5f * static_cast<float>(x) / 64.0f);
      (void)y;
      return Color{v, v, v};
    });
    hdr_frame->color_primaries = AVCOL_PRI_BT2020;
    hdr_frame->color_trc = AVCOL_TRC_SMPTE2084;

    auto sdr_ref = MakeFrame(64, 48, [](int x, int y) {
      const float v = 0.2f + 0.4f * (static_cast<float>(x) / 64.0f);
      (void)y;
      return Color{v, v, v};
    });

    std::map<int, std::shared_ptr<AVFrame>> sdr_frames;
    sdr_frames[0] = sdr_frame;
    std::map<int, std::shared_ptr<AVFrame>> hdr_frames;
    hdr_frames[0] = hdr_frame;
    std::map<int, std::shared_ptr<AVFrame>> slide_frames;
    slide_frames[0] = sdr_frame;
    slide_frames[1] = hdr_frame;
    std::map<int, std::shared_ptr<AVFrame>> grid_frames;
    grid_frames[0] = sdr_frame;
    grid_frames[1] = sdr_frame;
    grid_frames[2] = sdr_frame;
    std::map<int, std::shared_ptr<AVFrame>> amplify_frames;
    amplify_frames[0] = sdr_frame;
    amplify_frames[1] = sdr_ref;

    EYEQ::FrameRenderer::Config base;
    base.pixel_density = 1.0f;
    base.enable_osd = false;

    std::vector<PresentCase> cases;

    // Fill, SDR target: the explicit OutputEncoder path.
    {
      PresentCase c;
      c.name = "fill/SDR-BT1886";
      c.frames = &sdr_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Fill;
      c.ids = {0};
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_BT_1886;
      cases.push_back(c);
    }
    // Fill, HDR PQ target: the encode path with a PQ wire transfer.
    {
      PresentCase c;
      c.name = "fill/HDR-PQ";
      c.frames = &hdr_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Fill;
      c.ids = {0};
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_PQ;
      cases.push_back(c);
    }
    // Fill, sRGB target: the color-managed FillRender::PresentTo path.
    {
      PresentCase c;
      c.name = "fill/SDR-sRGB";
      c.frames = &sdr_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Fill;
      c.ids = {0};
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_SRGB;
      cases.push_back(c);
    }
    // Letterboxed desktop preview with an HDR PQ target.
    {
      PresentCase c;
      c.name = "fill/HDR-PQ letterboxed";
      c.frames = &hdr_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Fill;
      c.ids = {0};
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_PQ;
      c.use_preview = true;
      cases.push_back(c);
    }
    // Slide: two sources of different transfer into one target.
    {
      PresentCase c;
      c.name = "slide/SDR+HDR";
      c.frames = &slide_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Slide;
      c.ids = {0, 1};
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_PQ;
      cases.push_back(c);
    }
    // Grid: three cells.
    {
      PresentCase c;
      c.name = "grid/SDR";
      c.frames = &grid_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Grid;
      c.ids = {0, 1, 2};
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_BT_1886;
      cases.push_back(c);
    }
    // Amplified difference against a reference: the F32 work-texture path.
    {
      PresentCase c;
      c.name = "fill/SDR amplify";
      c.frames = &amplify_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Fill;
      c.ids = {0, 1};
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_BT_1886;
      c.state_modifier = MakeBaseState({0, 1}, 0);
      c.state_modifier.ref_id = 1;
      c.state_modifier.amplify = 10.0f;
      c.has_state_modifier = true;
      cases.push_back(c);
    }

    for (const PresentCase &c : cases) {
      RunPresentParity(c, context, gpu, width, height, *c.frames, trap);
    }

    // OSD: the actual bug. Font availability is an explicit environment
    // precondition; without a font the OSD path cannot be exercised.
    if (!EYEQ::OsdManager::FontAvailable()) {
      Skip("OSD present-target case: no usable system font");
    } else {
      PresentCase c;
      c.name = "fill/HDR-PQ + OSD";
      c.frames = &hdr_frames;
      c.config = base;
      c.config.mode = EYEQ::DisplayMode::Fill;
      c.config.enable_osd = true;
      c.ids = {0};
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_PQ;
      c.state_modifier = MakeBaseState({0}, 0);
      EYEQ::OsdInfo info;
      info.filename = "clip.mkv";
      info.video_id = 0;
      info.pict_type = 'I';
      info.current_time_s = 1.0f;
      info.frame_serial = 10;
      info.total_frames = 100;
      c.state_modifier.osd_infos[0] = info;
      c.state_modifier.total_duration_s = 10.0f;
      c.state_modifier.mouse_time_s = 2.0f;
      c.state_modifier.playback_progress = 0.2f;
      c.state_modifier.show_osd = true;
      c.has_state_modifier = true;
      RunPresentParity(c, context, gpu, width, height, hdr_frames, trap);
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    ++g_failures;
  }

  SDL_Quit();

  std::fprintf(stdout, "\n%d checks, %d failures, %d skipped subtests\n", g_checks, g_failures, g_skipped);
  return g_failures == 0 ? 0 : 1;
}
