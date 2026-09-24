// SDK-facing implementation of the read-only DeckLink catalog and mode query.
// All DeckLink COM types are confined to this translation unit; the public header
// does not include any SDK type.
#include "output/decklink_backend.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "DeckLinkAPI.h"

namespace EYEQ {

namespace {

void SetError(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

// A stable, greppable hex rendering of an HRESULT. It never guesses a meaning.
std::string FormatHresult(HRESULT result) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(result));
  return buffer;
}

// FourCC rendering of a BMDDisplayMode for error context.
std::string FormatDisplayMode(BMDDisplayMode mode) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(mode));
  return buffer;
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
  // Each capability is recorded independently: a failed read stays unknown instead of
  // being reported as a definite false/inactive.
  int64_t video_io = 0;
  if (attributes->GetInt(BMDDeckLinkVideoIOSupport, &video_io) == S_OK) {
    info.supports_playback = (video_io & bmdDeviceSupportsPlayback) != 0;
    info.supports_playback_known = true;
    info.supports_capture = (video_io & bmdDeviceSupportsCapture) != 0;
    info.supports_capture_known = true;
  }
  int64_t duplex = 0;
  if (attributes->GetInt(BMDDeckLinkDuplex, &duplex) == S_OK) {
    info.active = duplex != bmdDuplexInactive;
    info.active_known = true;
  }
  attributes->Release();
  return info;
}

