// Pure-CPU known-answer and property tests for the independent r210/v210 reference
// in pack_reference.hpp. No GPU, no SDK. Hand-derived anchors do not use the
// reference to generate their expected values.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "output/pixel_format.hpp"
#include "pack_compare.hpp"
#include "pack_reference.hpp"

using namespace EYEQ;
using eyeq_test::CompareFrame;
using eyeq_test::CompareStats;
using eyeq_test::ReferenceBytes;
using eyeq_test::ReferenceRow;
using eyeq_test::RefField;
using eyeq_test::RefWord;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

struct Anchor {
  const char *name;
  float r, g, b;
  int rc, gc, bc; // r210 per-component codes (hand-derived)
  int y, cb, cr;  // v210 codes (hand-derived)
};

// Hand-derived anchors: BT709 and BT2020 luma/chroma quantization of the nonlinear
// primaries/complements. r210 codes only depend on the component value.
const Anchor kAnchors[] = {
    {"white", 1.0f, 1.0f, 1.0f, 940, 940, 940, 940, 512, 512},
    {"black", 0.0f, 0.0f, 0.0f, 64, 64, 64, 64, 512, 512},
    {"red", 1.0f, 0.0f, 0.0f, 940, 64, 64, 250, 409, 960},
    {"green", 0.0f, 1.0f, 0.0f, 64, 940, 64, 691, 167, 105},
    {"blue", 0.0f, 0.0f, 1.0f, 64, 64, 940, 127, 960, 471},
    {"yellow", 1.0f, 1.0f, 0.0f, 940, 940, 64, 877, 64, 553},
    {"cyan", 0.0f, 1.0f, 1.0f, 64, 940, 940, 754, 615, 64},
    {"magenta", 1.0f, 0.0f, 1.0f, 940, 64, 940, 313, 857, 919},
};

const Anchor kAnchors2020[] = {
    {"white", 1.0f, 1.0f, 1.0f, 940, 940, 940, 940, 512, 512},
    {"black", 0.0f, 0.0f, 0.0f, 64, 64, 64, 64, 512, 512},
    {"red", 1.0f, 0.0f, 0.0f, 940, 64, 64, 294, 387, 960},
    {"green", 0.0f, 1.0f, 0.0f, 64, 940, 64, 658, 189, 100},
    {"blue", 0.0f, 0.0f, 1.0f, 64, 64, 940, 116, 960, 476},
    {"yellow", 1.0f, 1.0f, 0.0f, 940, 940, 64, 888, 64, 548},
    {"cyan", 0.0f, 1.0f, 1.0f, 64, 940, 940, 710, 637, 64},
    {"magenta", 1.0f, 0.0f, 1.0f, 940, 64, 940, 346, 835, 924},
};

std::vector<float> UniformRow(int width, float r, float g, float b) {
  std::vector<float> row(static_cast<std::size_t>(width) * 3);
  for (int x = 0; x < width; ++x) {
    row[x * 3 + 0] = r;
    row[x * 3 + 1] = g;
    row[x * 3 + 2] = b;
  }
  return row;
}

std::vector<float> ColorRow(int width, const std::vector<std::array<float, 3>> &colors) {
  std::vector<float> row(static_cast<std::size_t>(width) * 3, 0.0f);
  for (int x = 0; x < width; ++x) {
    const auto &c = colors[static_cast<std::size_t>(x)];
    row[x * 3 + 0] = c[0];
    row[x * 3 + 1] = c[1];
    row[x * 3 + 2] = c[2];
  }
  return row;
}

