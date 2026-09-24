// Pure-CPU tests for the deterministic DeckLink test-signal pattern generator.
// No SDK, no device, no GPU.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "output/pixel_format.hpp"
#include "test_pattern.hpp"

using namespace EYEQ;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

BufferLayout MakeLayout(int width, int height, PixelFormat format) {
  BufferLayout layout;
  layout.width = width;
  layout.height = height;
  layout.pixel_format = format;
  layout.row_bytes = MinimalRowBytes(format, width);
  layout.image_bytes = static_cast<std::size_t>(layout.row_bytes) * static_cast<std::size_t>(height);
  return layout;
}

struct Decoded {
  int y = 0;
  int cb = 0;
  int cr = 0;
};

uint32_t ReadLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Decodes one v210 pixel back to Y/Cb/Cr for verification.
Decoded DecodeV210(const std::vector<uint8_t> &bytes, const BufferLayout &layout, int x, int y) {
  const uint8_t *row = bytes.data() + static_cast<std::size_t>(y) * layout.row_bytes;
  const uint8_t *group = row + static_cast<std::size_t>(x / 6) * 16;
  const uint32_t w0 = ReadLe32(group);
  const uint32_t w1 = ReadLe32(group + 4);
  const uint32_t w2 = ReadLe32(group + 8);
  const uint32_t w3 = ReadLe32(group + 12);
  Decoded out;
  switch (x % 6) {
  case 0:
    out.y = static_cast<int>((w0 >> 10) & 0x3ffu);
    out.cb = static_cast<int>(w0 & 0x3ffu);
    out.cr = static_cast<int>((w0 >> 20) & 0x3ffu);
    break;
  case 1:
    out.y = static_cast<int>(w1 & 0x3ffu);
    out.cb = static_cast<int>(w0 & 0x3ffu);
    out.cr = static_cast<int>((w0 >> 20) & 0x3ffu);
    break;
  case 2:
    out.y = static_cast<int>((w1 >> 20) & 0x3ffu);
    out.cb = static_cast<int>((w1 >> 10) & 0x3ffu);
    out.cr = static_cast<int>(w2 & 0x3ffu);
    break;
  case 3:
    out.y = static_cast<int>((w2 >> 10) & 0x3ffu);
    out.cb = static_cast<int>((w1 >> 10) & 0x3ffu);
    out.cr = static_cast<int>(w2 & 0x3ffu);
    break;
  case 4:
    out.y = static_cast<int>(w3 & 0x3ffu);
    out.cb = static_cast<int>((w2 >> 20) & 0x3ffu);
    out.cr = static_cast<int>((w3 >> 10) & 0x3ffu);
    break;
  default:
    out.y = static_cast<int>((w3 >> 20) & 0x3ffu);
    out.cb = static_cast<int>((w2 >> 20) & 0x3ffu);
    out.cr = static_cast<int>((w3 >> 10) & 0x3ffu);
    break;
  }
  return out;
}

std::vector<uint8_t> Render(const BufferLayout &layout, const ColorEncoding &encoding, uint64_t frame_index) {
  std::vector<uint8_t> bytes(layout.image_bytes, 0);
  RenderTestPattern(frame_index, layout, encoding, bytes.data());
  return bytes;
}

void TestR210SinglePixelExactBytes() {
  const BufferLayout layout = MakeLayout(1, 1, PixelFormat::R210);
  CHECK(layout.row_bytes == 4);
  CHECK(layout.image_bytes == static_cast<std::size_t>(layout.row_bytes) * layout.height);
  const std::vector<uint8_t> bytes = Render(layout, ColorEncoding(), 0);
  // frame_index 0 puts the white square at x=0, so the single pixel is white.
  // Video-range RGB maps 1023 -> 940 (0x3AC); r210 is big-endian
  // (R<<20)|(G<<10)|B = 0x3ACEB3AC.
  CHECK(bytes.size() == 4);
  CHECK(bytes[0] == 0x3A && bytes[1] == 0xCE && bytes[2] == 0xB3 && bytes[3] == 0xAC);
}

