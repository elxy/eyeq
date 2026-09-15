#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <stdexcept>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
}
#include <libplacebo/colorspace.h>

static constexpr int kUpdateIntervalMS = 2;     // ~500Hz
static constexpr int kWaitIntervalMS = 200;     // ~5Hz, user-facing wait interval
static constexpr int kTooLongIntervalMS = 2000; // Timeout interval suggesting an error

static inline std::string ffmpeg_error_string(int errnum) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
  av_strerror(errnum, buf.data(), buf.size());
  return std::string(buf.data());
}

namespace EYEQ {
class timeout_error : public std::runtime_error {
public:
  timeout_error(const std::string &what_arg) : std::runtime_error(what_arg) {}
  timeout_error(const char *what_arg) : std::runtime_error(what_arg) {}
};

class texture_format_error : public std::runtime_error {
public:
  texture_format_error(const std::string &what_arg) : std::runtime_error(what_arg) {}
  texture_format_error(const char *what_arg) : std::runtime_error(what_arg) {}
};

/**
 * @brief Convert timeout to std::chrono::milliseconds
 *
 * @param timeout_ms Timeout in ms; if negative, returns a very long duration
 */
inline std::chrono::milliseconds get_timeout(int timeout_ms) {
  return (timeout_ms < 0) ? std::chrono::seconds(10000) : std::chrono::milliseconds(timeout_ms);
}

int save_frame(AVFrame *frame, const std::filesystem::path &filename, std::string_view format);

/**
 * @brief Save rendered frame as a 12-bit RGB DPX image
 *
 * Stores display-referred values as-is (what you see is what you get); the DPX
 * header records the transfer/primaries so downstream tools interpret them correctly.
 *
 * @param rgba Tightly packed normalized RGBA float data, values in [0, 1]
 * @param width Image width
 * @param height Image height
 * @param trc Transfer characteristic to record in the DPX header (e.g. PQ)
 * @param prim Primaries to record in the DPX header (e.g. BT.2020)
 * @param max_luma Peak luminance in nits, recorded in the DPX reference high quantity
 * @param bit_depth Bit depth of the source framebuffer (clamped to 8/10/12/16)
 * @param filename Output path (.dpx)
 * @return 0 on success, negative on failure
 */
int save_render_frame(const float *rgba, int width, int height, enum pl_color_transfer trc,
                      enum pl_color_primaries prim, float max_luma, int bit_depth,
                      const std::filesystem::path &filename);

} // namespace EYEQ
