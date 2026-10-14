#include "frame_renderer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

#include <spdlog/fmt/fmt.h>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "log.hpp"
#include "platform_icc.h"
#include "slide_render.hpp"
#include "grid_render.hpp"
#include "hdrvivid_meta.hpp"
#include "utils.hpp"

using namespace EYEQ;

static const struct pl_filter_config *get_filter_config(ScaleMethod method) {
  switch (method) {
  case ScaleMethod::Nearest:
    return &pl_filter_nearest;
  case ScaleMethod::Bilinear:
    return &pl_filter_bilinear;
  case ScaleMethod::Bicubic:
    return &pl_filter_bicubic;
  case ScaleMethod::Lanczos:
    return &pl_filter_lanczos;
  case ScaleMethod::EWALanczos:
    return &pl_filter_ewa_lanczos;
  case ScaleMethod::EWALanczosSharp:
    return &pl_filter_ewa_lanczossharp;
  case ScaleMethod::Mitchell:
    return &pl_filter_mitchell;
  case ScaleMethod::CatmullRom:
    return &pl_filter_catmull_rom;
  case ScaleMethod::Spline36:
    return &pl_filter_spline36;
  case ScaleMethod::Spline64:
    return &pl_filter_spline64;
  default:
    return nullptr;
  }
}

FrameRenderer::FrameRenderer(RenderContext &context, const Config &config, ColorspaceHintFunc colorspace_hint)
    : context_(&context), mode_(config.mode), pixel_density_(config.pixel_density), target_kind_(config.target_kind),
      tone_mapping_(config.tone_mapping), colorspace_hint_(std::move(colorspace_hint)) {
  if (!context_->HasDevice()) {
    throw std::logic_error("FrameRenderer requires a RenderContext with a created device");
  }

  render_params_ = pl_render_default_params;
  render_params_.dither_params = nullptr;
  render_params_.peak_detect_params = nullptr;
  render_params_.background_color[0] = 0.2f;
  render_params_.background_color[1] = 0.2f;
  render_params_.background_color[2] = 0.2f;

  // --tone-mapping. Without a target nothing exceeds the resolved peak, so the
  // function is irrelevant there; both clip modes bypass the tone mapping per
  // source (see FillRender::SetClip).
  color_map_params_ = pl_color_map_default_params;
  color_map_params_.tone_mapping_function = &pl_tone_map_spline;
  render_params_.color_map_params = &color_map_params_;

  render_params_.upscaler = get_filter_config(config.scale_method);
  render_params_.plane_upscaler = get_filter_config(config.plane_scale_method);

  FillRender::ICCQueryFunc icc_query = [this](int video_id) -> pl_icc_object { return GetICCObjectForVideo(video_id); };
  // The HDR Vivid hook is selected per source inside the compositor, so Grid can
  // attach a different curve to each cell from one shared renderer.
  FillRender::HdrVividQueryFunc hdrvivid_query = [this](int video_id) -> const struct pl_hook * {
    return HdrVividHookForVideo(video_id);
  };

  pl_gpu gpu = context_->gpu();
  switch (mode_) {
  case DisplayMode::Fill:
    display_render_ =
        std::make_unique<FillRender>(context_->log(), gpu, &render_params_, pixel_density_, icc_query, hdrvivid_query);
    break;
  case DisplayMode::Slide:
    display_render_ =
        std::make_unique<SlideRender>(context_->log(), gpu, &render_params_, pixel_density_, icc_query, hdrvivid_query);
    break;
  case DisplayMode::Grid:
    display_render_ =
        std::make_unique<GridRender>(context_->log(), gpu, &render_params_, pixel_density_, icc_query, hdrvivid_query);
    break;
  default:
    throw std::runtime_error("Unsupported display mode for FrameRenderer");
  }

  // With a SDR target clip-rgb needs no hook: the presentation hop already clips
  // every channel in the real output gamut.
  if (tone_mapping_ == ToneMapping::Clip) {
    clip_hook_ = std::make_unique<ClipHook>(ClipHook::Mode::MaxRgb);
    display_render_->SetClip(true, clip_hook_->hook());
  } else if (tone_mapping_ == ToneMapping::ClipRgb) {
    if (target_kind_ != TargetKind::Sdr) {
      clip_hook_ = std::make_unique<ClipHook>(ClipHook::Mode::PerChannel);
    }
    display_render_->SetClip(true, clip_hook_ ? clip_hook_->hook() : nullptr);
  }

  // The OSD is optional. Leaving it disabled keeps offscreen instances free of
  // SDL_ttf/font/SDL surface dependencies.
  if (config.enable_osd) {
    osd_manager_ = std::make_unique<OsdManager>(gpu, pixel_density_);
  }

  // Default comparison mapping target: BT.2020/D65, a 1000-nit nominal peak, true
  // black and the standard SDR white. The coordinator replaces this target with
  // the fixed one it owns; the default only matters for a standalone renderer. The
  // work precision is a functional choice; F16 is the plain-display candidate and
  // the difference path upgrades itself to F32.
  mapping_target_.primaries = PL_COLOR_PRIM_BT_2020;
  mapping_target_.peak_nits = 1000.0f;
  mapping_target_.black_nits = 0.0f;
  mapping_target_.sdr_white_nits = PL_COLOR_SDR_WHITE;
  work_precision_ = WorkPrecision::F16;
  if (clip_hook_) {
    clip_hook_->SetPeak(mapping_target_.NitsToNorm(mapping_target_.peak_nits));
  }
  display_render_->SetMappingTarget(mapping_target_, work_precision_);

  display_render_->SetPresentDitherEnabled(config.present_dither);
  encoder_ = std::make_unique<OutputEncoder>(context.log(), gpu);
}

