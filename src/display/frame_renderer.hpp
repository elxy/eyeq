#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}
#include <libplacebo/gpu.h>

#include "canvas_geometry.hpp"
#include "clip_hook.hpp"
#include "fill_render.hpp"
#include "hdrvivid_shader.hpp"
#include "mapping_target.hpp"
#include "osd.hpp"
#include "output_encode.hpp"
#include "output_present.hpp"
#include "render.hpp"
#include "render_context.hpp"
#include "video_frame.hpp"

namespace EYEQ {

/*
 * Owns all target-related rendering resources so that several independent
 * targets can be rendered from one shared RenderContext: the Fill/Slide/Grid
 * compositor, render parameters, the intermediate textures and dispatches held
 * by the compositor, the VideoFrame/pl_frame views and the OSD state.
 *
 * Render() takes an explicit target frame and DisplayState; it never queries a
 * window or swapchain. The OSD is optional and created only when the config
 * requests it, so an offscreen
 * instance without a font or SDL window is possible.
 *
 * Threading contract: this class is not internally synchronized. The caller
 * must serialize every state operation on one instance from a single thread,
 * including parameter updates, FeedFrames(), Render() and destruction.
 * In the player that thread is the UI thread: the 2C-1 synchronized player
 * publishes frame snapshots, so FeedFrames() is no longer called from the decode
 * thread. The colorspace-hint callback runs inside FeedFrames() on that same
 * thread. Distinct instances share only the RenderContext and must not be used
 * concurrently during this phase.
 */
class FrameRenderer {
public:
  /*
   * Called from FeedFrames() with the color space the output should declare
   * (see FeedFrames), while the caller's serialization is held. Used
   * by Window to drive pl_swapchain_colorspace_hint without exposing the frame
   * map. May be null for offscreen instances.
   */
  using ColorspaceHintFunc = std::function<void(const struct pl_color_space *)>;

  struct Config {
    DisplayMode mode = DisplayMode::Fill;
    ScaleMethod scale_method = ScaleMethod::Bicubic;
    ScaleMethod plane_scale_method = ScaleMethod::Bicubic;
    float pixel_density = 1.0f;
    // When false, no SDL_ttf font and no SDL surface are created and Render()
    // draws no OSD overlay. The OSD manager is created (or not) at construction
    // time based on this flag; it is not created lazily on first use.
    bool enable_osd = true;
    // --target-display-nits / --tone-mapping. The default is the explicit-target,
    // highlight-compressing configuration; SetMappingTarget() provides the peak.
    TargetKind target_kind = TargetKind::HdrNits;
    ToneMapping tone_mapping = ToneMapping::Spline;
  };

  FrameRenderer(RenderContext &context, const Config &config, ColorspaceHintFunc colorspace_hint = nullptr);
  ~FrameRenderer();

  FrameRenderer(const FrameRenderer &) = delete;
  FrameRenderer &operator=(const FrameRenderer &) = delete;
  FrameRenderer(FrameRenderer &&) = delete;
  FrameRenderer &operator=(FrameRenderer &&) = delete;

  void SetMainSource(int id) { main_id_ = id; }
  void SetSdrWhiteOnHdr(float nits) { sdr_white_on_hdr_ = nits; }
  /*
   * --default-mastering-nits: mastering display peak (pl_color_space.hdr.max_luma)
   * for HDR sources that have none after libplacebo mapped the frame (Dolby Vision
   * gets it from the RPU and is therefore not "missing"). Sources with a peak are
   * left alone; MaxCLL and dynamic metadata are never touched.
   */
  void SetDefaultMasteringNits(double nits);
  // The mastering peak declared to the system with a HDR output (0 before the
  // first feed, or when no HDR output is declared). See ResolveDeclaredPeak().
  float declared_peak_nits() const { return declared_peak_nits_; }
  // True when the fed sources and the configured target ask for a HDR output.
  bool expects_hdr_output() const { return expects_hdr_output_; }
  // The resolved output color space (valid after the first feed).
  const struct pl_color_space &output_color() const { return output_color_; }
  const MappingTarget &mapping_target() const { return mapping_target_; }

