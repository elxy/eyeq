#pragma once

// Strict device-selector parsing for the read-only decklink-catalog tool.
//
// The implementation was moved to the shared, SDK-independent monitor CLI header
// so the player and the tools use exactly one parser. This header is kept for the
// existing tool/test include paths.
//
// The selector is explicit and unambiguous:
//   index:<non-negative decimal>
//   id:<signed int64 decimal>
// A bare number, an empty value, a malformed number or an out-of-range index is
// rejected with a message; nothing is silently reinterpreted.

#include "output/monitor_cli.hpp"