FrameRenderer::~FrameRenderer() {
  // ICC objects hold GPU-related state and must be released before the
  // context's Vulkan device goes away.
  if (global_icc_object_) {
    pl_icc_close(&global_icc_object_);
  }
  for (auto &[id, obj] : per_video_icc_objects_) {
    if (obj) {
      pl_icc_close(&obj);
    }
  }
  per_video_icc_objects_.clear();

  // Frame mappings (pl_unmap_avframe, hwframe references) and the compositor's
  // GPU resources must be released before the context releases the FFmpeg
  // hw_device wrapper and the Vulkan device.
  pl_frames_.clear();
  av_frames_.clear();
  osd_manager_.reset();
  display_render_.reset();
}

void FrameRenderer::SetDefaultMasteringNits(double nits) {
  // Source metadata completion only; the mapping target is untouched.
  default_mastering_nits_ = nits;
  default_mastering_logged_.clear();
}

void FrameRenderer::ResolveDeclaredPeak() {
  // Resolved once from the first fed frames (after --default-mastering-nits) and
  // fixed afterwards, so neither the declared peak nor a source-derived target
  // follows per-frame metadata or the main video selection.
  declared_peak_resolved_ = true;
  float source_peak = 0.0f;
  int peak_id = -1;
  for (const auto &[id, frame] : pl_frames_) {
    const struct pl_color_space &csp = frame->color;
    if (!pl_color_space_is_hdr(&csp)) {
      continue;
    }
    // Only the mastering peak (for Dolby Vision libplacebo fills it from the RPU
    // source_max_pq); MaxCLL is not a substitute, it does not drive tone mapping.
    float nits = csp.hdr.max_luma;
    if (nits <= 0.0f) {
      // libplacebo maps such a source from the transfer's nominal peak.
      struct pl_nominal_luma_params luma_params{};
      luma_params.color = &csp;
      luma_params.metadata = PL_HDR_METADATA_NONE;
      luma_params.scaling = PL_HDR_NITS;
      luma_params.out_max = &nits;
      pl_color_space_nominal_luma_ex(&luma_params);
      if (missing_peak_warned_.insert(id).second) {
        Logger->warn("Video #{}: HDR source has no mastering peak, assuming {:.0f} nits; the picture may be too dark. "
                     "Pass --default-mastering-nits to set it",
                     id, nits);
      }
    }
    if (nits > source_peak) {
      source_peak = nits;
      peak_id = id;
    }
  }
  source_peak = std::min(source_peak, 10000.0f);

  const bool main_is_hdr = pl_color_space_is_hdr(&pl_frames_.at(main_id_)->color);
  switch (target_kind_) {
  case TargetKind::Sdr:
    expects_hdr_output_ = false;
    declared_peak_nits_ = 0.0f;
    break;
  case TargetKind::HdrNits:
    expects_hdr_output_ = true;
    declared_peak_nits_ = peak_id >= 0 ? std::min(mapping_target_.peak_nits, source_peak) : mapping_target_.peak_nits;
    break;
  case TargetKind::None:
    expects_hdr_output_ = main_is_hdr;
    if (peak_id >= 0) {
      mapping_target_.peak_nits = source_peak;
      ValidateMappingTarget(mapping_target_);
      display_render_->SetMappingTarget(mapping_target_, work_precision_);
      Logger->info("No mapping target: sources are not tone-mapped (peak {:.0f} nits from video #{})", source_peak,
                   peak_id);
    }
    declared_peak_nits_ = main_is_hdr ? mapping_target_.peak_nits : 0.0f;
    break;
  }
  if (expects_hdr_output_) {
    Logger->info("HDR output: mastering peak declared to the system is {:.0f} nits", declared_peak_nits_);
  }
}

