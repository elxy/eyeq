// SDK-facing implementation of the read-only DeckLink catalog and mode query.
// All DeckLink COM types are confined to this translation unit; the public header
// does not include any SDK type.
#include "output/decklink_backend.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "DeckLinkAPI.h"

namespace EYEQ {

namespace {

void SetError(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

std::string CfStringToString(CFStringRef value) {
  if (value == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(value);
  const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  if (capacity <= 0) {
    return {};
  }
  std::string buffer(static_cast<std::size_t>(capacity), '\0');
  if (!CFStringGetCString(value, buffer.data(), capacity, kCFStringEncodingUTF8)) {
    return {};
  }
  return std::string(buffer.c_str());
}

std::string QueryApiVersion() {
  IDeckLinkAPIInformation *information = CreateDeckLinkAPIInformationInstance();
  if (information == nullptr) {
    return {};
  }
  int64_t version = 0;
  std::string result;
  if (information->GetInt(BMDDeckLinkAPIVersion, &version) == S_OK) {
    result = std::to_string((version >> 24) & 0xFF) + "." + std::to_string((version >> 16) & 0xFF) + "." +
             std::to_string((version >> 8) & 0xFF);
  }
  information->Release();
  return result;
}

BMDVideoConnection ToSdkConnection(VideoConnection connection) {
  switch (connection) {
  case VideoConnection::SDI:
    return bmdVideoConnectionSDI;
  case VideoConnection::HDMI:
    return bmdVideoConnectionHDMI;
  }
  return bmdVideoConnectionUnspecified;
}

BMDPixelFormat ToSdkPixelFormat(PixelFormat format) {
  switch (format) {
  case PixelFormat::R210:
    return bmdFormat10BitRGB;
  case PixelFormat::V210:
    return bmdFormat10BitYUV;
  }
  return 0;
}

// Reads a device's identity and capabilities. Never opens an output.
DeviceInfo ReadDeviceInfo(IDeckLink *device, int index) {
  DeviceInfo info;
  info.index = index;
  info.id.index = index;

  CFStringRef name = nullptr;
  if (device->GetDisplayName(&name) == S_OK && name != nullptr) {
    info.display_name = CfStringToString(name);
    CFRelease(name);
  }
  name = nullptr;
  if (device->GetModelName(&name) == S_OK && name != nullptr) {
    info.model_name = CfStringToString(name);
    CFRelease(name);
  }

  IDeckLinkProfileAttributes *attributes = nullptr;
  if (device->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void **>(&attributes)) != S_OK ||
      attributes == nullptr) {
    return info;
  }
  info.profile_attributes_available = true;

  int64_t value = 0;
  if (attributes->GetInt(BMDDeckLinkPersistentID, &value) == S_OK) {
    info.has_persistent_id = true;
    info.id.persistent_id = std::to_string(value);
  }
  int64_t video_io = 0;
  if (attributes->GetInt(BMDDeckLinkVideoIOSupport, &video_io) == S_OK) {
    info.supports_playback = (video_io & bmdDeviceSupportsPlayback) != 0;
    info.supports_capture = (video_io & bmdDeviceSupportsCapture) != 0;
  }
  int64_t duplex = 0;
  if (attributes->GetInt(BMDDeckLinkDuplex, &duplex) == S_OK) {
    info.active = duplex != bmdDuplexInactive;
  }
  attributes->Release();
  return info;
}

// Finds the device matching a selector and returns its IDeckLinkOutput. The caller
// owns *out. Read-only: does not enable or reconfigure the output.
bool ResolveOutput(const DeviceId &selector, IDeckLinkOutput **out, std::string *error) {
  *out = nullptr;
  if (selector.persistent_id.empty() && selector.index < 0) {
    SetError(error, "no device selector: provide a persistent id or an index");
    return false;
  }

  IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
  if (iterator == nullptr) {
    SetError(error, "DeckLink iterator unavailable: the Desktop Video framework/driver is not loaded");
    return false;
  }

  IDeckLink *device = nullptr;
  int index = 0;
  while (iterator->Next(&device) == S_OK && device != nullptr) {
    const int current = index++;
    bool has_persistent_id = false;
    std::string persistent_id;
    IDeckLinkProfileAttributes *attributes = nullptr;
    if (device->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void **>(&attributes)) == S_OK &&
        attributes != nullptr) {
      int64_t value = 0;
      if (attributes->GetInt(BMDDeckLinkPersistentID, &value) == S_OK) {
        has_persistent_id = true;
        persistent_id = std::to_string(value);
      }
      attributes->Release();
    }

    const bool matches = !selector.persistent_id.empty()
                             ? (has_persistent_id && persistent_id == selector.persistent_id)
                             : (current == selector.index);
    if (!matches) {
      device->Release();
      device = nullptr;
      continue;
    }

    const HRESULT result = device->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void **>(out));
    device->Release();
    iterator->Release();
    if (result != S_OK || *out == nullptr) {
      SetError(error, "device does not expose the IDeckLinkOutput interface");
      return false;
    }
    return true;
  }

