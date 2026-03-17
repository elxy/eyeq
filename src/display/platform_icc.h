#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace EYEQ {

/**
 * @brief ICC profile information structure
 */
struct ICCProfileInfo {
  std::vector<uint8_t> data;      // ICC profile binary data
  std::string name;               // Profile name (macOS ColorSpace localizedName or ICC desc tag)
  std::string color_space;        // Color space ("RGB", "CMYK", "Lab", "XYZ", etc.)
  std::string connection_space;   // Profile connection space (PCS: typically "XYZ" or "Lab")
  int version_major = 0;          // ICC profile version number (major)
  int version_minor = 0;          // ICC profile version number (minor)
  int header_intent = -1;         // Preferred rendering intent declared in ICC profile header
  bool is_system_profile;         // Whether this is a system profile
  std::string source;             // Source description (for debug output)
};

/**
 * @brief Get the ICC profile information for the current display
 *
 * macOS: Obtained via NSScreen.colorSpace
 * Windows/Linux: Not yet implemented, returns empty data
 *
 * @param window SDL window, used to determine which display it is on
 * @return ICC profile information structure
 */
ICCProfileInfo GetSystemDisplayICCProfile(SDL_Window *window);

/**
 * @brief Extract metadata from ICC profile binary data
 *
 * Parses the Header (first 128 bytes) per the ICC.4 spec
 * - Offset 8-11: Profile version
 * - Offset 16-19: Device color space signature
 * - Offset 20-23: Profile connection space (PCS) signature
 * - Offset 64-67: Rendering intent
 */
void ParseICCProfileHeader(ICCProfileInfo &info);

} // namespace EYEQ