void FrameRenderer::SetHdrVividEnabled(bool enabled) {
  if (hdrvivid_enabled_ == enabled) {
    return;
  }
  hdrvivid_enabled_ = enabled;
  if (enabled) {
    if (!hdrvivid_shader_) {
      hdrvivid_shader_ = std::make_unique<HDRVividShader>();
    }
    ResolveHdrVividParams();
    if (hdrvivid_params_.empty()) {
      // Not an error: the current input may simply have no dynamic metadata.
      Logger->info("HDR Vivid dynamic rendering enabled (mapping target peak {:.0f} nits); no source currently "
                   "carries HDR Vivid metadata",
                   mapping_target_.peak_nits);
    }
  } else {
    hdrvivid_params_.clear();
    Logger->info("HDR Vivid dynamic rendering disabled");
  }
}

void FrameRenderer::ResolveHdrVividParams() {
  if (!hdrvivid_enabled_ || !hdrvivid_shader_) {
    return;
  }
  hdrvivid_params_.clear();
  for (const auto &[id, meta] : hdrvivid_metadata_) {
    HDRVividShaderParams params;
    const auto it = hdrvivid_mastering_nits_.find(id);
    const double mastering = it == hdrvivid_mastering_nits_.end() ? 1000.0 : it->second;
    // Single target mapping exit: every source, Vivid or not, is solved against
    // the same common display mapping target peak.
    SolveHDRVividShaderParams(meta, mapping_target_.peak_nits, mastering, &params);
    hdrvivid_params_[id] = params;
    if (!hdrvivid_logged_) {
      Logger->info("Video #{}: HDR Vivid 1.0 dynamic metadata detected; tone mapping to the common display target "
                   "({:.0f} nits) on the GPU",
                   id, mapping_target_.peak_nits);
      hdrvivid_logged_ = true;
    }
  }
}

const struct pl_hook *FrameRenderer::HdrVividHookForVideo(int video_id) {
  if (!hdrvivid_enabled_ || !hdrvivid_shader_) {
    return nullptr;
  }
  const auto it = hdrvivid_params_.find(video_id);
  if (it == hdrvivid_params_.end() || !it->second.valid) {
    return nullptr;
  }
  // The hook is consumed synchronously by the very next pl_render_image() call
  // for this source, so updating the single shared shader here cannot leak one
  // source's curve into another.
  hdrvivid_shader_->SetParams(it->second);
  return hdrvivid_shader_->Hook();
}

const struct pl_frame &FrameRenderer::GetLinearResult() const { return display_render_->LinearResult(); }

uint64_t FrameRenderer::MappingPassCount(int video_id) const {
  return display_render_ ? display_render_->MappingPassCount(video_id) : 0;
}

uint64_t FrameRenderer::HdrVividApplyCount() const {
  return display_render_ ? display_render_->HdrVividApplyCount() : 0;
}

void FrameRenderer::ResetMappingCounts() {
  if (display_render_) {
    display_render_->ResetMappingCounts();
  }
}

pl_tex FrameRenderer::GetLinearResultTexture() const { return display_render_->LinearResultTexture(); }

WorkPrecision FrameRenderer::GetLinearResultPrecision() const { return display_render_->ResultPrecision(); }

WorkPrecision FrameRenderer::GetDisPrecision() const { return display_render_->DisPrecision(); }

WorkPrecision FrameRenderer::GetRefPrecision() const { return display_render_->RefPrecision(); }

void FrameRenderer::SetMappingTarget(const MappingTarget &target, WorkPrecision precision) {
  ValidateMappingTarget(target);
  mapping_target_ = target;
  work_precision_ = precision;
  if (clip_hook_) {
    clip_hook_->SetPeak(mapping_target_.NitsToNorm(mapping_target_.peak_nits));
  }
  if (display_render_) {
    display_render_->SetMappingTarget(mapping_target_, work_precision_);
  }
  ResolveHdrVividParams();
}