  iterator->Release();
  if (!selector.persistent_id.empty()) {
    SetError(error, "device not found: " + selector.persistent_id);
  } else {
    SetError(error, "device index " + std::to_string(selector.index) + " not found");
  }
  return false;
}

bool SameMode(const MonitorMode &a, const MonitorMode &b) {
  return a.width == b.width && a.height == b.height && a.frame_duration == b.frame_duration &&
         a.time_scale == b.time_scale && a.is_progressive == b.is_progressive;
}

} // namespace

DeviceCatalog DeckLinkOutput::Enumerate() {
  DeviceCatalog catalog;
  catalog.api_version = QueryApiVersion();

  IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
  if (iterator == nullptr) {
    catalog.status = DeviceCatalog::Status::IteratorUnavailable;
    catalog.error = "DeckLink iterator unavailable: the Desktop Video framework/driver is not loaded";
    return catalog;
  }

  IDeckLink *device = nullptr;
  int index = 0;
  while (iterator->Next(&device) == S_OK && device != nullptr) {
    catalog.devices.push_back(ReadDeviceInfo(device, index++));
    device->Release();
    device = nullptr;
  }
  iterator->Release();
  return catalog;
}

bool DeckLinkOutput::ListModes(const DeviceId &device, VideoConnection connection, PixelFormat format,
                               std::vector<MonitorMode> *out, std::string *error) {
  if (out == nullptr) {
    SetError(error, "output vector is null");
    return false;
  }
  out->clear();

  const BMDVideoConnection sdk_connection = ToSdkConnection(connection);
  if (sdk_connection == bmdVideoConnectionUnspecified) {
    SetError(error, "unknown video connection");
    return false;
  }
  const BMDPixelFormat sdk_format = ToSdkPixelFormat(format);
  if (sdk_format == 0) {
    SetError(error, "unknown pixel format");
    return false;
  }

  IDeckLinkOutput *output = nullptr;
  if (!ResolveOutput(device, &output, error)) {
    return false;
  }

  IDeckLinkDisplayModeIterator *mode_iterator = nullptr;
  if (output->GetDisplayModeIterator(&mode_iterator) != S_OK || mode_iterator == nullptr) {
    output->Release();
    SetError(error, "could not obtain the display mode iterator");
    return false;
  }

  IDeckLinkDisplayMode *mode = nullptr;
  while (mode_iterator->Next(&mode) == S_OK && mode != nullptr) {
    const BMDDisplayMode requested = mode->GetDisplayMode();
    BMDDisplayMode actual = requested;
    bool supported = false;
    const HRESULT result =
        output->DoesSupportVideoMode(sdk_connection, requested, sdk_format, bmdNoVideoOutputConversion,
                                     bmdSupportedVideoModeDefault, &actual, &supported);
    // Native support only: a mode requiring output conversion is not reported.
    if (result == S_OK && supported && (actual == requested || actual == bmdModeUnknown)) {
      MonitorMode monitor_mode;
      monitor_mode.width = static_cast<int>(mode->GetWidth());
      monitor_mode.height = static_cast<int>(mode->GetHeight());
      BMDTimeValue frame_duration = 0;
      BMDTimeScale time_scale = 0;
      if (mode->GetFrameRate(&frame_duration, &time_scale) == S_OK) {
        monitor_mode.frame_duration = frame_duration;
        monitor_mode.time_scale = time_scale;
      }
      monitor_mode.is_progressive = mode->GetFieldDominance() == bmdProgressiveFrame;

      const bool duplicate = std::any_of(out->begin(), out->end(),
                                         [&](const MonitorMode &existing) { return SameMode(existing, monitor_mode); });
      if (!duplicate) {
        out->push_back(monitor_mode);
      }
    }
    mode->Release();
    mode = nullptr;
  }

  mode_iterator->Release();
  output->Release();
  return true;
}

} // namespace EYEQ
