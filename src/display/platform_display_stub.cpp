#include "platform_display.h"

namespace EYEQ {

DisplayEDRInfo QueryDisplayEDR(SDL_Window *) {
  DisplayEDRInfo info;
  info.error = "EDR query is not implemented on this platform";
  return info;
}

} // namespace EYEQ
