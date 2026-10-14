#include "display/display_coordinator.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <spdlog/fmt/fmt.h>

#include "log.hpp"

using namespace EYEQ;

namespace {

// Small integer mixer for the scene key; the exact hash does not matter as long
// as equal inputs give equal keys and any changed input changes the key.
uint64_t HashMix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t Quantize(float value) {
  // Quantize so that tiny floating-point noise from UI arithmetic does not look
  // like a new scene, while any visible change does.
  return static_cast<uint64_t>(static_cast<int64_t>(std::llround(static_cast<double>(value) * 1024.0)));
}

} // namespace

DisplayCoordinator::DisplayCoordinator(RenderContext &context, const Config &config, ColorspaceHintFunc presenter_hint)
    : ui_thread_id_(std::this_thread::get_id()), presenter_hint_(std::move(presenter_hint)),
      mapping_target_(config.mapping_target), work_precision_(config.work_precision), monitoring_(config.monitoring),
      canvas_w_(config.canvas_w), canvas_h_(config.canvas_h),
      validate_difference_result_(config.validate_difference_result) {
  ValidateMappingTarget(mapping_target_);

  FrameRenderer::Config renderer_config;
  renderer_config.mode = config.mode;
  renderer_config.scale_method = config.scale_method;
  renderer_config.plane_scale_method = config.plane_scale_method;
  renderer_config.pixel_density = config.pixel_density;
  renderer_config.enable_osd = config.enable_osd;
  renderer_config.target_kind = config.target_kind;
  renderer_config.tone_mapping = config.tone_mapping;

  renderer_ = std::make_unique<FrameRenderer>(context, renderer_config, presenter_hint_);
  renderer_->SetMappingTarget(mapping_target_, work_precision_);
  renderer_->SetValidateLinearResult(false);
  renderer_->SetHdrVividEnabled(config.hdrvivid);
}

DisplayCoordinator::~DisplayCoordinator() = default;

void DisplayCoordinator::SetMainSource(int id) {
  AssertUiThread();
  renderer_->SetMainSource(id);
}

void DisplayCoordinator::SetSdrWhiteOnHdr(float nits) {
  AssertUiThread();
  renderer_->SetSdrWhiteOnHdr(nits);
}

void DisplayCoordinator::SetDefaultMasteringNits(double nits) {
  AssertUiThread();
  // Source-only completion: an explicit mapping target is not re-applied.
  renderer_->SetDefaultMasteringNits(nits);
  InvalidateScene();
}

void DisplayCoordinator::SetHdrVividEnabled(bool enabled) {
  AssertUiThread();
  if (renderer_->IsHdrVividEnabled() == enabled) {
    return;
  }
  renderer_->SetHdrVividEnabled(enabled);
  InvalidateScene();
}

void DisplayCoordinator::SetMappingTarget(const MappingTarget &target) {
  AssertUiThread();
  ValidateMappingTarget(target);
  mapping_target_ = target;
  renderer_->SetMappingTarget(mapping_target_, work_precision_);
  InvalidateScene();
}

void DisplayCoordinator::ReconfigurePrecision(WorkPrecision precision) {
  AssertUiThread();
  ApplyPrecision(precision);
}

void DisplayCoordinator::AssertUiThread() const {
#ifndef NDEBUG
  assert(ui_thread_id_ == std::this_thread::get_id() &&
         "DisplayCoordinator must be used from the UI thread that constructed it");
#endif
}

void DisplayCoordinator::ApplyPrecision(WorkPrecision precision) {
  if (precision == work_precision_) {
    return;
  }
  work_precision_ = precision;
  renderer_->SetMappingTarget(mapping_target_, work_precision_);
  ++precision_reconfigure_count_;
  Logger->info("Work precision reconfigured to {}", ToString(work_precision_));
  InvalidateScene();
}

void DisplayCoordinator::InvalidateScene() {
  scene_key_valid_ = false;
  result_ = {};
}

void DisplayCoordinator::SetCanvasSize(int width, int height) {
  AssertUiThread();
  if (width <= 0 || height <= 0) {
    throw std::invalid_argument(fmt::format("DisplayCoordinator::SetCanvasSize: invalid size {}x{}", width, height));
  }
  if (canvas_w_ == width && canvas_h_ == height) {
    return;
  }
  canvas_w_ = width;
  canvas_h_ = height;
  InvalidateScene();
}

CanvasLayout DisplayCoordinator::canvas_layout(int window_w, int window_h) const {
  AssertUiThread();
  return ComputeCanvasLayout(canvas_w_, canvas_h_, window_w, window_h, 1.0f);
}

std::vector<int> DisplayCoordinator::FeedFrames(const std::map<int, std::shared_ptr<AVFrame>> &frames) {
  AssertUiThread();
  std::vector<int> failed = renderer_->FeedFrames(frames);
  // Without a mapping target the renderer resolves it on the first feed; keep this
  // layer's copy (scene key, result metadata, precision changes) in sync.
  if (renderer_->mapping_target().peak_nits != mapping_target_.peak_nits) {
    mapping_target_ = renderer_->mapping_target();
    InvalidateScene();
  }
  if (failed.empty()) {
    ++feed_count_;
    ++feed_generation_;
  }
  return failed;
}