void TestR210Anchors() {
  const int width = 6;
  const int row_bytes = MinimalRowBytes(PixelFormat::R210, width);
  std::vector<RefWord> words(static_cast<std::size_t>(row_bytes / 4));
  for (const Anchor &a : kAnchors) {
    std::vector<float> row = UniformRow(width, a.r, a.g, a.b);
    std::fill(words.begin(), words.end(), RefWord{});
    ReferenceRow(row.data(), width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709, words.data());
    const uint32_t expected =
        (static_cast<uint32_t>(a.rc) << 20) | (static_cast<uint32_t>(a.gc) << 10) | static_cast<uint32_t>(a.bc);
    CHECK(words[0].word == expected);
    CHECK((words[0].word >> 30) == 0); // the two high bits are always zero
    CHECK(words[0].code[0] == a.bc);
    CHECK(words[0].code[1] == a.gc);
    CHECK(words[0].code[2] == a.rc);
  }

  // Exact big-endian bytes for three anchors (independently known).
  auto first_word_bytes = [&](float r, float g, float b) {
    std::vector<float> row = UniformRow(width, r, g, b);
    std::fill(words.begin(), words.end(), RefWord{});
    ReferenceRow(row.data(), width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709, words.data());
    std::vector<uint8_t> bytes = ReferenceBytes(words, PixelFormat::R210);
    return std::vector<uint8_t>(bytes.begin(), bytes.begin() + 4);
  };
  CHECK(first_word_bytes(1.0f, 1.0f, 1.0f) == (std::vector<uint8_t>{0x3A, 0xCE, 0xB3, 0xAC}));
  CHECK(first_word_bytes(1.0f, 0.0f, 0.0f) == (std::vector<uint8_t>{0x3A, 0xC1, 0x00, 0x40}));
  CHECK(first_word_bytes(0.0f, 0.0f, 1.0f) == (std::vector<uint8_t>{0x04, 0x01, 0x03, 0xAC}));
}

void TestV210Anchors() {
  const int width = 6;
  const int row_bytes = MinimalRowBytes(PixelFormat::V210, width);
  CHECK(row_bytes / 4 == 4);
  std::vector<RefWord> words(4);
  auto check = [&](const Anchor &a, ColorPrimaries primaries) {
    std::vector<float> row = UniformRow(width, a.r, a.g, a.b);
    std::fill(words.begin(), words.end(), RefWord{});
    ReferenceRow(row.data(), width, row_bytes, PixelFormat::V210, primaries, words.data());
    // Uniform image: every chroma sample equals the pixel chroma.
    CHECK(words[0].code[0] == a.cb);
    CHECK(words[0].code[1] == a.y);
    CHECK(words[0].code[2] == a.cr);
    CHECK(words[1].code[0] == a.y);
    CHECK(words[1].code[1] == a.cb);
    CHECK(words[1].code[2] == a.y);
    CHECK(words[2].code[0] == a.cr);
    CHECK(words[2].code[1] == a.y);
    CHECK(words[2].code[2] == a.cb);
    CHECK(words[3].code[0] == a.y);
    CHECK(words[3].code[1] == a.cr);
    CHECK(words[3].code[2] == a.y);
  };
  for (const Anchor &a : kAnchors) {
    check(a, ColorPrimaries::BT709);
  }
  for (const Anchor &a : kAnchors2020) {
    check(a, ColorPrimaries::BT2020);
  }
}

// The [1,2,1]/4 chroma filter centred on even columns: an isolated red pixel at even
// column 2 contributes exactly half of its chroma, detected through the packed words.
void TestFilterWeightsAndPhase() {
  const int width = 6;
  const int row_bytes = MinimalRowBytes(PixelFormat::V210, width);
  std::vector<std::array<float, 3>> colors(static_cast<std::size_t>(width), {0.0f, 0.0f, 0.0f});
  colors[2] = {1.0f, 0.0f, 0.0f};
  std::vector<float> row = ColorRow(width, colors);
  std::vector<RefWord> words(4);
  ReferenceRow(row.data(), width, row_bytes, PixelFormat::V210, ColorPrimaries::BT709, words.data());

  // Y is untouched per pixel.
  CHECK(words[0].code[1] == 64);  // Y(0) black
  CHECK(words[1].code[0] == 64);  // Y(1) black
  CHECK(words[1].code[2] == 250); // Y(2) red
  // Cb(2) = (Cb(1) + 2*Cb(2) + Cb(3)) / 4 = Cb_red / 2 (hand-derived code 461).
  CHECK(words[1].code[1] == 461);
  // Cr(2) = Cr_red / 2 = 0.25 -> code 736.
  CHECK(words[2].code[0] == 736);
  // Cb(4) and Cr(4) see only black.
  CHECK(words[2].code[2] == 512);
  CHECK(words[3].code[1] == 512);
}

