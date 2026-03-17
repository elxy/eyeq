#import <AppKit/AppKit.h>
#include <map>
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include "platform_icc.h"

namespace EYEQ {

extern std::shared_ptr<spdlog::logger> Logger;

/**
 * @brief Extract color space and profile connection space info from ICC profile binary data
 *
 * ICC.4 spec: Header occupies bytes 0-127
 * Offset 16-19: Device color space signature (4 bytes)
 * Offset 20-23: Profile connection space (PCS) signature (4 bytes)
 */
void ParseICCProfileHeader(ICCProfileInfo &info) {
  const auto &data = info.data;
  if (data.size() < 128) {
    info.color_space = "Unknown";
    info.connection_space = "Unknown";
    return;
  }

  // Read profile version (offset 8-11)
  // Byte 8: major version, Byte 9: minor.bugfix (upper/lower nibble)
  info.version_major = data[8];
  info.version_minor = (data[9] >> 4) & 0xF;

  // Read device color space signature (offset 16-19)
  uint32_t dcs_sig = (static_cast<uint32_t>(data[16]) << 24) | (static_cast<uint32_t>(data[17]) << 16) |
                     (static_cast<uint32_t>(data[18]) << 8) | static_cast<uint32_t>(data[19]);

  // Read PCS signature (offset 20-23)
  uint32_t pcs_sig = (static_cast<uint32_t>(data[20]) << 24) | (static_cast<uint32_t>(data[21]) << 16) |
                     (static_cast<uint32_t>(data[22]) << 8) | static_cast<uint32_t>(data[23]);

  // Read rendering intent (offset 64-67, only the lower 2 bits of offset 67 are used)
  info.header_intent = static_cast<int>(data[67] & 0x03);

  // Convert 4-byte signature to string
  auto sig_to_string = [](uint32_t sig) -> std::string {
    char buf[5] = {static_cast<char>((sig >> 24) & 0xFF), static_cast<char>((sig >> 16) & 0xFF),
                   static_cast<char>((sig >> 8) & 0xFF), static_cast<char>(sig & 0xFF), '\0'};
    std::string result(buf);
    // Check if all characters are printable
    bool all_printable = true;
    for (char c : result) {
      if (c < 32 || c > 126) {
        all_printable = false;
        break;
      }
    }
    if (!all_printable) {
      result = fmt::format("0x{:08X}", sig);
    }
    return result;
  };

  info.color_space = sig_to_string(dcs_sig);
  info.connection_space = sig_to_string(pcs_sig);

  // Standardized name mapping
  std::map<std::string, std::string> sig_names{
      {"RGB ", "RGB"},        {"GRAY", "Grayscale"},  {"CMYK", "CMYK"},
      {"Lab ", "Lab"},        {"XYZ ", "XYZ"},        {"Luv ", "Luv"},
      {"YCbr", "YCbCr"},      {"Yxy ", "Yxy"},        {"HSV ", "HSV"},
      {"HLS ", "HLS"},        {"SCNR", "Scanner RGB"}};

  if (sig_names.count(info.color_space))
    info.color_space = sig_names[info.color_space];
  if (sig_names.count(info.connection_space))
    info.connection_space = sig_names[info.connection_space];
}

ICCProfileInfo GetSystemDisplayICCProfile(SDL_Window *window) {
  ICCProfileInfo info;

  if (!window) {
    info.source = "Invalid SDL window";
    return info;
  }

  NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  if (!nswindow) {
    info.source = "Failed to get NSWindow from SDL_Window";
    return info;
  }

  NSColorSpace *colorSpace = [[nswindow screen] colorSpace];
  if (!colorSpace) {
    info.source = "NSColorSpace is nil";
    return info;
  }

  NSData *iccData = [colorSpace ICCProfileData];
  if (!iccData) {
    info.source = "ICCProfileData is nil";
    return info;
  }

  // Copy ICC profile data
  auto *bytes = (const uint8_t *)[iccData bytes];
  info.data = std::vector<uint8_t>(bytes, bytes + [iccData length]);

  // Get profile name
  if ([colorSpace respondsToSelector:@selector(localizedName)]) {
    NSString *name = [colorSpace localizedName];
    if (name) {
      info.name = [name UTF8String];
    }
  }

  // If name is empty, try to parse from ICC header
  if (info.name.empty()) {
    info.name = fmt::format("System Display Profile ({} bytes)", info.data.size());
  }

  // Parse color space information
  ParseICCProfileHeader(info);

  // Mark as system profile
  info.is_system_profile = true;
  NSScreen *screen = [nswindow screen];
  std::string screen_name_str = "Unknown Display";
  if (screen && [screen respondsToSelector:@selector(localizedName)]) {
    NSString *name = [screen localizedName];
    if (name) {
      screen_name_str = [name UTF8String];
    }
  }
  info.source = fmt::format("macOS system display ({})", screen_name_str);

  return info;
}

} // namespace EYEQ
