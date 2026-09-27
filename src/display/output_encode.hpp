#pragma once

#include <cstdint>

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
// common result must be finite before it can be presented or encoded. It performs a
// full-frame host readback, which is affordable for the desktop difference path and
// for tests but forbidden on the continuous playback/monitor path; use FiniteGate
// there.
void ValidateLinearTextureFinite(pl_gpu gpu, pl_tex tex);

// Diagnostics for the full-frame readback gate. Counts the calls made to
// ValidateLinearTextureFinite since the last reset. The continuous playback/monitor
// path must never trigger one (it uses FiniteGate); tests assert this counter stays
// at zero while driving that path.
uint64_t LinearTextureReadbackCount();
void ResetLinearTextureReadbackCount();

/*
 * GPU-side finiteness gate for the continuous playback/monitor path.
 *
 * It dispatches one compute reduction over the float texture: every texel is
 * fetched in the shader and any NaN/Inf sets a single flag word with atomicOr. Only
 * that one word is read back to the host, so the gate never downloads the full
 * frame (the acceptance requirement for continuous playback).
 *
 * Failure is a hard error, exactly like the CPU-side gate: a non-finite sample
 * throws std::runtime_error instead of being silently clamped or dropped. A
 * dispatch or readback failure also throws. The desktop difference path keeps using
 * the explicit ValidateLinearTextureFinite() readback; this class is the playback
 * replacement the code comment above refers to.
 *
 * The class owns its pl_dispatch and one 4-byte storable/host-readable flag buffer.
 * It is not thread-safe; like the rest of the output layer it is used from one
 * thread.
 */
class FiniteGate {
public:
  FiniteGate(pl_log log, pl_gpu gpu);
  ~FiniteGate();

  FiniteGate(const FiniteGate &) = delete;
  FiniteGate &operator=(const FiniteGate &) = delete;
  FiniteGate(FiniteGate &&) = delete;
  FiniteGate &operator=(FiniteGate &&) = delete;

  // Throws std::runtime_error when @p tex contains a NaN/Inf sample, when the
  // texture is unusable, or when the dispatch/readback fails. Never performs a
  // full-frame host readback.
  void Validate(pl_tex tex);

private:
  pl_gpu gpu_ = nullptr;
  pl_dispatch dp_ = nullptr;
  pl_buf flag_ = nullptr;
};

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
