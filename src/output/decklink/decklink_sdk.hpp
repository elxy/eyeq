#pragma once

// Internal SDK-facing helpers shared by the read-only catalog and the output
// backend. This header includes DeckLink COM types, so it must only be included by
// backend translation units and is never part of the public API.

#include <string>

#include "DeckLinkAPI.h"
#include "output/output_profile.hpp"

namespace EYEQ {
namespace internal {

// Sets a human-readable error message when error is not null.
void SetError(std::string *error, const std::string &message);
std::string FormatHresult(HRESULT result);
std::string FormatDisplayMode(BMDDisplayMode mode);
BMDVideoConnection ToSdkConnection(VideoConnection connection);
BMDPixelFormat ToSdkPixelFormat(PixelFormat format);

// Resolves a device selector to an IDeckLink with one reference for the caller.
// persistent_id is preferred; index is only a fallback when persistent_id is empty.
// A specified persistent id that is not found never falls back to the index.
bool ResolveDevice(const DeviceId &selector, IDeckLink **out, std::string *error);

// Resolves a device and obtains its IDeckLinkOutput interface (one reference for the
// caller). Read-only: does not enable or reconfigure the output.
bool ResolvePlaybackOutput(const DeviceId &selector, IDeckLinkOutput **out, std::string *error);

} // namespace internal
} // namespace EYEQ
