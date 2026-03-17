#pragma once

#include <string>
#include <vector>

#include <SDL3_ttf/SDL_ttf.h>
#include <libplacebo/gpu.h>

#include "render.hpp"

namespace EYEQ {

struct OsdEntry {
  std::string text;
  int x, y; // Target position (pixel coordinates)
};

class OsdManager {
public:
  OsdManager(pl_gpu gpu, float pixel_density);
  ~OsdManager();

  /**
   * @brief Compose complete OSD overlay (same size as window, transparent background)
   *        Includes text info and progress bar
   *
   * @param window_w Window width (pixels)
   * @param window_h Window height (pixels)
   * @param entries OSD text entries (position and content)
   * @param progress Playback progress [0, 1]
   * @return OSD overlay texture; returns nullptr if no content
   */
  pl_tex ComposeOverlay(int window_w, int window_h, const std::vector<OsdEntry> &entries, float progress);

  int GetTextHeight() const;
  int GetTextWidth(const std::string &text) const;

private:
  pl_gpu gpu_;
  float pixel_density_;
  TTF_Font *font_ = nullptr;

  pl_tex overlay_tex_ = nullptr;
  int last_overlay_w_ = 0;
  int last_overlay_h_ = 0;

  // Cache detection
  std::string last_composed_;
  float last_progress_ = -1.0f;

  void InitFont();
};

}; // namespace EYEQ