void TestR210ChromaticExactBytes() {
  const BufferLayout layout = MakeLayout(64, 6, PixelFormat::R210);
  const std::vector<uint8_t> bytes = Render(layout, ColorEncoding(), 0);
  // Red bar (x=40..47) in the top band: R=1023->940 (0x3AC), G=B=0->64 (0x40).
  // Big-endian word = (940<<20)|(64<<10)|64 = 0x3AC10040.
  const std::size_t red = static_cast<std::size_t>(40) * 4;
  CHECK(bytes[red] == 0x3A && bytes[red + 1] == 0xC1 && bytes[red + 2] == 0x00 && bytes[red + 3] == 0x40);
  // Blue bar (x=48..55): R=G=64, B=940 -> 0x040103AC.
  const std::size_t blue = static_cast<std::size_t>(48) * 4;
  CHECK(bytes[blue] == 0x04 && bytes[blue + 1] == 0x01 && bytes[blue + 2] == 0x03 && bytes[blue + 3] == 0xAC);
}

void TestV210SixPixelExactBytes() {
  // Width 6, six rows: row 0 is the top band, so the six pixels are the first six
  // bars (white, yellow, cyan, magenta, red, blue) - deliberately NOT neutral gray.
  const BufferLayout layout = MakeLayout(6, 6, PixelFormat::V210);
  CHECK(layout.row_bytes == 16);
  CHECK(layout.image_bytes == static_cast<std::size_t>(layout.row_bytes) * layout.height);
  const std::vector<uint8_t> bytes = Render(layout, ColorEncoding(), 0);
  // BT.709 video range, left-sampled chroma pairs ((0,1),(2,3),(4,5)):
  //   white(940,512,512) yellow(877,64,553) cyan(754,615,64)
  //   magenta(313,857,919) red(250,409,960) blue(127,960,471)
  // w0 = Cb0 | Y0<<10 | Cr0<<20          = 0x200EB200
  // w1 = Y1 | Cb1<<10 | Y2<<20           = 0x2F299F6D
  // w2 = Cr1 | Y3<<10 | Cb2<<20          = 0x1994E440
  // w3 = Y4 | Cr2<<10 | Y5<<20           = 0x07FF00FA
  const uint8_t expected[16] = {0x00, 0xB2, 0x0E, 0x20, 0x6D, 0x9F, 0x29, 0x2F,
                                0x40, 0xE4, 0x94, 0x19, 0xFA, 0x00, 0xFF, 0x07};
  CHECK(bytes.size() == static_cast<std::size_t>(layout.row_bytes) * layout.height);
  CHECK(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0);
}

void TestV210PaddingIsCodeZero() {
  // Width 7 is not a multiple of 6: the last group has one real pixel and five
  // padding lanes that must be code value 0, then zero bytes up to row_bytes.
  const BufferLayout layout = MakeLayout(7, 6, PixelFormat::V210);
  CHECK(layout.row_bytes == 32);
  const std::vector<uint8_t> bytes = Render(layout, ColorEncoding(), 0);
  const uint8_t *group = bytes.data() + 16; // second group starts after six pixels
  CHECK(ReadLe32(group) != 0);              // one real pixel
  for (int i = 4; i < 16; ++i) {
    CHECK(group[i] == 0); // w1/w2/w3 are entirely padding code value 0
  }
}

void TestDeterministic() {
  const BufferLayout layout = MakeLayout(64, 24, PixelFormat::V210);
  const ColorEncoding encoding;
  CHECK(Render(layout, encoding, 7) == Render(layout, encoding, 7));
  CHECK(Render(layout, encoding, 0) != Render(layout, encoding, 5));
}