// Alternating red/blue columns must not be reduced to even-column-only sampling:
// the interior even column averages its two blue neighbours with itself.
void TestAlternatingColumns() {
  const int width = 8;
  const int row_bytes = MinimalRowBytes(PixelFormat::V210, width);
  std::vector<std::array<float, 3>> colors(static_cast<std::size_t>(width));
  for (int x = 0; x < width; ++x) {
    colors[static_cast<std::size_t>(x)] =
        (x % 2 == 0) ? std::array<float, 3>{1.0f, 0.0f, 0.0f} : std::array<float, 3>{0.0f, 0.0f, 1.0f};
  }
  std::vector<float> row = ColorRow(width, colors);
  std::vector<RefWord> words(static_cast<std::size_t>(row_bytes / 4));
  ReferenceRow(row.data(), width, row_bytes, PixelFormat::V210, ColorPrimaries::BT709, words.data());

  // Even column 2: Cb = (Cb_blue + 2*Cb_red + Cb_blue)/4, Cr likewise (hand-derived).
  CHECK(words[1].code[1] == 685); // Cb(2)
  CHECK(words[2].code[0] == 715); // Cr(2)
}

void TestQuantizeRoundingAndClamp() {
  CHECK(eyeq_test::QuantizeCode(100.4999, 64.0, 940.0) == 100);
  CHECK(eyeq_test::QuantizeCode(100.5, 64.0, 940.0) == 101);
  CHECK(eyeq_test::QuantizeCode(100.5001, 64.0, 940.0) == 101);
  CHECK(eyeq_test::QuantizeCode(-5.0, 64.0, 940.0) == 64);
  CHECK(eyeq_test::QuantizeCode(2000.0, 64.0, 940.0) == 940);
  CHECK(eyeq_test::QuantizeCode(-5.0, 64.0, 960.0) == 64);
  CHECK(eyeq_test::QuantizeCode(2000.0, 64.0, 960.0) == 960);
}

// Finite out-of-range inputs are legalized to [0,1] before conversion.
void TestFiniteOutOfRangeClamp() {
  const int width = 6;
  const int row_bytes = MinimalRowBytes(PixelFormat::R210, width);
  std::vector<RefWord> words(static_cast<std::size_t>(row_bytes / 4));
  std::vector<float> low = UniformRow(width, -2.0f, -0.5f, -1.0f);
  ReferenceRow(low.data(), width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709, words.data());
  CHECK(words[0].code[0] == 64);
  CHECK(words[0].code[1] == 64);
  CHECK(words[0].code[2] == 64);
  std::vector<float> high = UniformRow(width, 2.0f, 1.5f, 3.0f);
  ReferenceRow(high.data(), width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709, words.data());
  CHECK(words[0].code[0] == 940);
  CHECK(words[0].code[1] == 940);
  CHECK(words[0].code[2] == 940);
}

// Every legal 10-bit code round-trips through the luma transfer for both formats.
void TestAllLumaCodesRoundTrip() {
  for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
    const int width = 6;
    const int row_bytes = MinimalRowBytes(format, width);
    std::vector<RefWord> words(static_cast<std::size_t>(row_bytes / 4));
    for (int code = 64; code <= 940; ++code) {
      const float gray = static_cast<float>((code - 64) / 876.0);
      std::vector<float> row = UniformRow(width, gray, gray, gray);
      ReferenceRow(row.data(), width, row_bytes, format, ColorPrimaries::BT709, words.data());
      if (format == PixelFormat::R210) {
        CHECK(words[0].code[0] == code);
        CHECK(words[0].code[1] == code);
        CHECK(words[0].code[2] == code);
      } else {
        CHECK(words[0].code[1] == code);
      }
    }
  }
}