void FrameRenderer::SetICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id) {
  if (!icc_enabled_) {
    // Monitoring mode (V2.1 D2): no application-side ICC is loaded or applied.
    // Report explicitly instead of silently dropping the profile.
    static bool logged = false;
    if (!logged) {
      Logger->info("Application ICC is disabled in monitoring mode; profile '{}' is ignored", source);
      logged = true;
    }
    return;
  }
  OpenICCProfile(std::move(data), source, video_id);
}

void FrameRenderer::SetIccEnabled(bool enabled) {
  if (icc_enabled_ == enabled) {
    return;
  }
  icc_enabled_ = enabled;
  if (!enabled) {
    // Close the GPU-backed ICC objects so nothing can apply them afterwards.
    CloseICCProfiles();
  }
  Logger->info("Application ICC {}", enabled ? "enabled" : "disabled");
}

void FrameRenderer::CloseICCProfiles() {
  if (global_icc_object_) {
    pl_icc_close(&global_icc_object_);
  }
  for (auto &[id, obj] : per_video_icc_objects_) {
    if (obj) {
      pl_icc_close(&obj);
    }
  }
  per_video_icc_objects_.clear();
  per_video_icc_data_.clear();
  global_icc_data_.clear();
  last_icc_target_luma_ = -1.0f;
}

void FrameRenderer::OpenICCProfile(std::vector<uint8_t> data, const std::string &source, std::optional<int> video_id) {
  if (data.empty()) {
    Logger->warn("Empty ICC profile data from '{}'", source);
    return;
  }

  ICCProfileInfo profile_info;
  profile_info.data = std::move(data);
  profile_info.source = source;
  ParseICCProfileHeader(profile_info);

  Logger->debug("Loaded ICC profile from '{}'", source);
  Logger->debug("  Version: {}.{}, Color Space: {}, PCS: {}, Header Intent: {}, Size: {} bytes",
                profile_info.version_major, profile_info.version_minor, profile_info.color_space,
                profile_info.connection_space, profile_info.header_intent, profile_info.data.size());

  size_t data_size = profile_info.data.size();
  struct pl_icc_profile pl_profile{};

  if (video_id.has_value()) {
    int id = video_id.value();
    per_video_icc_data_[id] = std::move(profile_info.data);
    pl_profile.data = per_video_icc_data_[id].data();
    pl_profile.len = per_video_icc_data_[id].size();
    pl_icc_profile_compute_signature(&pl_profile);

    // Close the old ICC object (if any)
    if (per_video_icc_objects_.count(id) && per_video_icc_objects_[id]) {
      pl_icc_close(&per_video_icc_objects_[id]);
    }
    Logger->debug("Opening ICC profile for video {} ...", id);
    per_video_icc_objects_[id] = pl_icc_open(context_->log(), &pl_profile, nullptr);
    if (!per_video_icc_objects_[id]) {
      Logger->error("Failed to open ICC profile for video {}", id);
      return;
    }
    Logger->debug("ICC profile assigned to video {}: {} ({} bytes, {} → {}), gamma={:.3f}, primaries={}", id,
                  profile_info.name, data_size, profile_info.color_space, profile_info.connection_space,
                  per_video_icc_objects_[id]->gamma,
                  pl_color_primaries_name(per_video_icc_objects_[id]->containing_primaries));
  } else {
    global_icc_data_ = std::move(profile_info.data);
    pl_profile.data = global_icc_data_.data();
    pl_profile.len = global_icc_data_.size();
    pl_icc_profile_compute_signature(&pl_profile);

    // Close the old ICC object (if any)
    if (global_icc_object_) {
      pl_icc_close(&global_icc_object_);
    }
    Logger->debug("Opening ICC profile globally ...");
    global_icc_object_ = pl_icc_open(context_->log(), &pl_profile, nullptr);
    if (!global_icc_object_) {
      Logger->error("Failed to open ICC profile globally");
      return;
    }
    Logger->debug("ICC profile assigned globally: {} ({} bytes, {} → {}), gamma={:.3f}, primaries={}",
                  profile_info.name, global_icc_data_.size(), profile_info.color_space, profile_info.connection_space,
                  global_icc_object_->gamma, pl_color_primaries_name(global_icc_object_->containing_primaries));
  }
}

pl_icc_object FrameRenderer::GetICCObjectForVideo(int video_id) const {
  if (!icc_enabled_) {
    return nullptr;
  }
  auto it = per_video_icc_objects_.find(video_id);
  if (it != per_video_icc_objects_.end())
    return it->second;
  if (global_icc_object_)
    return global_icc_object_;
  return nullptr;
}

