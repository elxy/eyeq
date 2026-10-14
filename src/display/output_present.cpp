#include "display/output_present.hpp"

#include <cmath>
#include <stdexcept>

#include <spdlog/fmt/fmt.h>

namespace EYEQ {

const struct pl_filter_config *FilterConfig(ScaleMethod method) {
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
  }
  throw std::invalid_argument("unknown ScaleMethod");
}

void ConfigureScaling(struct pl_render_params &params, ScaleMethod method, ScaleMethod plane_method) {
  params.upscaler = FilterConfig(method);
  params.downscaler = FilterConfig(method);
  params.plane_upscaler = FilterConfig(plane_method);
  params.plane_downscaler = FilterConfig(plane_method);
  // Scale in the signal's own encoding, like common image viewers.
  params.sigmoid_params = nullptr;
  params.disable_linear_scaling = true;
  // libplacebo widens a downscaling kernel for anti-aliasing, which turns
  // nearest into an area average. nearest means one source pixel per output
  // pixel. This applies to every scaler of the pass, chroma included.
  params.skip_anti_aliasing = method == ScaleMethod::Nearest;
}

OutputTransfer OutputTransferFor(enum pl_color_transfer transfer) {
  switch (transfer) {
  case PL_COLOR_TRC_PQ:
    return OutputTransfer::PQ;
  case PL_COLOR_TRC_BT_1886:
    return OutputTransfer::SDR_BT1886;
  case PL_COLOR_TRC_SRGB:
    return OutputTransfer::SRGB;
  case PL_COLOR_TRC_HLG:
    return OutputTransfer::HLG;
  default:
    throw std::runtime_error(fmt::format("output transfer '{}' is not supported: pass --target-display-nits <nits|sdr>",
                                         pl_color_transfer_name(transfer)));
  }
}

OutputPresenter::OutputPresenter(pl_log log, pl_gpu gpu, ScaleMethod method) : gpu_(gpu), encoder_(log, gpu) {
  renderer_ = pl_renderer_create(log, gpu_);
  if (!renderer_) {
    throw std::runtime_error("OutputPresenter: pl_renderer_create failed");
  }
  // A pure rescale of already encoded codes: no dithering, no peak detection,
  // no border clear (Present clears the letterbox itself).
  params_ = pl_render_fast_params;
  params_.dither_params = nullptr;
  params_.peak_detect_params = nullptr;
  params_.border = PL_CLEAR_SKIP;
  ConfigureScaling(params_, method, method);
}

OutputPresenter::~OutputPresenter() {
  if (encoded_) {
    pl_tex_destroy(gpu_, &encoded_);
  }
  if (renderer_) {
    pl_renderer_destroy(&renderer_);
  }
}

void OutputPresenter::Present(const struct pl_frame &target, pl_tex canvas, const EncodeParams &params,
                              const EncodeRect &rect) {
  pl_tex dst = target.planes[0].texture;
  if (!dst || !canvas) {
    throw std::invalid_argument("OutputPresenter::Present: null texture");
  }
  const float tw = static_cast<float>(dst->params.w);
  const float th = static_cast<float>(dst->params.h);
  const float x0 = rect.x * tw;
  const float y0 = rect.y * th;
  const float w = rect.w * tw;
  const float h = rect.h * th;
  const auto is_int = [](float v) { return std::fabs(v - std::round(v)) < 1e-3f; };
  const bool one_to_one = std::fabs(w - static_cast<float>(canvas->params.w)) < 1e-3f &&
                          std::fabs(h - static_cast<float>(canvas->params.h)) < 1e-3f && is_int(x0) && is_int(y0);
  if (one_to_one) {
    // Every target pixel center hits a canvas texel center: the encode is exact.
    encoder_.Encode(dst, canvas, params, rect);
    return;
  }

  ++scaled_count_;
  pl_fmt fmt =
      pl_find_fmt(gpu_, PL_FMT_UNORM, 4, 16, 16,
                  static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR));
  if (!fmt) {
    throw std::runtime_error("OutputPresenter: the GPU has no sampleable/renderable/linear RGBA16 UNORM format");
  }
  struct pl_tex_params tex_params = {
      .w = canvas->params.w,
      .h = canvas->params.h,
      .format = fmt,
      .sampleable = true,
      .renderable = true,
  };
  if (!pl_tex_recreate(gpu_, &encoded_, &tex_params)) {
    throw std::runtime_error("OutputPresenter: failed to create the encoded intermediate texture");
  }
  encoder_.Encode(encoded_, canvas, params);

  const bool covers = rect.x <= 0.0f && rect.y <= 0.0f && rect.x + rect.w >= 1.0f && rect.y + rect.h >= 1.0f;
  if (!covers) {
    if (!dst->params.blit_dst) {
      throw std::runtime_error("OutputPresenter: a letterboxed target must be blittable (blit_dst) to clear it");
    }
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    pl_tex_clear(gpu_, dst, black);
  }

  // Source and target carry the same color space, so pl_render_image only
  // resamples the codes.
  struct pl_frame image = {};
  image.num_planes = 1;
  image.planes[0].texture = encoded_;
  image.planes[0].components = 3;
  image.planes[0].component_mapping[0] = 0;
  image.planes[0].component_mapping[1] = 1;
  image.planes[0].component_mapping[2] = 2;
  image.repr = pl_color_repr_rgb;
  image.repr.alpha = PL_ALPHA_NONE;
  image.color = target.color;

  struct pl_frame out = target;
  out.crop = {x0, y0, x0 + w, y0 + h};
  if (!pl_render_image(renderer_, &image, &out, &params_)) {
    throw std::runtime_error("OutputPresenter: pl_render_image failed (scale)");
  }
}

} // namespace EYEQ
