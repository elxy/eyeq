#pragma once

#include <string>

struct SDL_Window;

namespace EYEQ {

/**
 * @brief EDR state of the display that hosts a window
 *
 * Headroom values are ratios relative to the SDR white (EDR value 1.0). The
 * public macOS API gives no absolute luminance; the preset fields come from the
 * private MonitorPanel framework and only exist for displays with presets
 * (Apple XDR displays).
 */
struct DisplayEDRInfo {
  bool valid = false; // false when the platform has no EDR query or the window has no screen
  std::string error;  // why valid is false
  std::string display_name;
  float potential_headroom = 1.0f; // hardware limit, independent of brightness and preset
  float reference_headroom = 0.0f; // > 0 only when the display is in a reference mode

  bool preset_valid = false;
  std::string preset_error; // why preset_valid is false
  std::string preset_name;
  float preset_peak_nits = 0.0f;      // PresetMaxHDRLuminance
  float preset_sdr_white_nits = 0.0f; // PresetMaxSDRLuminance
};

/**
 * @brief Query the EDR state of the display that currently hosts the window
 *
 * macOS: NSScreen EDR properties plus the active preset via MonitorPanel.
 * Other platforms: returns an invalid result.
 * Must be called on the UI thread.
 */
DisplayEDRInfo QueryDisplayEDR(SDL_Window *window);

} // namespace EYEQ