bool FrameRenderer::GetICCTargetColorSpace(struct pl_color_space *out) const {
  if (!icc_enabled_ || !global_icc_object_) {
    return false;
  }
  *out = {};
  out->primaries = global_icc_object_->containing_primaries;
  if (global_icc_object_->csp.transfer) {
    out->transfer = global_icc_object_->csp.transfer;
  }
  return true;
}

const struct pl_frame *FrameRenderer::GetSourceFrame(int video_id) const {
  auto it = av_frames_.find(video_id);
  if (it == av_frames_.end()) {
    return nullptr;
  }
  return it->second.GetPLFrame();
}

std::vector<int> FrameRenderer::FeedFrames(const std::map<int, std::shared_ptr<AVFrame>> &frames) {
  std::vector<int> failed_ids;
  std::map<int, HDRVividMetadata> parsed_vivid;
  std::map<int, double> parsed_mastering;
  for (auto [i, frame] : frames) {
    try {
      // Dynamic HDR metadata belongs to the decode result, not to the output:
      // parse it here, once, and keep only the raw CUVA codes. The AVFrame is
      // never handed to a consumer.
      HDRVividMetadata meta;
      if (ParseHDRVividMetadata(frame.get(), &meta)) {
        parsed_vivid[i] = meta;
        parsed_mastering[i] = MasteringDisplayMaxLuminance(frame.get());
      }
      auto search = av_frames_.find(i);
      if (search != av_frames_.end()) {
        av_frames_.at(i).UpdateAVFrame(frame, context_->hw_device());
      } else {
        // Avoid creating a temporary Frame that gets immediately destroyed
        av_frames_.try_emplace(i, frame, context_->gpu(), context_->hw_device());
      }
      pl_frames_[i] = av_frames_.at(i).GetPLFrame();

      // libplacebo's pl_frame_from_avframe() forces full range for RGB pixel
      // formats (FFmpeg treats color_range as YUV-only), so a limited-range tag
      // on an RGB frame, e.g. from setparams=range=limited, is silently dropped.
      if (frame->color_range == AVCOL_RANGE_MPEG && pl_frames_[i]->repr.sys == PL_COLOR_SYSTEM_RGB &&
          rgb_limited_warned_.insert(i).second) {
        const int depth = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format))->comp[0].depth;
        const long long black = 16LL << (depth - 8);
        const long long span = 219LL << (depth - 8);
        const long long max = (1LL << depth) - 1;
        Logger->warn("Video #{}: RGB frame is tagged as limited range (tv), but libplacebo always treats RGB as full "
                     "range, so the tag is ignored and the picture is shown as full range. To display limited-range "
                     "RGB, expand it in the filter, e.g. lutrgb=r='clip(floor((val-{})*{}/{}+0.5),0,{})' (same for g "
                     "and b)",
                     i, black, max, span, max);
      }

      // Complete a missing mastering display peak when the user asked for it
      // (--default-mastering-nits), e.g. DPX/JPEG XL whose peak FFmpeg does not
      // read. A source that has a peak is left alone, and so is MaxCLL: a MaxCLL
      // above the mastering peak (an unclipped 1010 on a 1000 master) is normal
      // and only reported.
      struct pl_color_space *csp = &pl_frames_[i]->color;
      if (default_mastering_nits_ > 0.0 && pl_color_space_is_hdr(csp) && csp->hdr.max_luma <= 0.0f) {
        csp->hdr.max_luma = static_cast<float>(default_mastering_nits_);
        if (default_mastering_logged_.insert(i).second) {
          Logger->info("Video #{}: no mastering peak, using --default-mastering-nits {:.0f}", i,
                       default_mastering_nits_);
        }
        if (csp->hdr.max_cll > csp->hdr.max_luma && maxcll_warned_.insert(i).second) {
          Logger->warn("Video #{}: MaxCLL {:.0f} nits exceeds the mastering peak {:.0f} nits "
                       "(--default-mastering-nits)",
                       i, csp->hdr.max_cll, csp->hdr.max_luma);
        }
      }
    } catch (const texture_format_error &e) {
      Logger->warn("Video #{}: {}", i, e.what());
      failed_ids.push_back(i);
    }
  }
  if (!failed_ids.empty()) {
    return failed_ids;
  }

  struct pl_frame *main = pl_frames_.at(main_id_);
  if (!declared_peak_resolved_) {
    ResolveDeclaredPeak();
  }
  // The colorspace hint touches the swapchain, which lives in Window; it is
  // invoked here while the caller still serializes Feed/Render. The mapping
  // target decides the output; the main video only decides it without a target.
  // With a HDR output the declared mastering peak describes this renderer's
  // output; MaxCLL/MaxFALL are passed on from the main video unchanged.
  if (colorspace_hint_) {
    const bool main_is_hdr = pl_color_space_is_hdr(&main->color);
    struct pl_color_space hint = main->color;
    switch (target_kind_) {
    case TargetKind::Sdr:
      if (main_is_hdr) {
        hint = pl_color_space_srgb;
      }
      break;
    case TargetKind::HdrNits:
      if (!main_is_hdr) {
        hint = pl_color_space_hdr10;
      }
      hint.hdr.max_luma = declared_peak_nits_;
      break;
    case TargetKind::None:
      if (main_is_hdr) {
        hint.hdr.max_luma = declared_peak_nits_;
      }
      break;
    }
    colorspace_hint_(&hint);
  }

  // Override the SDR white level of SDR sources shown in a HDR output
  if (sdr_white_on_hdr_ > 0 && expects_hdr_output_) {
    static bool sdr_white_logged = false;
    for (auto &[id, frame] : pl_frames_) {
      if (!pl_color_space_is_hdr(&frame->color)) {
        frame->color.hdr.max_luma = sdr_white_on_hdr_;
        if (!sdr_white_logged) {
          Logger->info("Video #{}: SDR white on HDR set to {:.0f} nits", id, sdr_white_on_hdr_);
        }
      }
    }
    sdr_white_logged = true;
  }

  display_render_->UpdateMainFrame(main);

  // Replace the stored dynamic metadata with this feed's result (a source that
  // lost its metadata must lose the curve) and re-solve against the current
  // common target.
  hdrvivid_metadata_.swap(parsed_vivid);
  hdrvivid_mastering_nits_.swap(parsed_mastering);
  if (hdrvivid_enabled_ && !hdrvivid_metadata_.empty()) {
    ++hdrvivid_frame_count_;
  }
  ResolveHdrVividParams();
  return failed_ids;
}

