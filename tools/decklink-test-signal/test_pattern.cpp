#include "test_pattern.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace EYEQ {
namespace {

struct Rgb10 {
  int r = 0;
  int g = 0;
  int b = 0;
};

struct YCbCr10 {
  uint32_t y = 0;
  uint32_t cb = 0;
  uint32_t cr = 0;
};

// 100% saturation bars, left to right: white, yellow, cyan, green, magenta, red,
// blue, black. Values are 10-bit RGB code values (0..1023).
constexpr Rgb10 kBars[8] = {
    {1023, 1023, 1023}, {1023, 1023, 0}, {0, 1023, 1023}, {0, 1023, 0},
    {1023, 0, 1023},    {1023, 0, 0},    {0, 0, 1023},    {0, 0, 0},
};

// Vertical band split: the top ~2/3 is the color bars, then a narrow grayscale
// staircase band, then a narrow motion/gray-code band. The two lower bands split
// the remainder evenly. Every row inside one band is identical, so the pattern is
// generated once per band and copied; this keeps UHD generation well inside a frame
// interval without a GPU.
struct Regions {
  int bars = 0;
  int mid = 0;
  int bottom = 0;
};

enum class Band { Bars, Mid, Bottom };

Regions ComputeRegions(int height) {
  Regions regions;
  regions.bars = (2 * height) / 3;
  const int rest = height - regions.bars;
  regions.mid = rest / 2;
  regions.bottom = rest - regions.mid;
  return regions;
}

// One logical 10-bit RGB pixel of the pattern. frame_index only influences the
// bottom band, and the bottom band is the same on every bottom row.
Rgb10 SamplePixel(uint64_t frame_index, const BufferLayout &layout, const Regions &regions, Band band, int x) {
  const int width = layout.width;
  if (band == Band::Bars) {
    int bar = (x * 8) / width;
    bar = std::min(bar, 7);
    return kBars[bar];
  }
  if (band == Band::Mid) {
    // 11-step horizontal staircase, black to white.
    int level = (x * 11) / width;
    level = std::min(level, 10);
    const int value = (level * 1023 + 5) / 10;
    return Rgb10{value, value, value};
  }

  // Bottom band: black background, a Gray-coded index block on the left, and a
  // white square whose horizontal position is frame_index % width.
  Rgb10 pixel;
  const uint64_t gray = frame_index ^ (frame_index >> 1);
  const int cell_width = std::max(1, width / 64);
  for (int bit = 0; bit < 16; ++bit) {
    const int xa = bit * cell_width;
    if (xa >= width) {
      break;
    }
    const int xb = std::min(xa + cell_width, width);
    if (x >= xa && x < xb) {
      const int value = ((gray >> (15 - bit)) & 1u) ? 1023 : 0;
      pixel = Rgb10{value, value, value};
      break;
    }
  }

  const int span = regions.bottom; // square side, matched to the band height
  const int origin = static_cast<int>(frame_index % static_cast<uint64_t>(width));
  const int offset = ((x - origin) % width + width) % width;
  if (offset < span) {
    pixel = Rgb10{1023, 1023, 1023};
  }
  return pixel;
}

void WriteLe32(uint8_t *dst, uint32_t word) {
  dst[0] = static_cast<uint8_t>(word & 0xffu);
  dst[1] = static_cast<uint8_t>((word >> 8) & 0xffu);
  dst[2] = static_cast<uint8_t>((word >> 16) & 0xffu);
  dst[3] = static_cast<uint8_t>((word >> 24) & 0xffu);
}

// bmdFormat10BitRGB ('r210') is a big-endian word with R in the high 10 bits.
void WriteBe32(uint8_t *dst, uint32_t word) {
  dst[0] = static_cast<uint8_t>((word >> 24) & 0xffu);
  dst[1] = static_cast<uint8_t>((word >> 16) & 0xffu);
  dst[2] = static_cast<uint8_t>((word >> 8) & 0xffu);
  dst[3] = static_cast<uint8_t>(word & 0xffu);
}

int Clamp10(int value) { return std::min(std::max(value, 0), 1023); }

// Maps a full-scale 10-bit RGB code value (0..1023) into the requested range.
// r210 has no Y'CbCr conversion, so its RGB components themselves must respect the
// range: video-range RGB uses 64..940, full-range RGB keeps 0..1023.
int QuantizeComponent(int value, ColorRange range) {
  if (range == ColorRange::Full) {
    return Clamp10(value);
  }
  return Clamp10(64 + static_cast<int>(std::lround(static_cast<double>(value) * 876.0 / 1023.0)));
}

// RGB -> Y'CbCr 10-bit quantization. R/G/B are already 10-bit code values; no
// transfer function is applied.
YCbCr10 Quantize(const Rgb10 &rgb, const ColorEncoding &encoding) {
  const double r = static_cast<double>(rgb.r) / 1023.0;
  const double g = static_cast<double>(rgb.g) / 1023.0;
  const double b = static_cast<double>(rgb.b) / 1023.0;

  double kr = 0.2126;
  double kb = 0.0722;
  if (encoding.primaries == ColorPrimaries::BT2020) {
    kr = 0.2627;
    kb = 0.0593;
  }
  const double kg = 1.0 - kr - kb;
  const double luma = kr * r + kg * g + kb * b;
  const double cb_norm = (b - luma) / (2.0 * (1.0 - kb));
  const double cr_norm = (r - luma) / (2.0 * (1.0 - kr));

  YCbCr10 out;
  if (encoding.range == ColorRange::Full) {
    out.y = static_cast<uint32_t>(Clamp10(static_cast<int>(std::lround(luma * 1023.0))));
    out.cb = static_cast<uint32_t>(Clamp10(static_cast<int>(std::lround(512.0 + cb_norm * 1023.0))));
    out.cr = static_cast<uint32_t>(Clamp10(static_cast<int>(std::lround(512.0 + cr_norm * 1023.0))));
  } else {
    out.y = static_cast<uint32_t>(Clamp10(static_cast<int>(std::lround(64.0 + luma * 876.0))));
    out.cb = static_cast<uint32_t>(Clamp10(static_cast<int>(std::lround(512.0 + cb_norm * 896.0))));
    out.cr = static_cast<uint32_t>(Clamp10(static_cast<int>(std::lround(512.0 + cr_norm * 896.0))));
  }
  return out;
}

void RenderR210Row(uint64_t frame_index, const BufferLayout &layout, const Regions &regions,
                   const ColorEncoding &encoding, Band band, uint8_t *row) {
  for (int x = 0; x < layout.width; ++x) {
    const Rgb10 rgb = SamplePixel(frame_index, layout, regions, band, x);
    const uint32_t r = static_cast<uint32_t>(QuantizeComponent(rgb.r, encoding.range));
    const uint32_t g = static_cast<uint32_t>(QuantizeComponent(rgb.g, encoding.range));
    const uint32_t b = static_cast<uint32_t>(QuantizeComponent(rgb.b, encoding.range));
    // bmdFormat10BitRGB: big-endian word, (R << 20) | (G << 10) | B.
    WriteBe32(row + static_cast<std::size_t>(x) * 4, (r << 20) | (g << 10) | b);
  }
}

void RenderV210Row(uint64_t frame_index, const BufferLayout &layout, const Regions &regions,
                   const ColorEncoding &encoding, Band band, uint8_t *row) {
  std::size_t offset = 0;
  for (int x = 0; x < layout.width; x += 6) {
    YCbCr10 group[6];
    for (int k = 0; k < 6; ++k) {
      if (x + k < layout.width) {
        group[k] = Quantize(SamplePixel(frame_index, layout, regions, band, x + k), encoding);
      } else {
        // Out-of-range pixels are filled with code value 0, not black.
        group[k] = YCbCr10{0, 0, 0};
      }
    }
    // Chroma is left-sampled from the left pixel of each pair: pairs are
    // (0,1) -> group[0], (2,3) -> group[2], (4,5) -> group[4].
    const uint32_t w0 = group[0].cb | (group[0].y << 10) | (group[0].cr << 20);
    const uint32_t w1 = group[1].y | (group[2].cb << 10) | (group[2].y << 20);
    const uint32_t w2 = group[2].cr | (group[3].y << 10) | (group[4].cb << 20);
    const uint32_t w3 = group[4].y | (group[4].cr << 10) | (group[5].y << 20);
    WriteLe32(row + offset, w0);
    WriteLe32(row + offset + 4, w1);
    WriteLe32(row + offset + 8, w2);
    WriteLe32(row + offset + 12, w3);
    offset += 16;
  }
}

// Renders one representative packed row for a band. The row stride is always
// layout.row_bytes; unused trailing bytes are code value 0.
void RenderBandRow(uint64_t frame_index, const BufferLayout &layout, const Regions &regions,
                   const ColorEncoding &encoding, Band band, uint8_t *row) {
  std::memset(row, 0, static_cast<std::size_t>(layout.row_bytes));
  if (layout.pixel_format == PixelFormat::R210) {
    RenderR210Row(frame_index, layout, regions, encoding, band, row);
  } else {
    RenderV210Row(frame_index, layout, regions, encoding, band, row);
  }
}

} // namespace

