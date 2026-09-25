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
// pipeline; it is never applied anywhere else).
float EncodeNormToCode(float norm, const EncodeParams &params);
// Inverse of EncodeNormToCode, without the final clamp.
float DecodeCodeToNorm(float code, const EncodeParams &params);

/*
 * Explicit, independently testable display-linear -> wire-transfer encoder.
 *
 * It converts a full-range RGB PL_COLOR_TRC_LINEAR texture into the non-linear
 * full-range RGB texture the FramePacker consumes. It never tone maps, never
 * gamut maps and never touches source metadata: only the per-channel transfer
 * function and the final [0,1] clamp run, and the result is checked against the
 * CPU oracles above.
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
  // live on the same pl_gpu and have matching dimensions.
  void Encode(pl_tex dst, const pl_tex linear, const EncodeParams &params);

private:
  pl_gpu gpu_ = nullptr;
  pl_dispatch dp_ = nullptr;
};

} // namespace EYEQ
