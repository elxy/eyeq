#include "platform_icc.h"
#include <spdlog/spdlog.h>

namespace EYEQ {

extern std::shared_ptr<spdlog::logger> Logger;

void ParseICCProfileHeader(ICCProfileInfo &info) {
  const auto &data = info.data;
  if (data.size() < 128) {
    info.color_space = "Unknown";
    info.connection_space = "Unknown";
    return;
  }
  info.version_major = data[8];
  info.version_minor = (data[9] >> 4) & 0xF;
  info.header_intent = static_cast<int>(data[67] & 0x03);
  // Simplified version: directly read color space signature
  auto read_sig = [&](int offset) -> std::string {
    char buf[5] = {(char)data[offset], (char)data[offset + 1], (char)data[offset + 2], (char)data[offset + 3], 0};
    return std::string(buf);
  };
  info.color_space = read_sig(16);
  info.connection_space = read_sig(20);
}

ICCProfileInfo GetSystemDisplayICCProfile(SDL_Window *) {
  ICCProfileInfo info;
  info.source = "Auto ICC detection not implemented on this platform";
  Logger->warn("Auto ICC profile detection is not implemented on this platform");
  return info;
}

} // namespace EYEQ
