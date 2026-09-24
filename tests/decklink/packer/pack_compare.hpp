#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "output/pixel_format.hpp"
#include "pack_reference.hpp"

// Byte-level comparison between packed output and the independent CPU reference.
// Shared by the CPU oracle (mutation tests) and the real-GPU tests. This header needs
// no GPU and no packer implementation.
namespace eyeq_test {

struct CompareStats {
  long long exact = 0;
  long long within_one = 0;
  long long boundary = 0;
  long long mismatch = 0;
  long long absent_checked = 0;
};

// Absolute pre-quantization distance below which a float/double rounding
// disagreement can legitimately flip a 10-bit code by one. The 876-step luma span maps
// [0,1] to 876 codes, so a float32 shader and the float64 reference differ by at most
// a few 1e-4 code steps; 1e-3 is a narrow documented margin, not a blanket tolerance.
constexpr double kRoundingBoundaryEpsilon = 1e-3;

// Compares packed bytes against the reference row by row. Exact code matches are
// required except within one code when the reference pre-value sits on a rounding
// boundary. The two high bits and every absent/padding field must be exactly zero for
// both formats.
//
// `allow_one_code` is retained only for source compatibility with earlier probes; it
// no longer widens the tolerance. The boundary rule above is always applied, so FP16
// input precision does not justify an arbitrary +/-1 waiver.
inline CompareStats CompareFrame(const uint8_t *actual, int width, int height, int row_bytes,
                                 const std::vector<float> &rgb, EYEQ::PixelFormat format,
                                 EYEQ::ColorPrimaries primaries, bool /*allow_one_code*/ = false) {
  const int words_per_row = row_bytes / 4;
  std::vector<RefWord> reference(static_cast<std::size_t>(words_per_row));
  CompareStats stats;
  for (int y = 0; y < height; ++y) {
    ReferenceRow(rgb.data() + static_cast<std::size_t>(y) * width * 3, width, row_bytes, format, primaries,
                 reference.data());
    const uint8_t *row = actual + static_cast<std::size_t>(y) * row_bytes;
    for (int x = 0; x < words_per_row; ++x) {
      const uint8_t *p = row + static_cast<std::size_t>(x) * 4;
      uint32_t word = 0;
      if (format == EYEQ::PixelFormat::R210) {
        word = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
      } else {
        word = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
      }
      // Both formats use three 10-bit fields; bits 30-31 must be zero.
      if ((word >> 30) != 0) {
        ++stats.mismatch;
      }
      const RefWord &expected = reference[static_cast<std::size_t>(x)];
      for (int k = 0; k < 3; ++k) {
        const int got = static_cast<int>((word >> (10 * k)) & 0x3FFu);
        if (expected.absent[k]) {
          ++stats.absent_checked;
          if (got != 0) {
            ++stats.mismatch;
          }
          continue;
        }
        const int want = expected.code[k];
        const int diff = std::abs(got - want);
        if (diff == 0) {
          ++stats.exact;
          continue;
        }
        const double fraction = std::fabs((expected.pre[k] - std::floor(expected.pre[k])) - 0.5);
        if (diff == 1 && fraction < kRoundingBoundaryEpsilon) {
          ++stats.within_one;
          ++stats.boundary;
        } else {
          ++stats.mismatch;
        }
      }
    }
  }
  return stats;
}

} // namespace eyeq_test
