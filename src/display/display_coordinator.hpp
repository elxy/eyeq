#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
 * Threading: UI thread only. The 2C-1 synchronized player publishes frame
 * snapshots, so FeedFrames() is no longer called from the decode thread; every
 * public method must be called from the thread that constructed this object. There
 * is no internal mutex; Debug builds assert the calling thread.
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
    // difference/high-precision path (a full-frame readback). The desktop playback
    // path leaves this off to avoid the readback cost, and the monitor/playback
    // path deliberately does not validate NaN/Inf at all (a non-finite sample is a
    // source/upstream problem there; see the MonitorOutput header).
    bool validate_difference_result = true;
    // Fixed common mapping target owned by this layer. It is never re-derived
    // from the presentation target (V2-1 handoff #5).
    MappingTarget mapping_target{};
    int canvas_w = 0;
    int canvas_h = 0;
    // HDR Vivid dynamic tone mapping. When disabled (default) sources with CUVA
    // metadata are mapped as plain PQ content, exactly as before.
    bool hdrvivid = false;
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
  // Opt-in source HDR max_luma correction (--source-max-luma). Source-only: it
  // never moves the mapping target.
  void SetSourceMaxLuma(double nits);

  /*
   * Enable or disable HDR Vivid dynamic tone mapping on the single renderer.
   * The curve targets the coordinator's fixed common mapping target, so Vivid
   * and Dolby Vision (whose reshape/mapping is performed inside the same
   * pl_render_image source decode) share one mapping exit. Toggling it changes
   * the scene, so the current result is invalidated.
   */
  void SetHdrVividEnabled(bool enabled);
  bool hdrvivid_enabled() const { return renderer_->IsHdrVividEnabled(); }
  const MappingTarget &mapping_target() const { return mapping_target_; }
  // Explicitly replaces the fixed mapping target (invalidates the cached scene).
  void SetMappingTarget(const MappingTarget &target);

  WorkPrecision work_precision() const { return work_precision_; }
  /*
   * Functional precision reconfiguration. It applies immediately (the precision
   * never changes with load); the cached scene is invalidated so the next
   * RenderScene() recomposes at the new precision.
   */
  void ReconfigurePrecision(WorkPrecision precision);

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

  // ---- diagnostics ---------------------------------------------------------
  const RenderedFrame &result() const { return result_; }
  uint64_t feed_count() const { return feed_count_; }
  uint64_t scene_render_count() const { return scene_render_count_; }
  uint64_t present_count() const { return present_count_; }
  uint64_t precision_reconfigure_count() const { return precision_reconfigure_count_; }
  uint64_t scene_revision() const { return scene_revision_; }

private:
  uint64_t ComputeSceneKey(const DisplayState &state) const;
  void ApplyPrecision(WorkPrecision precision);
  void InvalidateScene();
  // Debug-only UI-thread assertion; a no-op in release builds.
  void AssertUiThread() const;
  const RenderedFrame &ComposeLocked(const DisplayState &state);
  void PresentLocked(struct pl_frame &target, const DisplayState &state);

  std::thread::id ui_thread_id_;
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

  bool validate_difference_result_ = true;

  uint64_t feed_count_ = 0;
  uint64_t scene_render_count_ = 0;
  uint64_t present_count_ = 0;
  uint64_t precision_reconfigure_count_ = 0;
};

} // namespace EYEQ