  /*
   * Enable GPU HDR Vivid dynamic tone mapping. When enabled, each fed source
   * that carries CUVA HDR Vivid metadata is solved against the *common mapping
   * target* (not a per-window target) and mapped by the single renderer through
   * a PL_HOOK_RGB render hook. Sources without such metadata are untouched.
   */
  void SetHdrVividEnabled(bool enabled);
  bool IsHdrVividEnabled() const { return hdrvivid_enabled_; }
  // Frames that were solved from HDR Vivid metadata since construction.
  uint64_t hdrvivid_frame_count() const { return hdrvivid_frame_count_; }
  void ResetHdrVividCounters() { hdrvivid_frame_count_ = 0; }

  /*
   * Overrides the display mapping target used by the comparison core. The
   * target is the "HDR mapping completed" description; its storage precision is
   * a functional choice (F16 for plain display once verified, F32 for the
   * difference path), never a load-adaptive one.
   */
  void SetMappingTarget(const MappingTarget &target, WorkPrecision precision);

  std::vector<int> FeedFrames(const std::map<int, std::shared_ptr<AVFrame>> &frames);
  bool HasFrames() const { return !av_frames_.empty(); }

  /*
   * Read-only view of a mapped source frame (nullptr when @p video_id was not
   * fed). Exposed for diagnostics and tests; the caller must not retain it
   * beyond the next Feed/Render call on this instance.
   */
  const struct pl_frame *GetSourceFrame(int video_id) const;

  /*
   * Composes the OSD (if enabled and requested) and renders @p state into the
   * explicit @p target. The target determines size and color space; no window
   * or swapchain state is consulted.
   */
  void Render(struct pl_frame &target, const DisplayState &state);

  /*
   * Compose the common display-linear result at the fixed canvas size. The
   * mapping target is an explicit, fixed input here: unlike Render() it never
   * follows the presentation target's primaries (V2-1 handoff #5).
   */
  void RenderScene(int width, int height, const DisplayState &state);

  /*
   * Encode/present the result of the last RenderScene() into @p target, scaled
   * into @p preview (normalized target rectangle; full by default), then blend
   * the desktop text OSD. The desktop presenter owns the swapchain, so this is
   * the only place the canvas is scaled and letterboxed.
   */
  void PresentToTarget(struct pl_frame &target, const DisplayState &state, const EncodeRect &preview = EncodeRect{});

  // When enabled, RenderScene() downloads the linear result and throws when it
  // contains NaN/Inf (fail-fast NaN/Inf policy, V2.1 section 3.2). Off by default
  // because a full-frame readback per frame regresses the plain desktop path;
  // the difference/high-precision path and the tests enable it.
  void SetValidateLinearResult(bool enabled) { validate_linear_result_ = enabled; }
  bool validate_linear_result() const { return validate_linear_result_; }
  // Explicitly inspect the last linear result; throws std::runtime_error on a
  // non-finite sample. Public so the coordinator/callers can enforce it.
  void ValidateLinearResultFinite();

  /*
   * Diagnostics/tests: the display-linear composition produced by the last
   * Render() call. The texture is owned by the compositor and is only valid
   * until the next Render()/FeedFrames() call.
   */
  const struct pl_frame &GetLinearResult() const;
  pl_tex GetLinearResultTexture() const;
  WorkPrecision GetLinearResultPrecision() const;
  WorkPrecision GetDisPrecision() const;
  WorkPrecision GetRefPrecision() const;

  // Diagnostics: the number of source-mapping passes for a video and the number
  // of those that attached the HDR Vivid hook since the last reset. The player
  // does not use them; they make the "each compared source is mapped exactly
  // once, the dynamic-HDR curve runs once" contract observable.
  uint64_t MappingPassCount(int video_id) const;
  uint64_t HdrVividApplyCount() const;
  void ResetMappingCounts();

  OsdManager *GetOsdManager() { return osd_manager_.get(); }
  DisplayMode mode() const { return mode_; }

private:
  // Re-solve the HDR Vivid curve for every source that carried dynamic metadata
  // when it was fed. Called whenever the input changes and whenever the common
  // mapping target changes, so the curve always targets the shared target.
  void ResolveHdrVividParams();
  const struct pl_hook *HdrVividHookForVideo(int video_id);

