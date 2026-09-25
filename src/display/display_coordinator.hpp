#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include <libplacebo/colorspace.h>

#include "canvas_geometry.hpp"
#include "frame_renderer.hpp"
#include "mapping_target.hpp"
#include "render.hpp"
#include "render_context.hpp"
#include "rendered_frame.hpp"

namespace EYEQ {

/*
 * Application-side ICC master switch (V2.1 D2). This is a coordinator-level
 * configuration; the user-visible CLI wiring lands in V2-3.
 */
enum class IccMode {
  // Plain desktop: global / auto / per-video ICC keep their original behavior.
  Enabled,
  // Monitoring mode: the common render, the monitor encode path and the Mac
  // preview all disable every application ICC. macOS system color management is
  // not touched.
  MonitoringDisabled,
};

/*
 * Small application coordination layer (V2.1 section 2.1). It owns the *single*
 * FrameRenderer, the fixed common MappingTarget, the work precision, the canvas
 * size and the scene/render identity. Desktop presentation (Window) and the
 * monitor output are consumers of the common result: neither owns a renderer,
 * and neither knows about the source videos, the comparison mode or the HDR
 * mapping algorithm.
 *
 * Threading: not internally synchronized. The player serializes FeedFrames (the
 * decode thread) against RenderScene/PresentTo (the UI thread) exactly like the
 * previous Window::render_mutex_ did.
 */
class DisplayCoordinator {
public:
  using ColorspaceHintFunc = std::function<void(const struct pl_color_space *)>;

  struct Config {
    DisplayMode mode = DisplayMode::Fill;
    ScaleMethod scale_method = ScaleMethod::Bicubic;
    ScaleMethod plane_scale_method = ScaleMethod::Bicubic;
    float pixel_density = 1.0f;
    bool enable_osd = true;
    bool colorspace_hint = false;
    // Dither the final desktop presentation hop (not the common result/encode).
    bool present_dither = true;
    IccMode icc_mode = IccMode::Enabled;
    // F16 is the plain-display candidate; the difference path upgrades to F32
    // per render. This is a functional choice, never load-adaptive.
    WorkPrecision work_precision = WorkPrecision::F16;
    // NaN/Inf policy: validate the common display-linear result for the
    // difference/high-precision path (a full-frame readback, acceptable while
    // still-only). Plain desktop playback leaves this off to avoid the readback
    // cost; the gate is always available through FrameRenderer.
    bool validate_difference_result = true;
    // Fixed common mapping target owned by this layer. It is never re-derived
    // from the presentation target (V2-1 handoff #5).
    MappingTarget mapping_target{};
    int canvas_w = 0;
    int canvas_h = 0;
  };

  DisplayCoordinator(RenderContext &context, const Config &config, ColorspaceHintFunc presenter_hint = nullptr);
  ~DisplayCoordinator();

  DisplayCoordinator(const DisplayCoordinator &) = delete;
  DisplayCoordinator &operator=(const DisplayCoordinator &) = delete;
  DisplayCoordinator(DisplayCoordinator &&) = delete;
  DisplayCoordinator &operator=(DisplayCoordinator &&) = delete;

  // ---- configuration / identity -------------------------------------------
  IccMode icc_mode() const { return icc_mode_; }
  void SetIccMode(IccMode mode);
  // True when application ICC is enabled and at least one profile is loaded.
  bool IsIccActive() const;

  /*
   * Load an application ICC profile. While ICC is disabled (monitoring mode)
   * this is a no-op that reports the mode explicitly instead of silently
   * applying or dropping the profile. @p data is the raw profile binary; the
   * "auto" resolution to system display data is the desktop layer's job.
   */
  void LoadICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id = std::nullopt);
  bool GetICCTargetColorSpace(struct pl_color_space *out) const;

  void SetMainSource(int id);
  void SetSdrWhiteOnHdr(float nits);
  void SetTargetDisplayNits(double nits, bool override_enabled);
  // Sets the monitor/comparison target peak and SDR white explicitly. Used when
  // monitoring is enabled, where the external monitor target is the comparison
  // baseline (V2.1 section 8.2); --target-display-nits keeps its historical
  // source-max_luma override meaning and never becomes the monitor target.
  void SetMonitorTarget(float peak_nits, float sdr_white_nits);

  const MappingTarget &mapping_target() const { return mapping_target_; }
  // Explicitly replaces the fixed mapping target (invalidates the cached scene).
  void SetMappingTarget(const MappingTarget &target);