void TestWidthsAndPadding() {
  for (int width : {1, 5, 6, 7, 8, 47, 48, 49, 63, 64, 65}) {
    for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
      for (int extra : {0, 16}) {
        const int minimum = MinimalRowBytes(format, width);
        const int row_bytes = minimum + extra;
        const int words_per_row = row_bytes / 4;
        std::vector<RefWord> words(static_cast<std::size_t>(words_per_row));
        std::vector<float> row = UniformRow(width, 0.6f, 0.3f, 0.9f);
        ReferenceRow(row.data(), width, row_bytes, format, ColorPrimaries::BT2020, words.data());

        const std::vector<uint8_t> bytes = ReferenceBytes(words, format);
        CHECK(bytes.size() == static_cast<std::size_t>(row_bytes));

        if (format == PixelFormat::R210) {
          for (int x = width; x < words_per_row; ++x) {
            CHECK(words[static_cast<std::size_t>(x)].word == 0);
          }
        } else {
          const int pixel_words = ((width + 5) / 6) * 4;
          static const int kColumnOffset[4][3] = {{0, 0, 0}, {1, 2, 2}, {2, 3, 4}, {4, 4, 5}};
          for (int x = pixel_words; x < words_per_row; ++x) {
            CHECK(words[static_cast<std::size_t>(x)].word == 0);
          }
          for (int x = 0; x < pixel_words; ++x) {
            const RefWord &w = words[static_cast<std::size_t>(x)];
            const int base = (x / 4) * 6;
            for (int k = 0; k < 3; ++k) {
              const int column = base + kColumnOffset[x % 4][k];
              if (column >= width) {
                CHECK(w.absent[k]);
                CHECK(w.code[k] == 0);
              }
            }
          }
        }

        // Trailing stride padding bytes are zero, and the first pixel word is real.
        for (std::size_t i = static_cast<std::size_t>(minimum); i < bytes.size(); ++i) {
          CHECK(bytes[i] == 0);
        }
      }
    }
  }
}

// r210 is top-down row major: distinct rows serialize at distinct offsets, and the
// first row comes first.
void TestRowsAreIndependentAndTopDown() {
  const int width = 4;
  const int row_bytes = MinimalRowBytes(PixelFormat::R210, width);
  std::vector<RefWord> row0(static_cast<std::size_t>(row_bytes / 4));
  std::vector<RefWord> row1(static_cast<std::size_t>(row_bytes / 4));
  std::vector<float> a = UniformRow(width, 1.0f, 1.0f, 1.0f);
  std::vector<float> b = UniformRow(width, 0.0f, 0.0f, 0.0f);
  ReferenceRow(a.data(), width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709, row0.data());
  ReferenceRow(b.data(), width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709, row1.data());
  CHECK(row0[0].word != row1[0].word);
  CHECK(row0[0].word == 0x3ACEB3ACu);
  CHECK(row1[0].word == 0x04010040u);
}

std::vector<uint8_t> ReferenceRowBytes(const std::vector<float> &rgb, int width, int row_bytes, PixelFormat format,
                                       ColorPrimaries primaries) {
  std::vector<RefWord> words(static_cast<std::size_t>(row_bytes / 4));
  ReferenceRow(rgb.data(), width, row_bytes, format, primaries, words.data());
  return ReferenceBytes(words, format);
}

