#pragma once

#include <cstddef>
#include <string>

namespace EYEQ {

enum class PixelFormat { R210, V210 };
enum class ColorPrimaries { BT709, BT2020 };
enum class ColorTransfer { SDR_BT1886, PQ };
enum class ColorRange { Video, Full };
enum class ChromaLocation { Left, Center };

// Pixel encoding description. Defaults are the SDR, full-first-version-safe values,
// so an unconfigured ColorProfile is coherent with a default (SDR) OutputRequest.
struct ColorEncoding {
  ColorPrimaries primaries = ColorPrimaries::BT709;
  ColorTransfer transfer = ColorTransfer::SDR_BT1886;
  ColorRange range = ColorRange::Video;
  ChromaLocation chroma_loc = ChromaLocation::Left;
};

// Monitor target. target_nits is a renderer brightness goal and is never passed
// to the packer.
struct ColorProfile {
  ColorEncoding encoding;
  double target_nits = 1000.0;
};

// Resolved host buffer layout. row_bytes is the SDK-reported stride; image_bytes is
// row_bytes * height and is NOT the underlying allocation capacity.
struct BufferLayout {
  int width = 0;
  int height = 0;
  PixelFormat pixel_format = PixelFormat::V210;
  int row_bytes = 0;
  std::size_t image_bytes = 0;
};

// Minimal packer input: host layout + the wire encoding the packer must produce.
// Deliberately independent of the SDK-facing OutputProfile types.
struct PackConfig {
  BufferLayout layout;
  ColorEncoding output;
};

// Word-aligned minimum row stride for the format. r210 = width * 4 (one pixel per
// 32-bit word); v210 = ceil(width / 6) * 16 (six pixels per four 32-bit words).
// Returns 0 for width <= 0. This is a lower bound for validation only; the SDK's
// RowBytesForPixelFormat result is the authority.
int MinimalRowBytes(PixelFormat fmt, int width);

// Validates a resolved layout against the SDK-reported buffer capacity.
// Rejects non-positive dimensions, undersized/misaligned stride, checked-multiply
// overflow, an inconsistent image_bytes, and a capacity smaller than image_bytes.
// A capacity larger than image_bytes is valid. Returns false and sets *error.
bool ValidateLayout(const BufferLayout &layout, std::size_t sdk_capacity, std::string *error);

const char *ToString(PixelFormat fmt);

} // namespace EYEQ
