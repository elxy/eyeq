#include "utils.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <libplacebo/colorspace.h>

namespace EYEQ {

namespace {

// DPX V1.0 header layout (SMPTE 268M)
static constexpr size_t kDpxGenericHeaderSize = 1664; // file info + image info + image source info
static constexpr size_t kDpxDataOffset = 8192;        // offset to image data (8 KiB aligned, like Resolve)

static void put16(std::vector<uint8_t> &buf, size_t off, uint16_t v) {
  buf[off] = static_cast<uint8_t>(v >> 8);
  buf[off + 1] = static_cast<uint8_t>(v & 0xFF);
}

static void put32(std::vector<uint8_t> &buf, size_t off, uint32_t v) {
  buf[off] = static_cast<uint8_t>(v >> 24);
  buf[off + 1] = static_cast<uint8_t>(v >> 16);
  buf[off + 2] = static_cast<uint8_t>(v >> 8);
  buf[off + 3] = static_cast<uint8_t>(v & 0xFF);
}

static void put_f32(std::vector<uint8_t> &buf, size_t off, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  put32(buf, off, bits); // host is little-endian; put32 writes big-endian
}

// Map libplacebo transfer to DPX transfer characteristic (SMPTE ST 268:2014)
static uint8_t dpx_transfer(enum pl_color_transfer trc) {
  switch (trc) {
  case PL_COLOR_TRC_LINEAR:
  case PL_COLOR_TRC_ST428:
    return 2; // Linear
  case PL_COLOR_TRC_PQ:
    return 14; // SMPTE ST 2084 (PQ)
  case PL_COLOR_TRC_HLG:
    return 15; // ITU-R BT.2100 HLG
  case PL_COLOR_TRC_SRGB:
  case PL_COLOR_TRC_BT_1886:
  case PL_COLOR_TRC_GAMMA18:
  case PL_COLOR_TRC_GAMMA20:
  case PL_COLOR_TRC_GAMMA22:
  case PL_COLOR_TRC_GAMMA24:
  case PL_COLOR_TRC_GAMMA26:
  case PL_COLOR_TRC_GAMMA28:
  case PL_COLOR_TRC_PRO_PHOTO:
    return 6; // ITU-R BT.709-4
  default:
    return 4; // Unspecified video
  }
}

// Map libplacebo primaries to DPX colorimetric specification (SMPTE ST 268:2014)
static uint8_t dpx_colorimetric(enum pl_color_primaries prim) {
  switch (prim) {
  case PL_COLOR_PRIM_BT_709:
    return 6; // ITU-R BT.709-4
  case PL_COLOR_PRIM_BT_601_625:
    return 7; // ITU-R BT.601-5 (625)
  case PL_COLOR_PRIM_BT_601_525:
    return 8; // ITU-R BT.601-5 (525)
  case PL_COLOR_PRIM_BT_2020:
    return 15; // ITU-R BT.2020
  default:
    return 4; // Unspecified video
  }
}

// Clamp bit depth to a DPX-supported value (8, 10, 12 or 16)
static int clamp_bit_depth(int depth) {
  if (depth <= 8)
    return 8;
  if (depth <= 10)
    return 10;
  if (depth <= 12)
    return 12;
  return 16;
}

// Quantize a normalized float to an integer value in [0, 2^depth - 1]
static uint32_t quantize(float v, int depth) {
  if (v < 0.0f)
    v = 0.0f;
  if (v > 1.0f)
    v = 1.0f;
  return static_cast<uint32_t>(v * static_cast<float>((1u << depth) - 1) + 0.5f);
}

// Pack one row of RGB pixels into DPX data for the given bit depth.
// Returns the number of bytes written (before any row padding).
static size_t encode_row(uint8_t *dst, const float *src, int width, int depth) {
  uint8_t *start = dst;
  auto emit_be32 = [&dst](uint32_t w) {
    *dst++ = static_cast<uint8_t>(w >> 24);
    *dst++ = static_cast<uint8_t>(w >> 16);
    *dst++ = static_cast<uint8_t>(w >> 8);
    *dst++ = static_cast<uint8_t>(w);
  };

  switch (depth) {
  case 8:
    for (int x = 0; x < width; x++) {
      *dst++ = static_cast<uint8_t>(quantize(src[x * 4 + 0], 8));
      *dst++ = static_cast<uint8_t>(quantize(src[x * 4 + 1], 8));
      *dst++ = static_cast<uint8_t>(quantize(src[x * 4 + 2], 8));
    }
    break;
  case 10: {
    // Filled method A (packing=1): R[9:0] G[9:0] B[9:0] into a 32-bit word,
    // R in the MSBs, 2 zero padding bits in the LSBs.
    for (int x = 0; x < width; x++) {
      const uint32_t r = quantize(src[x * 4 + 0], 10);
      const uint32_t g = quantize(src[x * 4 + 1], 10);
      const uint32_t b = quantize(src[x * 4 + 2], 10);
      emit_be32((r << 22) | (g << 12) | (b << 2));
    }
    break;
  }
  case 12: {
    // Packed (packing=0): 12-bit components tightly packed into big-endian
    // 32-bit words, filling each word from the LSB (matches FFmpeg's
    // read12in32 and Resolve's layout).
    uint64_t acc = 0;
    int bits = 0;
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < 3; c++) {
        acc |= static_cast<uint64_t>(quantize(src[x * 4 + c], 12)) << bits;
        bits += 12;
        while (bits >= 32) {
          emit_be32(static_cast<uint32_t>(acc));
          acc >>= 32;
          bits -= 32;
        }
      }
    }
    if (bits > 0) {
      emit_be32(static_cast<uint32_t>(acc));
    }
    break;
  }
  case 16:
  default:
    for (int x = 0; x < width; x++) {
      const uint16_t r = static_cast<uint16_t>(quantize(src[x * 4 + 0], 16));
      const uint16_t g = static_cast<uint16_t>(quantize(src[x * 4 + 1], 16));
      const uint16_t b = static_cast<uint16_t>(quantize(src[x * 4 + 2], 16));
      *dst++ = static_cast<uint8_t>(r >> 8);
      *dst++ = static_cast<uint8_t>(r & 0xFF);
      *dst++ = static_cast<uint8_t>(g >> 8);
      *dst++ = static_cast<uint8_t>(g & 0xFF);
      *dst++ = static_cast<uint8_t>(b >> 8);
      *dst++ = static_cast<uint8_t>(b & 0xFF);
    }
    break;
  }
  return static_cast<size_t>(dst - start);
}

} // namespace

