#include "render.hpp"

using namespace EYEQ;

// Reference white level (normalized) for each transfer function, used by the
// compositors when drawing the OSD/grid lines in the target color space. Kept in
// its own translation unit so offscreen render tests can link the compositors
// without the window present path.
float EYEQ::reference_white(enum pl_color_transfer trc) {
  switch (trc) {
  case PL_COLOR_TRC_BT_1886:
  case PL_COLOR_TRC_SRGB:
  case PL_COLOR_TRC_LINEAR:
  case PL_COLOR_TRC_GAMMA18:
  case PL_COLOR_TRC_GAMMA20:
  case PL_COLOR_TRC_GAMMA22:
  case PL_COLOR_TRC_GAMMA24:
  case PL_COLOR_TRC_GAMMA26:
  case PL_COLOR_TRC_GAMMA28:
  case PL_COLOR_TRC_PRO_PHOTO:
  case PL_COLOR_TRC_ST428:
    return 1.0;
  case PL_COLOR_TRC_PQ:
    return 0.580689;
  case PL_COLOR_TRC_HLG:
    return 0.749877;
  case PL_COLOR_TRC_V_LOG:
  case PL_COLOR_TRC_S_LOG1:
  case PL_COLOR_TRC_S_LOG2:
  default:
    return 0.5;
  }
}