// Format seconds as HH:MM:SS
static std::string format_time(float seconds) {
  int s = static_cast<int>(seconds);
  int h = s / 3600;
  int m = (s % 3600) / 60;
  int sec = s % 60;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, sec);
  return buf;
}

void FrameRenderer::Render(struct pl_frame &target, const DisplayState &state) {
  // Fused convenience path used by the offscreen tests and single-target
  // callers: it keeps the historical behavior of following the presentation
  // target primaries. The production desktop path goes through the coordinator,
  // which composes with an explicit fixed mapping target and presents with a
  // preview rectangle.
  if (target.color.primaries != PL_COLOR_PRIM_UNKNOWN) {
    mapping_target_.primaries = target.color.primaries;
    display_render_->SetMappingTarget(mapping_target_, work_precision_);
  }
  const int width = static_cast<int>(target.planes[0].texture->params.w);
  const int height = static_cast<int>(target.planes[0].texture->params.h);
  RenderScene(width, height, state);
  PresentToTarget(target, state, EncodeRect{});
}

void FrameRenderer::RenderScene(int width, int height, const DisplayState &state) {
  if (width <= 0 || height <= 0) {
    throw std::invalid_argument(fmt::format("FrameRenderer::RenderScene: invalid canvas size {}x{}", width, height));
  }
  if (pl_frames_.empty()) {
    throw std::logic_error("FrameRenderer::RenderScene called before FeedFrames");
  }
  // The mapping target is an explicit, fixed input of the coordinator; it is not
  // re-derived from any presentation target here (V2-1 handoff #5).
  display_render_->RenderLinear(width, height, pl_frames_, state);
  scene_valid_ = true;
  scene_width_ = width;
  scene_height_ = height;
  if (validate_linear_result_) {
    ValidateLinearResultFinite();
  }
}

