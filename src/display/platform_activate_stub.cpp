#include "platform_activate.h"

namespace EYEQ {

void ActivateApp() {
  // No-op on non-macOS platforms; SDL_RaiseWindow handles activation sufficiently.
}

} // namespace EYEQ
