#include "display/mapping_target.hpp"

#include <cmath>
#include <stdexcept>

#include <spdlog/fmt/fmt.h>

namespace EYEQ {

const char *ToString(WorkPrecision precision) {
  switch (precision) {
  case WorkPrecision::F16:
    return "rgba16f";
  case WorkPrecision::F32:
    return "rgba32f";
  }
  return "unknown";
}

enum pl_color_transfer PLTransferFor(ColorTransfer transfer) {
  switch (transfer) {
  case ColorTransfer::SDR_BT1886:
    return PL_COLOR_TRC_BT_1886;
  case ColorTransfer::PQ:
    return PL_COLOR_TRC_PQ;
  }
  throw std::invalid_argument("unknown ColorTransfer");
}

enum pl_color_primaries PLPrimariesFor(ColorPrimaries primaries) {
  switch (primaries) {
  case ColorPrimaries::BT709:
    return PL_COLOR_PRIM_BT_709;
  case ColorPrimaries::BT2020:
    return PL_COLOR_PRIM_BT_2020;
  }
  throw std::invalid_argument("unknown ColorPrimaries");
}

struct pl_color_space MappingTarget::DisplayLinearColorSpace() const {
  struct pl_color_space csp{};
  csp.primaries = primaries;
  csp.transfer = PL_COLOR_TRC_LINEAR;
  // A declared true black must stay "known" to libplacebo; a literal 0.0 would
  // be read as unknown and infer a 1000:1 contrast instead of infinite.
  csp.hdr.min_luma = (black_nits > 0.0f) ? black_nits : kKnownBlackNits;
  csp.hdr.max_luma = peak_nits;
  return csp;
}

struct pl_color_space MappingTarget::WireColorSpace(ColorTransfer wire) const {
  struct pl_color_space csp{};
  csp.primaries = primaries;
  csp.transfer = PLTransferFor(wire);
  // The wire transfer is applied in absolute/physical terms: the encoder maps
  // the display-linear value (1.0 == 203 cd/m^2) to nits first. For SDR the
  // code-100% level is the independent SDR white (not the target peak); PQ is
  // absolute and does not depend on it.
  csp.hdr.min_luma = (black_nits > 0.0f) ? black_nits : kKnownBlackNits;
  csp.hdr.max_luma = (wire == ColorTransfer::SDR_BT1886) ? sdr_white_nits : peak_nits;
  return csp;
}

void ValidateMappingTarget(const MappingTarget &target) {
  if (!std::isfinite(target.peak_nits) || target.peak_nits <= 0.0f) {
    throw std::invalid_argument("mapping target: peak_nits must be finite and positive");
  }
  if (!std::isfinite(target.black_nits) || target.black_nits < 0.0f) {
    throw std::invalid_argument("mapping target: black_nits must be finite and non-negative");
  }
  if (target.black_nits >= target.peak_nits) {
    throw std::invalid_argument("mapping target: black_nits must be below peak_nits");
  }
  if (!std::isfinite(target.sdr_white_nits) || target.sdr_white_nits <= 0.0f) {
    throw std::invalid_argument("mapping target: sdr_white_nits must be finite and positive");
  }
}

MappingTarget MappingTargetFromColorProfile(const ColorProfile &profile, float sdr_white_nits) {
  MappingTarget target;
  target.primaries = PLPrimariesFor(profile.encoding.primaries);
  target.peak_nits = static_cast<float>(profile.target_nits);
  target.black_nits = 0.0f;
  target.sdr_white_nits = sdr_white_nits;
  ValidateMappingTarget(target);
  return target;
}

pl_fmt FindLinearWorkFormat(pl_gpu gpu, WorkPrecision precision) {
  const int depth = (precision == WorkPrecision::F32) ? 32 : 16;
  const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE |
                                                              PL_FMT_CAP_HOST_READABLE | PL_FMT_CAP_LINEAR);
  pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, depth, depth, caps);
  if (!fmt) {
    throw std::runtime_error(fmt::format("GPU has no sampleable/renderable RGBA{} linear work format",
                                         (precision == WorkPrecision::F32) ? 32 : 16));
  }
  return fmt;
}

} // namespace EYEQ
