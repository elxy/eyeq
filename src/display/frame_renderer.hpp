#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/icc.h>

#include "canvas_geometry.hpp"
#include "fill_render.hpp"
#include "hdrvivid_shader.hpp"
#include "mapping_target.hpp"
#include "osd.hpp"
#include "output_encode.hpp"
#include "render.hpp"
#include "render_context.hpp"
#include "video_frame.hpp"

namespace EYEQ {

/*
 * Owns all target-related rendering resources so that several independent
 * targets can be rendered from one shared RenderContext: the Fill/Slide/Grid
 * compositor, render parameters, the intermediate textures and dispatches held
 * by the compositor, the VideoFrame/pl_frame views, the ICC data and objects
 * and the OSD state.
 *
 * Render() takes an explicit target frame and DisplayState; it never queries a
 * window or swapchain. The OSD is optional and created only when the config
 * requests it, so an offscreen
 * instance without a font or SDL window is possible.
 *
 * Threading contract: this class is not internally synchronized. The caller
 * must serialize every state operation on one instance from a single thread,
 * including ICC and parameter updates, FeedFrames(), Render() and destruction.
 * In the player that thread is the UI thread: the 2C-1 synchronized player
 * publishes frame snapshots, so FeedFrames() is no longer called from the decode
 * thread. The colorspace-hint callback runs inside FeedFrames() on that same
 * thread. Distinct instances share only the RenderContext and must not be used
 * concurrently during this phase.
 */
class FrameRenderer {
public:
  /*
   * Called from FeedFrames() with the main video's color space (or nullptr
   * when the hint is disabled), while the caller's serialization is held. Used
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
    bool colorspace_hint = false;
    // Dither the final desktop presentation hop to a low-depth swapchain. The
    // common display-linear result and the wire encode are never dithered.
    bool present_dither = true;
  };

  FrameRenderer(RenderContext &context, const Config &config, ColorspaceHintFunc colorspace_hint = nullptr);
  ~FrameRenderer();

  FrameRenderer(const FrameRenderer &) = delete;
  FrameRenderer &operator=(const FrameRenderer &) = delete;
  FrameRenderer(FrameRenderer &&) = delete;
  FrameRenderer &operator=(FrameRenderer &&) = delete;

  void SetMainSource(int id) { main_id_ = id; }
  void SetSdrWhiteOnHdr(float nits) { sdr_white_on_hdr_ = nits; }
  void SetTargetDisplayNits(double nits, bool override_enabled);

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

  /*
   * Store and open an ICC profile. @p data is the raw profile binary; resolving
   * "auto" to system display data is the caller's (Window's) responsibility.
   * While application-side ICC is disabled (monitoring mode, V2.1 D2) the call
   * is a no-op that logs once; the stored profiles are neither opened nor used.
   */
  void SetICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id = std::nullopt);

  // Enables/disables every application-side ICC profile (global, auto and
  // per-video). Disabling closes and clears the profiles and makes
  // GetICCObjectForVideo()/GetICCTargetColorSpace() inert. Monitoring mode sets
  // this to false at construction; the plain desktop keeps it enabled.
  void SetIccEnabled(bool enabled);
  bool IsIccEnabled() const { return icc_enabled_; }
  // True when at least one ICC object (global or per-video) is currently open.
  bool HasAnyICC() const { return global_icc_object_ != nullptr || !per_video_icc_objects_.empty(); }

  pl_icc_object GetICCObjectForVideo(int video_id) const;
  /*
   * Fills @p out with the color space the swapchain hint should declare when an
   * ICC profile is active. Returns false when no global ICC is active and the
   * caller should fall back to the source color space.
   */
  bool GetICCTargetColorSpace(struct pl_color_space *out) const;

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
  // Toggles dithering on the final desktop presentation hop (closure b).
  void SetPresentDitherEnabled(bool enabled) { display_render_->SetPresentDitherEnabled(enabled); }
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

  void RecalibrateICCParams(const struct pl_color_space *target_color);

  OsdManager *GetOsdManager() { return osd_manager_.get(); }
  DisplayMode mode() const { return mode_; }

private:
  void OpenICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id);
  void CloseICCProfiles();

  // Re-solve the HDR Vivid curve for every source that carried dynamic metadata
  // when it was fed. Called whenever the input changes and whenever the common
  // mapping target changes, so the curve always targets the shared target.
  void ResolveHdrVividParams();
  const struct pl_hook *HdrVividHookForVideo(int video_id);

  // Explicit transfer encode into the presentation target. Returns false when
  // the target transfer is not PQ/BT.1886, in which case the color-managed
  // presentation hop is used instead.
  bool EncodeToTarget(struct pl_frame &target, const struct pl_frame &linear, const EncodeRect &preview);

  // Compose the desktop text OSD overlay; @p preview is the normalized target
  // rectangle the canvas is presented into. Returns nullptr when OSD is off.
  pl_tex ComposeOsdOverlay(int target_w, int target_h, const EncodeRect &preview, const DisplayState &state);

  RenderContext *context_;
  DisplayMode mode_;
  bool colorspace_hint_enabled_;
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

  // Explicit display-linear -> wire-transfer encoder used for the presentation
  // target when its transfer is PQ or SDR BT.1886 (no tone/gamut mapping).
  std::unique_ptr<OutputEncoder> encoder_;

  // Application-side ICC master switch (monitoring mode disables it).
  bool icc_enabled_ = true;
  // NaN/Inf fail-fast switch and the validity of the last compose.
  bool validate_linear_result_ = false;
  bool scene_valid_ = false;
  int scene_width_ = 0;
  int scene_height_ = 0;

  double target_display_nits_ = 1000.0;
  bool target_display_override_enabled_ = false; // opt-in max_luma override (explicit --target-display-nits)
  bool target_display_override_logged_ = false;  // one-time log for max_luma override

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

  // ICC profile data storage
  std::vector<uint8_t> global_icc_data_;
  pl_icc_object global_icc_object_ = nullptr;
  std::map<int, std::vector<uint8_t>> per_video_icc_data_;
  std::map<int, pl_icc_object> per_video_icc_objects_;
  float last_icc_target_luma_ = -1.0f; // Last calibrated target_luma, used for deduplication

  ColorspaceHintFunc colorspace_hint_;
};

}; // namespace EYEQ
