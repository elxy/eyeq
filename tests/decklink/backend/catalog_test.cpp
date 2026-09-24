// Read-only SDK catalog / mode-query tests. Requires the DeckLink SDK headers but
// NOT a device or the Desktop Video driver. It never opens an output.
//
// When no driver is present, this test only verifies that the failure is reported
// truthfully; it does not fabricate a hardware result.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "output/decklink_backend.hpp"

using namespace EYEQ;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

int main() {
  const DeviceCatalog catalog = DeckLinkOutput::Enumerate();

  if (catalog.status != DeviceCatalog::Status::Ok) {
    // Environment condition. The failure must be reported, not faked as success.
    CHECK(!catalog.error.empty());
    std::printf("decklink catalog test: enumeration unavailable (%s)\n", catalog.error.c_str());
    return 0;
  }

  for (std::size_t i = 0; i < catalog.devices.size(); ++i) {
    CHECK(catalog.devices[i].index == static_cast<int>(i));
    CHECK(catalog.devices[i].has_persistent_id == !catalog.devices[i].id.persistent_id.empty());
    if (catalog.devices[i].profile_attributes_available && catalog.devices[i].has_persistent_id) {
      CHECK(!catalog.devices[i].id.persistent_id.empty());
    }
    // Without the attributes interface no capability may be reported as known.
    if (!catalog.devices[i].profile_attributes_available) {
      CHECK(!catalog.devices[i].supports_playback_known);
      CHECK(!catalog.devices[i].supports_capture_known);
      CHECK(!catalog.devices[i].active_known);
    }
  }

  std::vector<MonitorMode> modes;
  std::string error;

  // No selector -> hard failure.
  CHECK(!DeckLinkOutput::ListModes(DeviceId{}, VideoConnection::SDI, PixelFormat::V210, &modes, &error));
  CHECK(!error.empty());

  // Nonexistent persistent id -> hard failure.
  error.clear();
  DeviceId bogus_id;
  bogus_id.persistent_id = "__eyeq_no_such_device__";
  CHECK(!DeckLinkOutput::ListModes(bogus_id, VideoConnection::SDI, PixelFormat::V210, &modes, &error));
  CHECK(!error.empty());

  // Nonexistent index -> hard failure.
  error.clear();
  DeviceId bogus_index;
  bogus_index.index = 1 << 20;
  CHECK(!DeckLinkOutput::ListModes(bogus_index, VideoConnection::HDMI, PixelFormat::R210, &modes, &error));
  CHECK(!error.empty());

  // Unknown enum values -> hard failure before any device query.
  error.clear();
  CHECK(!DeckLinkOutput::ListModes(DeviceId{}, static_cast<VideoConnection>(999), PixelFormat::V210, &modes, &error));
  CHECK(!error.empty());
  error.clear();
  CHECK(!DeckLinkOutput::ListModes(DeviceId{}, VideoConnection::SDI, static_cast<PixelFormat>(999), &modes, &error));
  CHECK(!error.empty());

  // Real devices: report what the query returns. If it succeeds the list may be
  // empty; we never assert a hardware mode result.
  for (const DeviceInfo &device : catalog.devices) {
    // Skip only when playback is known to be unsupported; an unknown capability is
    // still worth probing.
    if (device.supports_playback_known && !device.supports_playback) {
      continue;
    }
    for (VideoConnection connection : {VideoConnection::SDI, VideoConnection::HDMI}) {
      modes.clear();
      error.clear();
      if (DeckLinkOutput::ListModes(device.id, connection, PixelFormat::V210, &modes, &error)) {
        std::printf("device[%d] %s: %s/v210 query ok, %zu native mode(s)\n", device.index, device.model_name.c_str(),
                    connection == VideoConnection::SDI ? "SDI" : "HDMI", modes.size());
        for (const MonitorMode &mode : modes) {
          CHECK(mode.width > 0 && mode.height > 0);
          CHECK(mode.frame_duration > 0 && mode.time_scale > 0);
        }
      } else {
        std::printf("device[%d] %s: %s/v210 query failed: %s\n", device.index, device.model_name.c_str(),
                    connection == VideoConnection::SDI ? "SDI" : "HDMI", error.c_str());
        CHECK(!error.empty());
      }
    }
  }

  std::printf("decklink catalog test: OK (%zu device(s))\n", catalog.devices.size());
  return 0;
}
