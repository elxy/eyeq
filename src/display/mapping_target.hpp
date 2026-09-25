#pragma once

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>

#include "output/pixel_format.hpp"

namespace EYEQ {

/*
 * Texture storage precision for the display-linear working space.
 *
 * This is a functional choice, never a load-adaptive one (V2.1 3.5): the
 * difference-amplification path stores the mapped sources, the reference image
 * and the amplification result at F32 before the subtraction, plain display may
 * use F16 once its error budget is verified.
 */
enum class WorkPrecision { F16, F32 };

const char *ToString(WorkPrecision precision);

/*
 * The single display mapping target: the physical display conditions every
 * source is mapped to *before* any comparison. It is deliberately separate from
 * the wire encoding (PQ / SDR BT.1886, v210/r210, video range), which describes
 * the connection rather than the mapping.
 *
 * Units follow libplacebo's PL_HDR_NORM convention: neutral white RGB (1,1,1)
 * is PL_COLOR_SDR_WHITE (203 cd/m^2), so a 1000-nit neutral grey is ~4.926.
 * `peak_nits`/`black_nits` are the target display's physical range and
 * `sdr_white_nits` is the independent SDR diffuse-white level; none of them is
 * assumed to be 203 and the SDR white is never replaced by the peak.
 */
struct MappingTarget {
  enum pl_color_primaries primaries = PL_COLOR_PRIM_BT_709;
  float peak_nits = PL_COLOR_SDR_WHITE;
  float black_nits = 0.0f;
  float sdr_white_nits = PL_COLOR_SDR_WHITE;

  /*
   * The mapping target as a libplacebo color space: RGB, full range and
   * PL_COLOR_TRC_LINEAR, with the physical black/peak in `hdr`. This is the
   * "HDR mapping completed" description; the values are display-linear, not
   * scene-linear source data.
   */
  struct pl_color_space DisplayLinearColorSpace() const;

  /*
   * The wire encoding for this target, expressed as the color space the
   * standalone encoder compresses into. SDR uses the defined black/white so the
   * BT.1886 inverse is not a bare pow(x, 1/2.4); PQ is absolute and does not
   * depend on the black/white at all.
   */
  struct pl_color_space WireColorSpace(ColorTransfer wire) const;

  // Physical luminance (cd/m^2) <-> display-linear PL_HDR_NORM value.
  float NitsToNorm(float nits) const { return nits / PL_COLOR_SDR_WHITE; }
  float NormToNits(float norm) const { return norm * PL_COLOR_SDR_WHITE; }

  // Display-linear value of the target peak / black / SDR reference white.
  float PeakNorm() const { return NitsToNorm(peak_nits); }
  float BlackNorm() const { return NitsToNorm(black_nits); }
  float ReferenceWhiteNorm() const { return 1.0f; }
};

/*
 * Validates a mapping target: positive finite peak, finite black with
 * 0 <= black < peak and a positive finite SDR white. Throws
 * std::invalid_argument with a precise reason instead of silently clamping.
 */
void ValidateMappingTarget(const MappingTarget &target);

/*
 * Builds the mapping target for a resolved output profile. `target_nits` is the
 * physical display peak and `sdr_white_nits` is an explicit, independent
 * parameter: the old --target-display-nits override semantics (V2.1 8-2) is
 * never reused as the SDR white.
 */
MappingTarget MappingTargetFromColorProfile(const ColorProfile &profile, float sdr_white_nits);

// libplacebo color space matching a wire ColorTransfer for the given primaries.
enum pl_color_transfer PLTransferFor(ColorTransfer transfer);
enum pl_color_primaries PLPrimariesFor(ColorPrimaries primaries);

/*
 * GPU format for the display-linear work textures. The format is selected by
 * the requested precision (a function of the feature set), never by the final
 * swapchain/encode format. Throws std::runtime_error when the GPU cannot
 * provide the requested precision.
 */
pl_fmt FindLinearWorkFormat(pl_gpu gpu, WorkPrecision precision);

// A black level that is effectively zero but still "known" to libplacebo (a
// literal 0 is treated as "minimum luminance not known" and infers a 1000:1
// contrast). Used when a target declares true black.
static constexpr float kKnownBlackNits = 0.0001f;

} // namespace EYEQ
