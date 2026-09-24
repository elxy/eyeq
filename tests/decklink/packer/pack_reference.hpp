#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "output/pixel_format.hpp"

// Independent, double-precision CPU reference for the r210/v210 packing rules.
// This file is test-only and deliberately does not reuse any production conversion
// or packing code, so the GPU test's golden data cannot be self-consistent with the
// implementation it is checking.
namespace eyeq_test {

// One packed 32-bit word of the reference frame. `code[k]` is the field in bit
// order: bits [10k, 10k+9] of `word`. `pre[k]` is the pre-quantization code-domain
// value, used only to decide whether a one-code float/double difference sits on a
// rounding boundary. `absent[k]` marks a field with no source sample (past the image
// width or an unused padding word), which must be exactly zero.
struct RefWord {
  uint32_t word = 0;
  int code[3] = {0, 0, 0};
  double pre[3] = {0.0, 0.0, 0.0};
  bool absent[3] = {true, true, true};
};

inline void ReferenceMatrix(EYEQ::ColorPrimaries primaries, double *kr, double *kb) {
  switch (primaries) {
  case EYEQ::ColorPrimaries::BT709:
    *kr = 0.2126;
    *kb = 0.0722;
    return;
  case EYEQ::ColorPrimaries::BT2020:
    *kr = 0.2627;
    *kb = 0.0593;
    return;
  default:
    *kr = 0.0;
    *kb = 0.0;
    return;
  }
}

inline double Clamp01(double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

inline void FetchPixel(const float *rgb_row, int x, double out[3]) {
  const float *pixel = rgb_row + static_cast<std::size_t>(x) * 3;
  out[0] = Clamp01(static_cast<double>(pixel[0]));
  out[1] = Clamp01(static_cast<double>(pixel[1]));
  out[2] = Clamp01(static_cast<double>(pixel[2]));
}

inline int QuantizeCode(double pre, double lo, double hi) {
  double value = std::floor(pre + 0.5);
  value = std::max(lo, std::min(hi, value));
  return static_cast<int>(value);
}

struct RefField {
  uint32_t value = 0;
  double pre = 0.0;
  int code = 0;
  bool absent = true;
};

// Builds one row of reference words. `row_bytes` is the configured stride, so the
// number of words includes any trailing padding words. r210 stores the word
// big-endian; v210 stores it little-endian (handled by ReferenceBytes).
inline void ReferenceRow(const float *rgb_row, int width, int row_bytes, EYEQ::PixelFormat format,
                         EYEQ::ColorPrimaries primaries, RefWord *out_words) {
  const int words_per_row = row_bytes / 4;

  if (format == EYEQ::PixelFormat::R210) {
    for (int x = 0; x < words_per_row; ++x) {
      RefWord &w = out_words[x];
      if (x >= width) {
        continue; // already zero/absent
      }
      double rgb[3];
      FetchPixel(rgb_row, x, rgb);
      // Bit order inside the word is B, G, R (R is the most significant field).
      const double pre[3] = {64.0 + 876.0 * rgb[2], 64.0 + 876.0 * rgb[1], 64.0 + 876.0 * rgb[0]};
      int code[3];
      for (int k = 0; k < 3; ++k) {
        code[k] = QuantizeCode(pre[k], 64.0, 940.0);
      }
      w.word = (static_cast<uint32_t>(code[2]) << 20) | (static_cast<uint32_t>(code[1]) << 10) |
               static_cast<uint32_t>(code[0]);
      for (int k = 0; k < 3; ++k) {
        w.code[k] = code[k];
        w.pre[k] = pre[k];
        w.absent[k] = false;
      }
    }
    return;
  }

  double kr = 0.0;
  double kb = 0.0;
  ReferenceMatrix(primaries, &kr, &kb);
  const double kg = 1.0 - kr - kb;

  std::vector<double> luma(width, 0.0);
  std::vector<double> cb(width, 0.0);
  std::vector<double> cr(width, 0.0);
  for (int x = 0; x < width; ++x) {
    double rgb[3];
    FetchPixel(rgb_row, x, rgb);
    const double y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
    luma[x] = y;
    cb[x] = (rgb[2] - y) / (2.0 * (1.0 - kb));
    cr[x] = (rgb[0] - y) / (2.0 * (1.0 - kr));
  }

  // Horizontal [1,2,1]/4 low-pass centred on an even column with clamp-to-edge taps.
  auto filtered_chroma = [&](int even_col, bool is_cb) -> double {
    const std::vector<double> &source = is_cb ? cb : cr;
    const int a = std::max(even_col - 1, 0);
    const int b = even_col;
    const int c = std::min(even_col + 1, width - 1);
    return 0.25 * (source[a] + 2.0 * source[b] + source[c]);
  };
  auto luma_field = [&](int col) -> RefField {
    RefField field;
    if (col < 0 || col >= width) {
      return field;
    }
    field.absent = false;
    field.pre = 64.0 + 876.0 * luma[col];
    field.code = QuantizeCode(field.pre, 64.0, 940.0);
    field.value = static_cast<uint32_t>(field.code);
    return field;
  };
  auto chroma_field = [&](int even_col, bool is_cb) -> RefField {
    RefField field;
    if (even_col < 0 || even_col >= width) {
      return field;
    }
    field.absent = false;
    field.pre = 512.0 + 896.0 * filtered_chroma(even_col, is_cb);
    field.code = QuantizeCode(field.pre, 64.0, 960.0);
    field.value = static_cast<uint32_t>(field.code);
    return field;
  };

  for (int x = 0; x < words_per_row; ++x) {
    RefWord &w = out_words[x];
    const int base = (x / 4) * 6;
    const int index = x % 4;
    RefField field[3];
    if (index == 0) {
      field[0] = chroma_field(base + 0, true);
      field[1] = luma_field(base + 0);
      field[2] = chroma_field(base + 0, false);
    } else if (index == 1) {
      field[0] = luma_field(base + 1);
      field[1] = chroma_field(base + 2, true);
      field[2] = luma_field(base + 2);
    } else if (index == 2) {
      field[0] = chroma_field(base + 2, false);
      field[1] = luma_field(base + 3);
      field[2] = chroma_field(base + 4, true);
    } else {
      field[0] = luma_field(base + 4);
      field[1] = chroma_field(base + 4, false);
      field[2] = luma_field(base + 5);
    }
    w.word = 0;
    for (int k = 0; k < 3; ++k) {
      w.word |= field[k].value << (10 * k);
      w.code[k] = field[k].code;
      w.pre[k] = field[k].pre;
      w.absent[k] = field[k].absent;
    }
  }
}

// Serializes reference words with the wire byte order: r210 big-endian, v210
// little-endian.
inline std::vector<uint8_t> ReferenceBytes(const std::vector<RefWord> &words, EYEQ::PixelFormat format) {
  std::vector<uint8_t> out(words.size() * 4);
  for (std::size_t i = 0; i < words.size(); ++i) {
    const uint32_t w = words[i].word;
    uint8_t *p = out.data() + i * 4;
    if (format == EYEQ::PixelFormat::R210) {
      p[0] = static_cast<uint8_t>((w >> 24) & 0xFF);
      p[1] = static_cast<uint8_t>((w >> 16) & 0xFF);
      p[2] = static_cast<uint8_t>((w >> 8) & 0xFF);
      p[3] = static_cast<uint8_t>(w & 0xFF);
    } else {
      p[0] = static_cast<uint8_t>(w & 0xFF);
      p[1] = static_cast<uint8_t>((w >> 8) & 0xFF);
      p[2] = static_cast<uint8_t>((w >> 16) & 0xFF);
      p[3] = static_cast<uint8_t>((w >> 24) & 0xFF);
    }
  }
  return out;
}

} // namespace eyeq_test