uint64_t DisplayCoordinator::ComputeSceneKey(const DisplayState &state) const {
  uint64_t h = 0xcbf29ce484222325ULL;
  h = HashMix(h, static_cast<uint64_t>(feed_generation_));
  h = HashMix(h, static_cast<uint64_t>(work_precision_));
  h = HashMix(h, static_cast<uint64_t>(canvas_w_));
  h = HashMix(h, static_cast<uint64_t>(canvas_h_));
  h = HashMix(h, Quantize(mapping_target_.peak_nits));
  h = HashMix(h, Quantize(mapping_target_.black_nits));
  h = HashMix(h, Quantize(mapping_target_.sdr_white_nits));
  h = HashMix(h, static_cast<uint64_t>(mapping_target_.primaries));
  h = HashMix(h, static_cast<uint64_t>(state.show_ref));
  h = HashMix(h, renderer_->IsHdrVividEnabled() ? 1u : 0u);
  h = HashMix(h, state.ref_id.has_value() ? static_cast<uint64_t>(*state.ref_id) + 1 : 0);
  h = HashMix(h, static_cast<uint64_t>(state.fill.id) + 1);
  h = HashMix(h, static_cast<uint64_t>(state.slide.left_id) + 1);
  h = HashMix(h, static_cast<uint64_t>(state.slide.right_id) + 1);
  h = HashMix(h, Quantize(state.slide.split_pos));
  h = HashMix(h, static_cast<uint64_t>(state.grid.cols));
  h = HashMix(h, static_cast<uint64_t>(state.grid.rows));
  for (int id : state.ids) {
    h = HashMix(h, static_cast<uint64_t>(id) + 1);
  }
  if (state.amplify.has_value()) {
    h = HashMix(h, Quantize(*state.amplify) ^ 0xabcdULL);
  }
  h = HashMix(h, Quantize(state.scale));
  h = HashMix(h, Quantize(state.offset_x));
  h = HashMix(h, Quantize(state.offset_y));
  return h;
}

const RenderedFrame &DisplayCoordinator::RenderScene(const DisplayState &state) {
  AssertUiThread();
  return ComposeLocked(state);
}

const RenderedFrame &DisplayCoordinator::ComposeLocked(const DisplayState &state) {
  if (!has_canvas()) {
    // Fall back to the main source size so a coordinator without an explicit
    // canvas still composes something sensible. Fail loudly when nothing is
    // available.
    const struct pl_frame *main = renderer_->GetSourceFrame(0);
    if (main == nullptr || main->planes[0].texture == nullptr) {
      throw std::logic_error("DisplayCoordinator::RenderScene: no canvas size and no main source frame");
    }
    canvas_w_ = static_cast<int>(main->planes[0].texture->params.w);
    canvas_h_ = static_cast<int>(main->planes[0].texture->params.h);
  }

  const uint64_t key = ComputeSceneKey(state);
  if (scene_key_valid_ && key == last_scene_key_ && result_.IsValid()) {
    // A UI-only change (resize, OSD toggle) must not re-run the source
    // mapping/HDR work.
    return result_;
  }

  // NaN/Inf policy: the difference/high-precision path validates the common
  // result; plain playback does not pay for the readback.
  const bool amplify = !state.show_ref && state.amplify.has_value() && state.ref_id.has_value();
  renderer_->SetValidateLinearResult(validate_difference_result_ && amplify);
  renderer_->RenderScene(canvas_w_, canvas_h_, state);
  const struct pl_frame &linear = renderer_->GetLinearResult();
  result_.texture = renderer_->GetLinearResultTexture();
  result_.width = canvas_w_;
  result_.height = canvas_h_;
  result_.color = linear.color;
  result_.precision = renderer_->GetLinearResultPrecision();
  result_.sdr_white_nits = mapping_target_.sdr_white_nits;
  result_.black_nits = mapping_target_.black_nits;
  if (!scene_key_valid_ || key != last_scene_key_) {
    ++scene_revision_;
  }
  result_.identity.scene_revision = scene_revision_;
  ++scene_render_count_;
  result_.identity.render_id = scene_render_count_;
  result_.identity.source_serials = feed_generation_;
  last_scene_key_ = key;
  scene_key_valid_ = true;
  return result_;
}

void DisplayCoordinator::PresentTo(struct pl_frame &target, const DisplayState &state) {
  AssertUiThread();
  ComposeLocked(state);
  PresentLocked(target, state);
}

void DisplayCoordinator::PresentLocked(struct pl_frame &target, const DisplayState &state) {
  if (!result_.IsValid()) {
    throw std::logic_error("DisplayCoordinator::PresentTo called before RenderScene");
  }
  if (!target.planes[0].texture) {
    throw std::invalid_argument("DisplayCoordinator::PresentTo: null target");
  }
  const int window_w = static_cast<int>(target.planes[0].texture->params.w);
  const int window_h = static_cast<int>(target.planes[0].texture->params.h);
  const CanvasLayout layout = canvas_layout(window_w, window_h);
  if (layout.preview.IsEmpty()) {
    throw std::logic_error("DisplayCoordinator::PresentTo: degenerate canvas or preview rectangle");
  }
  const RectF np = NormalizedPreview(layout);
  ++present_count_;
  renderer_->PresentToTarget(target, state, EncodeRect{np.x, np.y, np.w, np.h});
}

void DisplayCoordinator::CheckOutput(const struct pl_color_space *output) const {
  AssertUiThread();
  if (monitoring_ || !renderer_->expects_hdr_output() || pl_color_space_is_hdr(output)) {
    return;
  }
  throw std::runtime_error(fmt::format("A HDR output was requested but the window output is SDR ({}): this platform "
                                       "offers no HDR surface. Pass --target-display-nits sdr to map to SDR",
                                       pl_color_transfer_name(output->transfer)));
}