// Finds the device matching a selector and returns its IDeckLinkOutput. The caller
// owns *out. Read-only: does not enable or reconfigure the output.
//
// Frozen DeviceId semantics: a non-empty persistent_id is preferred and matched first;
// index is only a fallback when persistent_id is empty. Both fields may coexist (that
// is exactly what Enumerate() returns). A specified persistent id that is not found
// fails; it never falls back to the index.
bool ResolveOutput(const DeviceId &selector, IDeckLinkOutput **out, std::string *error) {
  *out = nullptr;
  if (selector.persistent_id.empty() && selector.index < 0) {
    SetError(error, "no device selector: provide a persistent id or an index");
    return false;
  }

  IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
  if (iterator == nullptr) {
    SetError(error, "DeckLink iterator could not be created: Desktop Video framework/driver unavailable or API "
                    "version unsupported");
    return false;
  }

  IDeckLink *device = nullptr;
  int index = 0;
  while (true) {
    const HRESULT next_result = iterator->Next(&device);
    if (next_result == S_FALSE) {
      break; // normal end of enumeration
    }
    if (next_result != S_OK) {
      iterator->Release();
      SetError(error, "IDeckLinkIterator::Next failed (HRESULT " + FormatHresult(next_result) + ") after " +
                          std::to_string(index) + " device(s)");
      return false;
    }
    if (device == nullptr) {
      iterator->Release();
      SetError(error, "IDeckLinkIterator::Next returned S_OK with a null device");
      return false;
    }

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
      SetError(error, "device does not expose the IDeckLinkOutput interface (HRESULT " + FormatHresult(result) + ")");
      return false;
    }
    return true;
  }

  iterator->Release();
  if (!selector.persistent_id.empty()) {
    SetError(error, "device not found: persistent id " + selector.persistent_id);
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
    catalog.error = "DeckLink iterator could not be created: Desktop Video framework/driver unavailable or API "
                    "version unsupported";
    return catalog;
  }

  IDeckLink *device = nullptr;
  int index = 0;
  while (true) {
    const HRESULT next_result = iterator->Next(&device);
    if (next_result == S_FALSE) {
      break; // normal end of enumeration
    }
    if (next_result != S_OK) {
      // A hard failure is not EOF: discard the partial list instead of presenting it
      // as a complete result.
      if (device != nullptr) {
        device->Release();
      }
      iterator->Release();
      catalog.devices.clear();
      catalog.status = DeviceCatalog::Status::InternalError;
      catalog.error = "IDeckLinkIterator::Next failed (HRESULT " + FormatHresult(next_result) + ") after " +
                      std::to_string(index) + " device(s)";
      return catalog;
    }
    if (device == nullptr) {
      iterator->Release();
      catalog.devices.clear();
      catalog.status = DeviceCatalog::Status::InternalError;
      catalog.error = "IDeckLinkIterator::Next returned S_OK with a null device";
      return catalog;
    }
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
  const HRESULT iterator_result = output->GetDisplayModeIterator(&mode_iterator);
  if (iterator_result != S_OK || mode_iterator == nullptr) {
    output->Release();
    SetError(error, "GetDisplayModeIterator failed (HRESULT " + FormatHresult(iterator_result) + ")");
    return false;
  }

  // Accumulate into a local list and commit only after the whole iteration succeeds,
  // so a failure after some modes never leaves a partial list behind.
  std::vector<MonitorMode> modes;
  int mode_count = 0;
  bool ok = true;
  while (ok) {
    IDeckLinkDisplayMode *mode = nullptr;
    const HRESULT next_result = mode_iterator->Next(&mode);
    if (next_result == S_FALSE) {
      break; // normal end of the mode list
    }
    if (next_result != S_OK) {
      // A failure HRESULT is a hard error even when mode is null.
      ok = false;
      SetError(error, "IDeckLinkDisplayModeIterator::Next failed (HRESULT " + FormatHresult(next_result) + ") after " +
                          std::to_string(mode_count) + " mode(s)");
      break;
    }
    if (mode == nullptr) {
      // SDK 16.0 documents a null displayMode with S_OK as the end of the display-mode
      // list (alongside S_FALSE handled above). Both are normal termination; the modes
      // already read are kept.
      break;
    }
    ++mode_count;

    const BMDDisplayMode requested = mode->GetDisplayMode();
    BMDDisplayMode actual = requested;
    bool supported = false;
    const HRESULT support_result =
        output->DoesSupportVideoMode(sdk_connection, requested, sdk_format, bmdNoVideoOutputConversion,
                                     bmdSupportedVideoModeDefault, &actual, &supported);
    if (support_result != S_OK) {
      ok = false;
      SetError(error, "DoesSupportVideoMode failed (HRESULT " + FormatHresult(support_result) + ") for mode " +
                          FormatDisplayMode(requested));
      mode->Release();
      break;
    }
    // supported == false and a substituted mode are legitimate "not natively available"
    // results, not query failures: skip the mode without failing the whole list.
    if (!supported || (actual != requested && actual != bmdModeUnknown)) {
      mode->Release();
      continue;
    }

    const long width = mode->GetWidth();
    const long height = mode->GetHeight();
    const bool is_progressive = mode->GetFieldDominance() == bmdProgressiveFrame;
    BMDTimeValue frame_duration = 0;
    BMDTimeScale time_scale = 0;
    const HRESULT rate_result = mode->GetFrameRate(&frame_duration, &time_scale);
    mode->Release();
    if (rate_result != S_OK) {
      ok = false;
      SetError(error, "GetFrameRate failed (HRESULT " + FormatHresult(rate_result) + ") for mode " +
                          FormatDisplayMode(requested));
      break;
    }
    if (frame_duration <= 0 || time_scale <= 0) {
      // A non-positive time base is an invalid result; never publish it as a mode.
      ok = false;
      SetError(error, "GetFrameRate returned an invalid time base for mode " + FormatDisplayMode(requested) +
                          " (duration=" + std::to_string(frame_duration) + " scale=" + std::to_string(time_scale) +
                          ")");
      break;
    }

    MonitorMode monitor_mode;
    monitor_mode.width = static_cast<int>(width);
    monitor_mode.height = static_cast<int>(height);
    monitor_mode.frame_duration = frame_duration;
    monitor_mode.time_scale = time_scale;
    monitor_mode.is_progressive = is_progressive;

    const bool duplicate = std::any_of(modes.begin(), modes.end(),
                                       [&](const MonitorMode &existing) { return SameMode(existing, monitor_mode); });
    if (!duplicate) {
      modes.push_back(monitor_mode);
    }
  }

  mode_iterator->Release();
  output->Release();

  if (!ok) {
    out->clear();
    return false;
  }
  *out = std::move(modes);
  return true;
}

} // namespace EYEQ
