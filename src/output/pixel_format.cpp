#include "output/pixel_format.hpp"

#include <cstdint>
#include <limits>

namespace EYEQ {

int MinimalRowBytes(PixelFormat fmt, int width) {
  if (width <= 0) {
    return 0;
  }
  const int64_t w = static_cast<int64_t>(width);
  int64_t bytes = 0;
  switch (fmt) {
  case PixelFormat::R210:
    bytes = w * 4; // one pixel per 32-bit word
    break;
  case PixelFormat::V210:
    bytes = ((w + 5) / 6) * 16; // six pixels per four 32-bit words
    break;
  }
  constexpr int64_t kMax = std::numeric_limits<int>::max();
  return static_cast<int>(bytes > kMax ? kMax : bytes);
}

bool ValidateLayout(const BufferLayout &layout, std::size_t sdk_capacity, std::string *error) {
  auto fail = [&](const char *message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };

  if (layout.width <= 0 || layout.height <= 0) {
    return fail("layout dimensions must be positive");
  }
  if (layout.row_bytes < MinimalRowBytes(layout.pixel_format, layout.width)) {
    return fail("row_bytes is below the format minimum");
  }
  if (layout.row_bytes % 4 != 0) {
    return fail("row_bytes must be 32-bit word aligned");
  }

  const std::size_t row_bytes = static_cast<std::size_t>(layout.row_bytes);
  const std::size_t height = static_cast<std::size_t>(layout.height);
  if (height != 0 && row_bytes > std::numeric_limits<std::size_t>::max() / height) {
    return fail("row_bytes * height overflows size_t");
  }
  const std::size_t image_bytes = row_bytes * height;
  if (layout.image_bytes != image_bytes) {
    return fail("image_bytes does not equal row_bytes * height");
  }
  if (sdk_capacity < image_bytes) {
    return fail("SDK buffer capacity is smaller than image_bytes");
  }
  return true;
}

const char *ToString(PixelFormat fmt) {
  switch (fmt) {
  case PixelFormat::R210:
    return "r210";
  case PixelFormat::V210:
    return "v210";
  }
  return "unknown";
}

} // namespace EYEQ
