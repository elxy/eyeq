#pragma once

// Strict, SDK-independent parsing and validation for the monitoring CLI. Keeping
// this logic out of main() lets the exact integer mode match, the color-target
// mapping and the rejection paths be tested without a device, a GPU or the SDK.
//
// Nothing here guesses: a value that is not exactly one of the accepted forms is
// rejected with a message, and no mode is ever constructed that the native mode
// list did not return.

#include <cstdint>
#include <string>
#include <vector>

#include "output/output_profile.hpp"

namespace EYEQ {

// ---------------------------------------------------------------------------
// Device selector.
//
// The tool never guesses whether a value is an index or a persistent id, because a
// persistent id is usually a plain decimal number too. Selectors are therefore
// explicit and unambiguous:
//   index:<non-negative decimal>
//   id:<signed int64 decimal>
// A bare number, an empty value, a malformed number or an out-of-range index is
// rejected with a message; nothing is silently reinterpreted.
// ---------------------------------------------------------------------------
bool ParseDeviceSelector(const std::string &text, DeviceId *selector, std::string *error);

// ---------------------------------------------------------------------------
// Monitor output selection.
// ---------------------------------------------------------------------------

// Color target the user may request. Only these two coherent SDR/PQ pairs are
// accepted; anything else is rejected instead of silently downgrading to SDR.
enum class MonitorColorTarget { SdrBt709, PqBt2020 };

bool ParseMonitorConnection(const std::string &text, VideoConnection *out, std::string *error);
bool ParseMonitorPixelFormat(const std::string &text, PixelFormat *out, std::string *error);
bool ParseMonitorColorTarget(const std::string &text, MonitorColorTarget *out, std::string *error);

// Parses "WxH@N" or "WxH@N/D" into exact integer mode data (frame_duration=N,
// time_scale=D). No floating point is used, so 30000/1001 does not round to 29.97.
bool ParseMonitorMode(const std::string &text, MonitorMode *out, std::string *error);

// Exact integer-rational match of @p requested against the native @p modes. A mode
// is accepted only when the device listed it; an unsupported request fails with
// the available list formatted into *error. Uses MonitorModeIs (no float compare).
bool SelectMonitorMode(const std::vector<MonitorMode> &modes, const MonitorMode &requested, MonitorMode *out,
                       std::string *error);

// Builds a coherent OutputRequest for the given selection and validates it through
// ValidateRequest. hdr signaling is never fabricated: an SDR target leaves
// hdr_signaling off, a PQ target enables exactly PQ/BT2020 signaling with no
// mastering/CLL values (which ValidateRequest allows to stay unknown).
bool BuildMonitorOutputRequest(const DeviceId &device, const MonitorMode &mode, VideoConnection connection,
                               PixelFormat pixel_format, MonitorColorTarget color_target, double target_nits,
                               OutputRequest *out, std::string *error);

const char *MonitorColorTargetLabel(MonitorColorTarget target);

} // namespace EYEQ
