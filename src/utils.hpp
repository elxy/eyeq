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
    return (timeout_ms < 0) ? 
        std::chrono::seconds(10000) : 
        std::chrono::milliseconds(timeout_ms);
}

int save_frame(AVFrame *frame, const std::filesystem::path &filename, std::string_view format);

} // namespace EYEQ