  // Encodes the canvas into the presentation target (transfer only) and scales
  // it into @p preview through the shared output stage.
  void EncodeToTarget(struct pl_frame &target, const struct pl_frame &linear, const EncodeRect &preview);
  // Resolves the output color space (and the mapping target primaries) once,
  // on the first feed, after the declared peak.
  void ResolveOutputColor();

  // Compose the desktop text OSD overlay; @p preview is the normalized target
  // rectangle the canvas is presented into. Returns nullptr when OSD is off.
  pl_tex ComposeOsdOverlay(int target_w, int target_h, const EncodeRect &preview, const DisplayState &state);

  RenderContext *context_;
  DisplayMode mode_;
  float pixel_density_;

  struct pl_render_params render_params_;
  // FillRender (and its Slide/Grid subclasses) owns the pl_renderer and the
  // compositor textures/dispatches; FrameRenderer must not own them separately.
  std::unique_ptr<FillRender> display_render_;
  std::unique_ptr<OsdManager> osd_manager_;

  int main_id_ = 0;
  float sdr_white_on_hdr_ = 0; // SDR white level override (nits) for HDR rendering

  // The single display mapping target every source is mapped to before the
  // comparison, and the storage precision of the display-linear work textures.
  MappingTarget mapping_target_;
  WorkPrecision work_precision_ = WorkPrecision::F16;

  // The output stage: transfer encode, then scaling into the preview rectangle.
  std::unique_ptr<OutputPresenter> presenter_;
  // The resolved output color space (the swapchain hint).
  struct pl_color_space output_color_{};

  // NaN/Inf fail-fast switch and the validity of the last compose.
  bool validate_linear_result_ = false;
  bool scene_valid_ = false;
  int scene_width_ = 0;
  int scene_height_ = 0;

  // --default-mastering-nits; 0 when not set.
  double default_mastering_nits_ = 0.0;
  std::set<int> default_mastering_logged_; // video ids already reported as filled
  std::set<int> maxcll_warned_;            // video ids already warned about MaxCLL above the filled peak
  std::set<int> missing_peak_warned_;      // video ids already warned about a missing mastering peak

  // --target-display-nits / --tone-mapping.
  TargetKind target_kind_;
  ToneMapping tone_mapping_;
  struct pl_color_map_params color_map_params_;
  std::unique_ptr<ClipHook> clip_hook_;
  /*
   * The mastering peak handed to the system with a HDR output, describing what
   * this renderer actually outputs rather than the main video's source metadata:
   * min(target peak, brightest HDR source's mastering peak), or just the
   * brightest source peak without a target. Resolved once on the first feed.
   * Without a target that value also becomes the mapping target peak, so sources
   * are not compressed.
   */
  void ResolveDeclaredPeak();
  bool declared_peak_resolved_ = false;
  float declared_peak_nits_ = 0.0f;
  bool expects_hdr_output_ = false;
  std::set<int> rgb_limited_warned_; // video ids already warned about an ignored RGB limited-range tag

  // Dynamic HDR (HDR Vivid). The shader instance is shared and its parameters
  // are set immediately before the source mapping of one video; the mapping is
  // not re-entrant, so that stays safe without a second renderer.
  bool hdrvivid_enabled_ = false;
  std::unique_ptr<HDRVividShader> hdrvivid_shader_;
  std::map<int, HDRVividMetadata> hdrvivid_metadata_; // parsed dynamic metadata per source
  std::map<int, double> hdrvivid_mastering_nits_;     // mastering peak per source
  std::map<int, HDRVividShaderParams> hdrvivid_params_;
  uint64_t hdrvivid_frame_count_ = 0;
  bool hdrvivid_logged_ = false;

  std::map<int, VideoFrame> av_frames_;
  std::map<int, struct pl_frame *> pl_frames_;

  ColorspaceHintFunc colorspace_hint_;
};

}; // namespace EYEQ
