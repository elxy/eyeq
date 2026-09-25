#pragma once

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

namespace EYEQ {

/*
 * Transfer function of the wire encoding. This is a property of the connection,
 * not of the mapping target: PQ for an HDR link, BT.1886 for an SDR link.
 */
enum class OutputTransfer { PQ, SDR_BT1886 };

const char *ToString(OutputTransfer transfer);

/*
 * Parameters of the encode step.
 *
 * The display-linear input is in PL_HDR_NORM units (1.0 == 203 cd/m^2). PQ is
 * encoded absolutely (nits = norm * 203, then the ST2084 OETF); the SDR BT.1886
 * inverse uses the defined black and the independent SDR white level, so it is
 * *not* a bare pow(x, 1/2.4).
 */
struct EncodeParams {
  OutputTransfer transfer = OutputTransfer::SDR_BT1886;
  float sdr_white_nits = 203.0f;
  float black_nits = 0.0f;
  float gamma = 2.4f; // BT.1886 display gamma
};

// ---------------------------------------------------------------------------
// CPU oracles. These are the independent formula reference the GPU shader is
// checked against; they are also used by the encode/decode consistency tests.
// ---------------------------------------------------------------------------

// ST2084 (PQ) OETF: absolute luminance in cd/m^2 (10000 == code 1.0).
float PqEncodeNits(float nits);
// Inverse of PqEncodeNits.
float PqDecodeNits(float code);

// BT.1886 inverse EOTF with an explicit black/white and display gamma.
//   b = (black/white)^(1/gamma)
//   code = ( (max(nits, black)/white)^(1/gamma) - b ) / (1 - b)
float Bt1886EncodeNits(float nits, float black_nits, float white_nits, float gamma);
// Inverse of Bt1886EncodeNits.
float Bt1886DecodeNits(float code, float black_nits, float white_nits, float gamma);

// Applies the wire transfer to a display-linear PL_HDR_NORM value and clamps to
// the full-range [0, 1] code interval (the single range compression of the
// pipeline; it is never applied anywhere else). Throws std::invalid_argument on
// a non-finite input: NaN/Inf is a hard error, never clamped away (V2.1 3.2).
float EncodeNormToCode(float norm, const EncodeParams &params);
// Inverse of EncodeNormToCode, without the final clamp. Throws on a non-finite
// input.
float DecodeCodeToNorm(float code, const EncodeParams &params);

// Downloads a display-linear texture (F16 or F32) and throws std::runtime_error
// when any sample is NaN/Inf. This is the explicit product-side NaN/Inf gate: the
// common result must be finite before it can be presented or encoded. A full-frame
// readback is acceptable only because V2-2 is still-only; the continuous-play
// checkpoint must replace it with a GPU-side check.
void ValidateLinearTextureFinite(pl_gpu gpu, pl_tex tex);

// Normalized destination rectangle the encoded image is written into. The
// default covers the whole target; a smaller rectangle letterboxes the image and
// fills the rest with black (used by the desktop presenter to place the fixed
// canvas inside the preview rectangle).
struct EncodeRect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 1.0f;
  float h = 1.0f;
};

/*
 * Explicit, independently testable display-linear -> wire-transfer encoder.
 *
 * It converts a full-range RGB PL_COLOR_TRC_LINEAR texture into the non-linear
 * full-range RGB texture the FramePacker consumes. It never tone maps, never
 * gamut maps and never touches source metadata: only the per-channel transfer
 * function and the final [0,1] clamp run, and the result is checked against the
 * CPU oracles above. The destination may differ in size from the source: the
 * linear image is sampled (bilinear) into @p rect.
 */
class OutputEncoder {
public:
  OutputEncoder(pl_log log, pl_gpu gpu);
  ~OutputEncoder();

  OutputEncoder(const OutputEncoder &) = delete;
  OutputEncoder &operator=(const OutputEncoder &) = delete;
  OutputEncoder(OutputEncoder &&) = delete;
  OutputEncoder &operator=(OutputEncoder &&) = delete;

  // Encodes @p linear (full-range RGB LINEAR) into @p dst. Both textures must
  // live on the same pl_gpu. When @p rect does not cover the whole target the
  // area outside it is set to black.
  // @p timer, when non-null, receives the GPU execution time of this dispatch
  // (distinct from host/queue wait). Used by the precision/perf evidence.
  void Encode(pl_tex dst, const pl_tex linear, const EncodeParams &params, const EncodeRect &rect = EncodeRect{},
              pl_timer timer = nullptr);

private:
  pl_gpu gpu_ = nullptr;
  pl_dispatch dp_ = nullptr;
};

} // namespace EYEQ
