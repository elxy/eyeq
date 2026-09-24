// Read-only DeckLink device/mode catalog tool.
//
// It enumerates devices and queries supported output modes without opening an
// output, enabling video, scheduling frames, or modifying device configuration.
// Video output is not implemented yet, so this tool cannot play anything.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "output/decklink/monitor_mode.hpp"
#include "output/decklink_backend.hpp"

namespace {

void PrintUsage(const char *program) {
  std::printf("Usage: %s [options]\n"
              "\n"
              "Read-only DeckLink catalog. Lists devices always; lists output modes when\n"
              "--device is given. Does not open an output or modify device settings.\n"
              "\n"
              "Options:\n"
              "  --device <index|persistent-id>  Select a device for the mode query\n"
              "  --connection <sdi|hdmi>         Output connection (default: sdi)\n"
              "  --pixel-format <r210|v210>      Pixel format (default: v210)\n"
              "  -h, --help                      Show this help\n"
              "\n"
              "Exit codes: 0 success, 1 usage/query error, 2 DeckLink driver unavailable.\n",
              program);
}

bool ParseConnection(const std::string &text, EYEQ::VideoConnection *out) {
  if (text == "sdi") {
    *out = EYEQ::VideoConnection::SDI;
    return true;
  }
  if (text == "hdmi") {
    *out = EYEQ::VideoConnection::HDMI;
    return true;
  }
  return false;
}

bool ParsePixelFormat(const std::string &text, EYEQ::PixelFormat *out) {
  if (text == "r210") {
    *out = EYEQ::PixelFormat::R210;
    return true;
  }
  if (text == "v210") {
    *out = EYEQ::PixelFormat::V210;
    return true;
  }
  return false;
}

const char *ConnectionLabel(EYEQ::VideoConnection connection) {
  return connection == EYEQ::VideoConnection::SDI ? "sdi" : "hdmi";
}

const char *PixelFormatLabel(EYEQ::PixelFormat format) { return format == EYEQ::PixelFormat::R210 ? "r210" : "v210"; }

void PrintDevices(const EYEQ::DeviceCatalog &catalog) {
  std::printf("DeckLink API version: %s\n", catalog.api_version.empty() ? "(unknown)" : catalog.api_version.c_str());
  std::printf("Devices: %zu\n", catalog.devices.size());
  for (const EYEQ::DeviceInfo &device : catalog.devices) {
    std::printf("  [%d] model=\"%s\" display=\"%s\"\n", device.index, device.model_name.c_str(),
                device.display_name.c_str());
    if (device.has_persistent_id) {
      std::printf("      persistent_id=%s\n", device.id.persistent_id.c_str());
    } else {
      std::printf("      persistent_id=(none; select by index)\n");
    }
    if (device.profile_attributes_available) {
      std::printf("      playback=%s capture=%s active=%s\n", device.supports_playback ? "yes" : "no",
                  device.supports_capture ? "yes" : "no", device.active ? "yes" : "no");
    } else {
      std::printf("      profile attributes unavailable\n");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  EYEQ::DeviceId selector;
  bool has_selector = false;
  EYEQ::VideoConnection connection = EYEQ::VideoConnection::SDI;
  EYEQ::PixelFormat format = EYEQ::PixelFormat::V210;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (arg == "--device") {
      if (++i >= argc) {
        std::fprintf(stderr, "--device requires a value\n");
        return 1;
      }
      const std::string value = argv[i];
      // A decimal number is treated as an enumeration index; anything else as a
      // persistent id.
      const bool numeric = !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
      if (numeric) {
        selector.index = std::atoi(value.c_str());
        selector.persistent_id.clear();
      } else {
        selector.persistent_id = value;
        selector.index = -1;
      }
      has_selector = true;
      continue;
    }
    if (arg == "--connection") {
      if (++i >= argc || !ParseConnection(argv[i], &connection)) {
        std::fprintf(stderr, "--connection must be sdi or hdmi\n");
        return 1;
      }
      continue;
    }
    if (arg == "--pixel-format") {
      if (++i >= argc || !ParsePixelFormat(argv[i], &format)) {
        std::fprintf(stderr, "--pixel-format must be r210 or v210\n");
        return 1;
      }
      continue;
    }
    std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
    PrintUsage(argv[0]);
    return 1;
  }

  const EYEQ::DeviceCatalog catalog = EYEQ::DeckLinkOutput::Enumerate();
  if (catalog.status != EYEQ::DeviceCatalog::Status::Ok) {
    std::fprintf(stderr, "DeckLink enumeration failed: %s\n", catalog.error.c_str());
    return 2;
  }

  PrintDevices(catalog);

  if (!has_selector) {
    return 0;
  }

  std::vector<EYEQ::MonitorMode> modes;
  std::string error;
  std::printf("\nOutput modes for %s/%s:\n", ConnectionLabel(connection), PixelFormatLabel(format));
  if (!EYEQ::DeckLinkOutput::ListModes(selector, connection, format, &modes, &error)) {
    std::fprintf(stderr, "mode query failed: %s\n", error.c_str());
    return 1;
  }

  if (modes.empty()) {
    std::printf("  (none; the device exists but no mode natively supports this connection and format)\n");
    return 0;
  }
  for (const EYEQ::MonitorMode &mode : modes) {
    std::printf("  %s  frameDuration=%lld timeScale=%lld %s\n", EYEQ::FormatMonitorMode(mode).c_str(),
                static_cast<long long>(mode.frame_duration), static_cast<long long>(mode.time_scale),
                mode.is_progressive ? "progressive" : "interlaced");
  }
  std::printf("Note: host pixel format support does not prove the wiring (e.g. RGB 4:4:4).\n");
  return 0;
}