void RenderTestPattern(uint64_t frame_index, const BufferLayout &layout, const ColorEncoding &encoding, uint8_t *dst) {
  // Precondition: a valid layout (see ValidateLayout) with pixel_format R210 or V210.
  // Invalid input and unknown formats are ignored rather than silently reinterpreted.
  if (dst == nullptr || layout.width <= 0 || layout.height <= 0 || layout.row_bytes <= 0) {
    return;
  }
  if (layout.pixel_format != PixelFormat::R210 && layout.pixel_format != PixelFormat::V210) {
    return;
  }

  const Regions regions = ComputeRegions(layout.height);
  const std::size_t stride = static_cast<std::size_t>(layout.row_bytes);

  // Every row within a band is identical, so generate one packed row per band and
  // copy it. This avoids recomputing a full UHD frame pixel by pixel.
  std::vector<uint8_t> bands(stride * 3, 0);
  RenderBandRow(frame_index, layout, regions, encoding, Band::Bars, bands.data());
  RenderBandRow(frame_index, layout, regions, encoding, Band::Mid, bands.data() + stride);
  RenderBandRow(frame_index, layout, regions, encoding, Band::Bottom, bands.data() + stride * 2);

  const uint8_t *bars_row = bands.data();
  const uint8_t *mid_row = bars_row + stride;
  const uint8_t *bottom_row = mid_row + stride;
  for (int y = 0; y < layout.height; ++y) {
    const uint8_t *src = y < regions.bars ? bars_row : (y < regions.bars + regions.mid ? mid_row : bottom_row);
    std::memcpy(dst + static_cast<std::size_t>(y) * stride, src, stride);
  }
}

} // namespace EYEQ