int save_render_frame(const float *rgba, int width, int height, enum pl_color_transfer trc,
                      enum pl_color_primaries prim, float max_luma, int bit_depth,
                      const std::filesystem::path &filename) {
  if (!rgba || width <= 0 || height <= 0)
    return -1;

  bit_depth = clamp_bit_depth(bit_depth);

  // Row stride and padding per bit depth
  size_t row_bytes;
  size_t row_pad;
  switch (bit_depth) {
  case 8:
    row_bytes = static_cast<size_t>(width) * 3;
    row_pad = (4 - (row_bytes % 4)) % 4;
    break;
  case 10:
    row_bytes = static_cast<size_t>(width) * 4; // already 4-byte aligned
    row_pad = 0;
    break;
  case 12:
    row_bytes = (static_cast<size_t>(width) * 36 + 31) / 32 * 4; // already aligned
    row_pad = 0;
    break;
  case 16:
  default:
    row_bytes = static_cast<size_t>(width) * 6;
    row_pad = (4 - (row_bytes % 4)) % 4;
    break;
  }
  const size_t data_size = (row_bytes + row_pad) * static_cast<size_t>(height);

  std::vector<uint8_t> buf(kDpxDataOffset + data_size, 0);

  // File information header
  std::memcpy(buf.data(), "SDPX", 4);
  put32(buf, 4, kDpxDataOffset);          // offset to image data
  std::memcpy(buf.data() + 8, "V1.0", 4);
  put32(buf, 20, 1);                      // new image
  put32(buf, 24, kDpxGenericHeaderSize);  // generic header size
  put32(buf, 660, 0xFFFFFFFF);            // unencrypted

  // Image information header
  put16(buf, 768, 0);                    // orientation: left-to-right, top-to-bottom
  put16(buf, 770, 1);                    // number of elements
  put32(buf, 772, static_cast<uint32_t>(width));
  put32(buf, 776, static_cast<uint32_t>(height));
  buf[800] = 50;                         // descriptor: RGB
  buf[801] = dpx_transfer(trc);          // transfer characteristic
  buf[802] = dpx_colorimetric(prim);     // colorimetric specification
  buf[803] = static_cast<uint8_t>(bit_depth);
  put16(buf, 804, bit_depth == 10 ? 1 : 0); // packing: filled for 10-bit, packed otherwise
  put32(buf, 808, kDpxDataOffset);       // data offset

  // Reference data/quantity mapping (SMPTE ST 268). Quantity is in nits so that
  // downstream tools can recover the HDR peak luminance.
  put32(buf, 784, 0);                    // Reference Low Data
  put_f32(buf, 788, 0.0f);               // Reference Low Quantity (nits)
  put32(buf, 792, (1u << bit_depth) - 1); // Reference High Data (max code value)
  put_f32(buf, 796, max_luma);           // Reference High Quantity (nits)

  put32(buf, 1628, 1);                   // pixel aspect ratio numerator
  put32(buf, 1632, 1);                   // pixel aspect ratio denominator

  // Pixel data
  uint8_t *dst = buf.data() + kDpxDataOffset;
  for (int y = 0; y < height; y++) {
    const float *src = rgba + static_cast<size_t>(y) * width * 4;
    encode_row(dst, src, width, bit_depth);
    dst += row_bytes + row_pad;
  }

  std::ofstream out(filename, std::ios::binary);
  if (!out)
    return -1;
  out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
  out.close();
  return out ? 0 : -1;
}

} // namespace EYEQ