pl_tex FrameRenderer::ComposeOsdOverlay(int target_w, int target_h, const EncodeRect &preview,
                                        const DisplayState &state) {
  if (!state.show_osd || !osd_manager_) {
    return nullptr;
  }
  // The OSD belongs to the desktop preview: position every entry relative to the
  // presented (letterboxed) rectangle, not the raw window, so it stays inside
  // the image.
  const int px = static_cast<int>(preview.x * target_w);
  const int py = static_cast<int>(preview.y * target_h);
  const int pw = static_cast<int>(preview.w * target_w);
  const int ph = static_cast<int>(preview.h * target_h);
  const int margin = static_cast<int>(kOsdMargin * pixel_density_);

  std::vector<OsdEntry> entries;
  switch (mode_) {
  case DisplayMode::Fill: {
    int id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.fill.id;
    auto it = state.osd_infos.find(id);
    if (it != state.osd_infos.end()) {
      entries.push_back({it->second.Format(), px + margin, py + margin});
    }
  } break;
  case DisplayMode::Slide: {
    int left_id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.slide.left_id;
    int right_id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.slide.right_id;
    auto it_left = state.osd_infos.find(left_id);
    if (it_left != state.osd_infos.end()) {
      entries.push_back({it_left->second.Format(), px + margin, py + margin});
    }
    auto it_right = state.osd_infos.find(right_id);
    if (it_right != state.osd_infos.end()) {
      std::string text = it_right->second.Format();
      int text_w = osd_manager_->GetTextWidth(text);
      int x = px + pw - text_w - margin;
      if (x < px + margin)
        x = px + margin;
      entries.push_back({text, x, py + margin});
    }
  } break;
  case DisplayMode::Grid: {
    float line_width = kLineWidth * pixel_density_;
    float cell_width = (pw - (state.grid.cols - 1) * line_width) / state.grid.cols;
    float cell_height = (ph - (state.grid.rows - 1) * line_width) / state.grid.rows;
    for (int row = 0; row < state.grid.rows; row++) {
      for (int col = 0; col < state.grid.cols; col++) {
        int index = row * state.grid.cols + col;
        if (index >= static_cast<int>(state.ids.size()))
          continue;
        int id = state.show_ref && state.ref_id.has_value() ? state.ref_id.value() : state.ids[index];
        auto it = state.osd_infos.find(id);
        if (it != state.osd_infos.end()) {
          int x = px + static_cast<int>(col * (cell_width + line_width) + margin);
          int y = py + static_cast<int>(row * (cell_height + line_width) + margin);
          entries.push_back({it->second.Format(), x, y});
        }
      }
    }
  } break;
  }

  // Bottom-right corner: time at mouse position / total duration
  if (state.mouse_time_s >= 0 && state.total_duration_s > 0) {
    std::string time_text = format_time(state.mouse_time_s) + " / " + format_time(state.total_duration_s);
    int text_w = osd_manager_->GetTextWidth(time_text);
    int text_h = osd_manager_->GetTextHeight();
    int bar_height = static_cast<int>(kProgressBarHeight * pixel_density_);
    int x = px + pw - text_w - margin;
    int y = py + ph - text_h - bar_height - margin;
    if (x < px + margin)
      x = px + margin;
    entries.push_back({time_text, x, y});
  }

  // Bottom-center: playback FPS
  {
    std::string fps_text = fmt::format("Playback FPS: {:.1f} / Main: {:.1f}", state.playback_fps, state.main_fps);
    int text_w = osd_manager_->GetTextWidth(fps_text);
    int text_h = osd_manager_->GetTextHeight();
    int bar_height = static_cast<int>(kProgressBarHeight * pixel_density_);
    int x = px + (pw - text_w) / 2;
    int y = py + ph - text_h - bar_height - margin;
    if (x < px + margin)
      x = px + margin;
    entries.push_back({fps_text, x, y});
  }

  return osd_manager_->ComposeOverlay(target_w, target_h, entries, state.playback_progress);
}

void FrameRenderer::PresentToTarget(struct pl_frame &target, const DisplayState &state, const EncodeRect &preview) {
  if (!scene_valid_) {
    throw std::logic_error("FrameRenderer::PresentToTarget called before RenderScene");
  }
  if (!target.planes[0].texture) {
    throw std::invalid_argument("FrameRenderer::PresentToTarget: null presentation target");
  }
  const int target_w = static_cast<int>(target.planes[0].texture->params.w);
  const int target_h = static_cast<int>(target.planes[0].texture->params.h);
  // Text OSD is desktop-only and is blended after the conversion, so the public
  // display-linear result never contains burned-in text.
  pl_tex osd_overlay = ComposeOsdOverlay(target_w, target_h, preview, state);
  display_render_->SetOsdOverlay(osd_overlay);

  const struct pl_frame &linear = display_render_->LinearResult();
  if (osd_overlay) {
    // The desktop target is usually the swapchain image, which is not
    // sampleable, so the encoded result must be staged in a sampleable copy
    // before the text overlay can be blended onto the real target.
    struct pl_frame staged = target;
    staged.planes[0].texture = display_render_->PrepareOsdStaging(target.planes[0].texture);
    if (!EncodeToTarget(staged, linear, preview)) {
      display_render_->PresentTo(staged, &linear, &preview);
    }
    display_render_->PlotOsdText(target.planes[0].texture, staged.planes[0].texture, target.color.transfer);
    return;
  }
  if (!EncodeToTarget(target, linear, preview)) {
    display_render_->PresentTo(target, &linear, &preview);
  }
}

