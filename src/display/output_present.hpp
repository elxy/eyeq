#pragma once

#include <libplacebo/colorspace.h>
#include <libplacebo/filters.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "output_encode.hpp"
#include "render.hpp"

namespace EYEQ {

// libplacebo filter for a --scale-method / --plane-scale-method value.
const struct pl_filter_config *FilterConfig(ScaleMethod method);

/*
 * Scaling policy shared by every scaling step (source -> canvas, canvas ->
 * output): up- and downscaling use the chosen method, scaling happens in the
 * signal's own encoding (no linear-light or sigmoid scaling), and `nearest`
 * really samples one source pixel when downscaling (no anti-aliasing).
 */
void ConfigureScaling(struct pl_render_params &params, ScaleMethod method, ScaleMethod plane_method);

/*
 * The single output stage of the display-linear canvas, shared by the desktop
 * window and the monitor output.
 *
 * The canvas is encoded 1:1 by OutputEncoder (transfer only), then libplacebo
 * only rescales the encoded codes into the destination rectangle: source and
 * target have the same color space, so no color processing runs, and the
 * scaling happens in the output encoding (e.g. PQ codes for a PQ output). When
 * the canvas maps 1:1 onto the rectangle the encode writes the target directly.
 * Outside the rectangle the target is black.
 */
class OutputPresenter {
public:
  OutputPresenter(pl_log log, pl_gpu gpu, ScaleMethod method);
  ~OutputPresenter();

  OutputPresenter(const OutputPresenter &) = delete;
  OutputPresenter &operator=(const OutputPresenter &) = delete;
  OutputPresenter(OutputPresenter &&) = delete;
  OutputPresenter &operator=(OutputPresenter &&) = delete;

  /*
   * Encodes @p canvas (full-range RGB LINEAR, sampleable) into @p target, scaled
   * into the normalized rectangle @p rect. @p target.color must already use the
   * canvas primaries; its transfer selects @p params.transfer by the caller.
   * Throws std::runtime_error when the target cannot be cleared (no blit_dst)
   * for a letterboxed scale, or on a libplacebo failure.
   */
  void Present(const struct pl_frame &target, pl_tex canvas, const EncodeParams &params, const EncodeRect &rect);

  // Diagnostics: how many presents were scaled through the intermediate texture.
  uint64_t scaled_count() const { return scaled_count_; }

private:
  pl_gpu gpu_ = nullptr;
  OutputEncoder encoder_;
  pl_renderer renderer_ = nullptr;
  pl_tex encoded_ = nullptr; // canvas-sized encoded codes (RGBA16 UNORM)
  struct pl_render_params params_;
  uint64_t scaled_count_ = 0;
};

// Maps an output transfer (window swapchain or monitor wire) to the encoder's.
// Throws std::runtime_error for a transfer the encoder does not produce.
OutputTransfer OutputTransferFor(enum pl_color_transfer transfer);

} // namespace EYEQ
