#pragma once

namespace EYEQ {

/**
 * @brief Activate the application and bring it to the foreground
 *
 * macOS: Uses NSRunningApplication API to force app activation, which is
 *        more reliable than SDL_RaiseWindow alone when the app has lost
 *        foreground status (e.g., user clicked another window during startup).
 * Other platforms: No-op (SDL_RaiseWindow handles activation sufficiently).
 */
void ActivateApp();

} // namespace EYEQ
