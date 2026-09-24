#pragma once

// Strict device-selector parsing for the read-only decklink-catalog tool.
//
// The tool never guesses whether a value is an index or a persistent id, because a
// persistent id is usually a plain decimal number too. Selectors are therefore
// explicit and unambiguous:
//   index:<non-negative decimal>
//   id:<signed int64 decimal>
// A bare number, an empty value, a malformed number or an out-of-range index is
// rejected with a message; nothing is silently reinterpreted.

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

#include "output/output_profile.hpp"

namespace EYEQ {

// Writes the parsed selector into *selector and returns true. On failure leaves the
// selector unset, sets *error and returns false. error must not be null.
inline bool ParseDeviceSelector(const std::string &text, DeviceId *selector, std::string *error) {
  selector->persistent_id.clear();
  selector->index = -1;

  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    *error = "device selector '" + text + "' must be 'index:<n>' or 'id:<value>'";
    return false;
  }

  const std::string kind = text.substr(0, colon);
  const std::string value = text.substr(colon + 1);
  if (value.empty()) {
    *error = "device selector '" + kind + ":' is missing a value";
    return false;
  }

  if (kind == "index") {
    int64_t index = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, index);
    if (result.ec != std::errc() || result.ptr != end || index < 0 ||
        index > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      *error = "invalid device index '" + value + "': expected a non-negative integer that fits in int";
      return false;
    }
    selector->index = static_cast<int>(index);
    return true;
  }

  if (kind == "id") {
    int64_t id = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, id);
    if (result.ec != std::errc() || result.ptr != end) {
      *error = "invalid persistent id '" + value + "': expected a decimal int64";
      return false;
    }
    selector->persistent_id = std::to_string(id);
    return true;
  }

  *error = "unknown device selector kind '" + kind + "': use 'index:' or 'id:'";
  return false;
}

} // namespace EYEQ