// The comparator itself must reject deliberate corruption: a one-code change away
// from a rounding boundary, high bits, and padding words. These are CPU-only, so
// they run even without a GPU.
void TestComparatorRejectsCorruption() {
  // v210: black Y0 = 64 is far from a .5 boundary, so 65 must fail.
  {
    const int width = 6;
    const int row_bytes = MinimalRowBytes(PixelFormat::V210, width);
    const std::vector<float> rgb = UniformRow(width, 0.0f, 0.0f, 0.0f);
    std::vector<uint8_t> clean = ReferenceRowBytes(rgb, width, row_bytes, PixelFormat::V210, ColorPrimaries::BT709);
    CompareStats ok = CompareFrame(clean.data(), width, 1, row_bytes, rgb, PixelFormat::V210, ColorPrimaries::BT709);
    CHECK(ok.mismatch == 0);
    CHECK(ok.within_one == 0);

    std::vector<uint8_t> bumped = clean;
    bumped[1] ^= 4u; // Y0: 64 -> 65
    CompareStats bad = CompareFrame(bumped.data(), width, 1, row_bytes, rgb, PixelFormat::V210, ColorPrimaries::BT709);
    CHECK(bad.mismatch == 1);

    std::vector<uint8_t> high = clean;
    high[3] |= 0xC0u; // the two high bits of word 0
    CompareStats high_stats =
        CompareFrame(high.data(), width, 1, row_bytes, rgb, PixelFormat::V210, ColorPrimaries::BT709);
    CHECK(high_stats.mismatch == 1);
  }

  // v210 padding word (width 5 with an extra stride word).
  {
    const int width = 5;
    const int row_bytes = MinimalRowBytes(PixelFormat::V210, width) + 16;
    const std::vector<float> rgb = UniformRow(width, 0.1f, 0.2f, 0.3f);
    std::vector<uint8_t> clean = ReferenceRowBytes(rgb, width, row_bytes, PixelFormat::V210, ColorPrimaries::BT709);
    CompareStats ok = CompareFrame(clean.data(), width, 1, row_bytes, rgb, PixelFormat::V210, ColorPrimaries::BT709);
    CHECK(ok.mismatch == 0);
    std::vector<uint8_t> padded = clean;
    padded[16] |= 1u; // first padding word
    CompareStats bad = CompareFrame(padded.data(), width, 1, row_bytes, rgb, PixelFormat::V210, ColorPrimaries::BT709);
    CHECK(bad.mismatch == 1);
  }

  // r210 high bits and padding word.
  {
    const int width = 3;
    const int row_bytes = MinimalRowBytes(PixelFormat::R210, width) + 8;
    const std::vector<float> rgb = UniformRow(width, 0.4f, 0.5f, 0.6f);
    std::vector<uint8_t> clean = ReferenceRowBytes(rgb, width, row_bytes, PixelFormat::R210, ColorPrimaries::BT709);
    CompareStats ok = CompareFrame(clean.data(), width, 1, row_bytes, rgb, PixelFormat::R210, ColorPrimaries::BT709);
    CHECK(ok.mismatch == 0);
    std::vector<uint8_t> high = clean;
    high[0] |= 0xC0u; // high bits of the big-endian first word
    CompareStats high_stats =
        CompareFrame(high.data(), width, 1, row_bytes, rgb, PixelFormat::R210, ColorPrimaries::BT709);
    CHECK(high_stats.mismatch == 1);
    std::vector<uint8_t> padded = clean;
    padded[12] |= 1u; // first padding word
    CompareStats bad = CompareFrame(padded.data(), width, 1, row_bytes, rgb, PixelFormat::R210, ColorPrimaries::BT709);
    CHECK(bad.mismatch == 1);
  }
}

} // namespace

int main() {
  TestR210Anchors();
  TestV210Anchors();
  TestFilterWeightsAndPhase();
  TestAlternatingColumns();
  TestQuantizeRoundingAndClamp();
  TestFiniteOutOfRangeClamp();
  TestAllLumaCodesRoundTrip();
  TestWidthsAndPadding();
  TestRowsAreIndependentAndTopDown();
  TestComparatorRejectsCorruption();
  std::printf("frame_packer_oracle: OK\n");
  return 0;
}