void TestVideoRangeBounds() {
  const BufferLayout layout = MakeLayout(64, 24, PixelFormat::V210);
  const std::vector<uint8_t> bytes = Render(layout, ColorEncoding(), 3);
  int min_y = 1023;
  int max_y = 0;
  for (int y = 0; y < layout.height; ++y) {
    for (int x = 0; x < layout.width; ++x) {
      const Decoded pixel = DecodeV210(bytes, layout, x, y);
      CHECK(pixel.y >= 64 && pixel.y <= 940);
      CHECK(pixel.cb >= 64 && pixel.cb <= 960);
      CHECK(pixel.cr >= 64 && pixel.cr <= 960);
      min_y = std::min(min_y, pixel.y);
      max_y = std::max(max_y, pixel.y);
    }
  }
  // The bars include 100% black and 100% white.
  CHECK(min_y == 64);
  CHECK(max_y == 940);
}

void TestFullRangeCoversFullSwing() {
  const BufferLayout layout = MakeLayout(64, 24, PixelFormat::V210);
  ColorEncoding encoding;
  encoding.range = ColorRange::Full;
  const std::vector<uint8_t> bytes = Render(layout, encoding, 3);
  int min_value = 1023;
  int max_value = 0;
  for (int y = 0; y < layout.height; ++y) {
    for (int x = 0; x < layout.width; ++x) {
      const Decoded pixel = DecodeV210(bytes, layout, x, y);
      for (int value : {pixel.y, pixel.cb, pixel.cr}) {
        CHECK(value >= 0 && value <= 1023);
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
      }
    }
  }
  CHECK(min_value == 0);
  CHECK(max_value == 1023);
}

void TestPrimariesChangeChroma() {
  const BufferLayout layout = MakeLayout(64, 24, PixelFormat::V210);
  ColorEncoding bt709;
  bt709.primaries = ColorPrimaries::BT709;
  ColorEncoding bt2020;
  bt2020.primaries = ColorPrimaries::BT2020;

  const std::vector<uint8_t> a = Render(layout, bt709, 0);
  const std::vector<uint8_t> b = Render(layout, bt2020, 0);
  // The red bar occupies x=40..47 in row 0.
  const Decoded red_709 = DecodeV210(a, layout, 40, 0);
  const Decoded red_2020 = DecodeV210(b, layout, 40, 0);
  CHECK(red_709.cb != red_2020.cb || red_709.cr != red_2020.cr);
  // The luma of pure red differs because the luma coefficients differ.
  CHECK(red_709.y != red_2020.y);
}

void TestFrameIndexMovesBottomBandOnly() {
  const BufferLayout layout = MakeLayout(64, 24, PixelFormat::V210);
  const ColorEncoding encoding;
  const std::vector<uint8_t> first = Render(layout, encoding, 0);
  const std::vector<uint8_t> second = Render(layout, encoding, 9);
  // Rows 0..3 (bars) and 4..5 (staircase) must be identical; the bottom band moves.
  const int bars_height = (2 * layout.height) / 3;
  const int rest = layout.height - bars_height;
  const int top_rows = bars_height + rest / 2;
  for (int y = 0; y < top_rows; ++y) {
    const std::size_t offset = static_cast<std::size_t>(y) * layout.row_bytes;
    CHECK(std::memcmp(first.data() + offset, second.data() + offset, layout.row_bytes) == 0);
  }
  const std::size_t bottom_offset = static_cast<std::size_t>(top_rows) * layout.row_bytes;
  CHECK(std::memcmp(first.data() + bottom_offset, second.data() + bottom_offset, layout.image_bytes - bottom_offset) !=
        0);
}

} // namespace

int main() {
  TestR210SinglePixelExactBytes();
  TestR210ChromaticExactBytes();
  TestV210SixPixelExactBytes();
  TestV210PaddingIsCodeZero();
  TestDeterministic();
  TestVideoRangeBounds();
  TestFullRangeCoversFullSwing();
  TestPrimariesChangeChroma();
  TestFrameIndexMovesBottomBandOnly();
  std::printf("test_signal_pattern: OK\n");
  return 0;
}