  /*
   * True when the given state would force a new composition (different scene key
   * or no usable cached result). The caller uses this to avoid asking for a
   * recomposition while a consumer holds the common result (the monitor borrow),
   * which the assertions below reject.
   */
  bool WouldRecompose(const DisplayState &state) const;

  WorkPrecision work_precision() const { return work_precision_; }
  /*
   * Safe functional precision reconfiguration. If a consumer currently holds the
   * common result it is deferred until ReleaseResult(); returns true when the
   * change was applied immediately. The precision never changes with load.
   */
  bool ReconfigurePrecision(WorkPrecision precision);
  bool has_pending_precision() const { return pending_precision_.has_value(); }

  void SetCanvasSize(int width, int height);
  bool has_canvas() const { return canvas_w_ > 0 && canvas_h_ > 0; }
  int canvas_width() const { return canvas_w_; }
  int canvas_height() const { return canvas_h_; }
  CanvasLayout canvas_layout(int window_w, int window_h) const;

  // ---- frame consumption / composition ------------------------------------
  std::vector<int> FeedFrames(const std::map<int, std::shared_ptr<AVFrame>> &frames);
  bool HasFrames() const { return renderer_->HasFrames(); }

  /*
   * Compose the common display-linear result for @p state at the fixed canvas.
   * Repeated calls with an unchanged scene key are a no-op that returns the
   * cached result (so a window resize or an OSD toggle does not re-run the
   * source mapping/HDR work).
   */
  const RenderedFrame &RenderScene(const DisplayState &state);

  /*
   * Present the current common result into @p target (desktop). The canvas is
   * scaled into the letterboxed preview rectangle computed from the target size.
   */
  void PresentTo(struct pl_frame &target, const DisplayState &state);

  // Recalibrate the open ICC objects for the final presentation color space.
  // No-op in monitoring mode (no application ICC is loaded).
  void RecalibrateICC(const struct pl_color_space *target_color);

  // Borrow management: a consumer marks the result borrowed while it uses it
  // (for example the monitor output encoding/packing it) so a precision
  // reconfiguration waits for the drain. While borrowed, asking for a different
  // scene or a mapping-target change is rejected (fail-fast) instead of
  // overwriting the texture the consumer is still reading.
  void AcquireResult();
  void ReleaseResult();
  bool result_borrowed() const { return result_borrowed_; }

  // ---- diagnostics ---------------------------------------------------------
  const RenderedFrame &result() const { return result_; }
  uint64_t feed_count() const { return feed_count_; }
  uint64_t scene_render_count() const { return scene_render_count_; }
  uint64_t present_count() const { return present_count_; }
  uint64_t precision_reconfigure_count() const { return precision_reconfigure_count_; }
  uint64_t recompose_rejected_count() const { return recompose_rejected_count_; }
  uint64_t scene_revision() const { return scene_revision_; }

private:
  uint64_t ComputeSceneKey(const DisplayState &state) const;
  void ApplyPrecision(WorkPrecision precision);
  void InvalidateScene();
  // Locked internals: FeedFrames/RenderScene/PresentTo all serialize on
  // mutex_ (the same single-writer contract the old Window::render_mutex_ had).
  const RenderedFrame &ComposeLocked(const DisplayState &state);
  void PresentLocked(struct pl_frame &target, const DisplayState &state);

  mutable std::mutex mutex_;
  std::unique_ptr<FrameRenderer> renderer_;
  ColorspaceHintFunc presenter_hint_;

  MappingTarget mapping_target_;
  WorkPrecision work_precision_ = WorkPrecision::F16;
  IccMode icc_mode_ = IccMode::Enabled;

  int canvas_w_ = 0;
  int canvas_h_ = 0;

  RenderedFrame result_;
  uint64_t last_scene_key_ = 0;
  bool scene_key_valid_ = false;
  uint64_t feed_generation_ = 0;
  uint64_t scene_revision_ = 0;

  bool result_borrowed_ = false;
  bool validate_difference_result_ = true;
  std::optional<WorkPrecision> pending_precision_;

  uint64_t feed_count_ = 0;
  uint64_t scene_render_count_ = 0;
  uint64_t present_count_ = 0;
  uint64_t precision_reconfigure_count_ = 0;
  uint64_t recompose_rejected_count_ = 0;
};

} // namespace EYEQ
