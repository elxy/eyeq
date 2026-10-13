#pragma once

// Strict, SDK-independent parsing and validation for the monitoring CLI. Keeping
// this logic out of main() lets the exact integer mode match, the color-target
// mapping and the rejection paths be tested without a device, a GPU or the SDK.
//
// Nothing here guesses: a value that is not exactly one of the accepted forms is
// rejected with a message, and no mode is ever constructed that the native mode
// list did not return. `auto` is always an explicit, logged choice, never a
// silent fallback for an unspecified value.

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

// Color target the user may request. `Auto` follows the main video's transfer
// function (PQ/HLG -> pq2020, anything else -> sdr709) and is resolved explicitly
// before any output is built.
enum class MonitorColorTarget { Auto, SdrBt709, PqBt2020 };

// Connection selection as typed on the CLI. `Auto` follows the device's already
// configured/active output connection; sdi/hdmi are explicit.
enum class MonitorConnectionChoice { Auto, Sdi, Hdmi };

// Neutral view of a device's output connections, filled by the catalog from the
// SDK bit fields so this header stays SDK-free. `known` is false when neither the
// configured nor the capability query succeeded; a `false`/`false` with `known`
// true means the device genuinely reported no SDI/HDMI output.
struct MonitorConnections {
  bool known = false;
  bool sdi = false;
  bool hdmi = false;
};

bool ParseMonitorConnectionChoice(const std::string &text, MonitorConnectionChoice *out, std::string *error);
const char *MonitorConnectionChoiceLabel(MonitorConnectionChoice choice);

// Resolves a connection choice against a device's active/configured connectors.
// `Auto` prefers SDI when the device reports it, then HDMI. An unresolvable auto
// (unknown mask, or neither connector) fails instead of guessing. On success
// *reason describes how the choice was made (for the startup log).
bool ResolveMonitorConnection(MonitorConnectionChoice requested, const MonitorConnections &connections,
                              VideoConnection *out, std::string *reason, std::string *error);

bool ParseMonitorPixelFormat(const std::string &text, PixelFormat *out, std::string *error);
bool ParseMonitorColorTarget(const std::string &text, MonitorColorTarget *out, std::string *error);

// Resolves `Auto` from the mapping target (--target-display-nits): a HDR target
// in nits picks pq2020, `sdr` picks sdr709. An explicit target is passed through
// unchanged; the caller rejects an explicit value that contradicts the mapping
// target.
bool ResolveMonitorColorTarget(MonitorColorTarget requested, bool target_is_hdr, MonitorColorTarget *out,
                               std::string *reason);

// Parses "WxH@N" or "WxH@N/D" into exact integer mode data (frame_duration=N,
// time_scale=D). No floating point is used, so 30000/1001 does not round to 29.97.
bool ParseMonitorMode(const std::string &text, MonitorMode *out, std::string *error);

// Parses either "auto" or an exact mode. On success exactly one of *is_auto or
// *explicit_mode is meaningful; "auto" is never silently treated as a mode.
bool ParseMonitorModeChoice(const std::string &text, bool *is_auto, MonitorMode *explicit_mode, std::string *error);

// Exact integer-rational match of @p requested against the native @p modes. A mode
// is accepted only when the device listed it; an unsupported request fails with
// the available list formatted into *error. Uses MonitorModeIs (no float compare).
bool SelectMonitorMode(const std::vector<MonitorMode> &modes, const MonitorMode &requested, MonitorMode *out,
                       std::string *error);

// Auto-selects the native mode closest to the main video. Selection is
// deterministic: exact rate first, then the lowest rate >= the source, then the
// highest rate below it; within the chosen rate the smallest mode that covers the
// source is preferred, otherwise the largest. Only progressive modes that satisfy
// IsMonitorModeWhitelisted are considered when any are available. Fails with a
// message when the source geometry/rate is unknown instead of guessing.
bool SelectMonitorModeForSource(const std::vector<MonitorMode> &modes, int source_width, int source_height,
                                double source_fps, MonitorMode *out, std::string *reason, std::string *error);

// Temporary backend constraint (see DeckLinkOutput::Open): the first version only
// supports these progressive modes. Kept in one place so auto selection and the
// backend agree on what is actually openable.
bool IsMonitorModeWhitelisted(const MonitorMode &mode);

// Builds a coherent OutputRequest for the given selection and validates it through
// ValidateRequest. hdr signaling is never fabricated: an SDR target leaves
// hdr_signaling off, a PQ target enables exactly PQ/BT2020 signaling with no
// mastering/CLL values (which ValidateRequest allows to stay unknown). `color_target`
// must already be resolved (not Auto).
bool BuildMonitorOutputRequest(const DeviceId &device, const MonitorMode &mode, VideoConnection connection,
                               PixelFormat pixel_format, MonitorColorTarget color_target, double target_nits,
                               OutputRequest *out, std::string *error);

const char *MonitorColorTargetLabel(MonitorColorTarget target);

} // namespace EYEQ