void FrameRenderer::ValidateLinearResultFinite() {
  if (!scene_valid_) {
    throw std::logic_error("FrameRenderer::ValidateLinearResultFinite called before RenderScene");
  }
  const struct pl_frame &linear = display_render_->LinearResult();
  pl_tex tex = linear.planes[0].texture;
  if (!tex || !tex->params.host_readable) {
    throw std::logic_error("FrameRenderer: the linear result is not host-readable; cannot validate NaN/Inf");
  }
  // NaN/Inf is a hard error (V2.1 3.2): the shared gate downloads and scans the
  // common result instead of letting a clamp hide the problem.
  ValidateLinearTextureFinite(context_->gpu(), tex);
}

bool FrameRenderer::EncodeToTarget(struct pl_frame &target, const struct pl_frame &linear, const EncodeRect &preview) {
  if (!encoder_ || !target.planes[0].texture || !linear.planes[0].texture) {
    return false;
  }
  // The encoder is transfer-only: it may only run when the target already uses
  // the mapping target's primaries (BT.2020/PQ). A BT.709 SDR target needs a gamut
  // conversion, which the color-managed presentation hop performs.
  if (target.color.primaries != mapping_target_.primaries) {
    return false;
  }
  EncodeParams params;
  switch (target.color.transfer) {
  case PL_COLOR_TRC_PQ:
    params.transfer = OutputTransfer::PQ;
    break;
  case PL_COLOR_TRC_BT_1886:
    params.transfer = OutputTransfer::SDR_BT1886;
    break;
  default:
    return false;
  }
  params.sdr_white_nits = mapping_target_.sdr_white_nits;
  params.black_nits = mapping_target_.black_nits;
  encoder_->Encode(target.planes[0].texture, linear.planes[0].texture, params, preview);
  return true;
}

void FrameRenderer::RecalibrateICCParams(const struct pl_color_space *target_color) {
  if (!global_icc_object_ && per_video_icc_objects_.empty())
    return;

  // Similar to plplay's render_frame approach:
  // Compute max_luma from the target's final color space, then use
  // pl_icc_update to recalibrate the ICC LUT (libplacebo skips if params unchanged)
  float target_luma = 0.0f;
  struct pl_nominal_luma_params luma_params{};
  luma_params.metadata = PL_HDR_METADATA_HDR10;
  luma_params.scaling = PL_HDR_NITS;
  luma_params.color = target_color;
  luma_params.out_max = &target_luma;
  pl_color_space_nominal_luma_ex(&luma_params);

  // Skip if params unchanged (libplacebo checks internally too, but early skip avoids redundant logging)
  if (target_luma == last_icc_target_luma_)
    return;

  Logger->debug("ICC recalibrating: target_luma {:.1f} → {:.1f} nits", last_icc_target_luma_, target_luma);
  last_icc_target_luma_ = target_luma;

  // Build update_params from each ICC object's parsed params, only updating max_luma.
  // Note: cannot use the original icc_params_.intent because PL_INTENT_AUTO (-1) is resolved
  // to the actual intent value from the ICC header inside pl_icc_open. Passing AUTO directly
  // would cause pl_icc_update to detect a param mismatch and trigger an unnecessary icc_reopen.
  if (global_icc_object_) {
    struct pl_icc_params update_params = global_icc_object_->params;
    update_params.max_luma = target_luma;
    if (!pl_icc_update(context_->log(), &global_icc_object_, nullptr, &update_params)) {
      Logger->error("Failed to recalibrate global ICC object");
    }
  }

  for (auto &[video_id, icc_obj] : per_video_icc_objects_) {
    if (icc_obj) {
      struct pl_icc_params update_params = icc_obj->params;
      update_params.max_luma = target_luma;
      if (!pl_icc_update(context_->log(), &icc_obj, nullptr, &update_params)) {
        Logger->error("Failed to recalibrate ICC object for video {}", video_id);
      }
    }
  }
}
