#pragma once

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>

namespace EYEQ {

enum class ScaleMethod {
  Nearest,
  Bilinear,
  Bicubic,
  Lanczos,
  EWALanczos,
  EWALanczosSharp,
  Mitchell,
  CatmullRom,
  Spline36,
  Spline64,
};

static constexpr float kLineWidth = 2;
static constexpr float kProgressBarHeight = 4; // Progress bar height (logical pixels)
static constexpr float kOsdMargin = 12;        // OSD margin (logical pixels)

enum class DisplayMode {
  Fill,  // Fill mode: switch video by key press
  Slide, // Slide mode: side-by-side swipe of 2 videos
  Grid,  // Grid mode
};

struct OsdInfo {
  std::string filename; // basename
  int video_id;         // Video index
  char pict_type;       // 'I', 'P', 'B'
  float current_time_s; // Current time (seconds)
  int frame_serial;     // Current frame serial
  int total_frames;     // Total frames
  int frame_offset = 0; // Per-video frame offset from single-video operations

  std::string Format() const;
};

struct DisplayState {
  bool show_ref;                // Whether to show reference video
  std::optional<int> ref_id;    // Reference video ID
  std::vector<int> ids;         // Video ID list, excluding reference video
  std::optional<float> amplify; // Amplification factor for differences from reference video
  bool window_attached;         // Whether to scale with window
  float scale;                  // Shared zoom scale
  float offset_x, offset_y;     // Shared pan offset
  struct {
    int id; // Displayed video ID
  } fill;
  struct {
    float split_pos; // Slider position (0.0-1.0)
    int left_id;     // Left video ID
    int right_id;    // Right video ID
  } slide;
  struct {
    int cols, rows; // Grid layout
  } grid;
  bool show_osd = false;            // OSD show/hide toggled by I key
  std::map<int, OsdInfo> osd_infos; // OSD info for each video
  float playback_progress = 0.0f;   // Playback progress [0, 1]
  float total_duration_s = 0.0f;    // Main video total duration (seconds)
  float mouse_time_s = -1.0f;       // Time at mouse X position (seconds), <0 means invalid
  float playback_fps = 0.0f;        // Current playback FPS
  float main_fps = 0.0f;            // Main video FPS
};

static inline float align_to_pixel_edge(float value, float scale) {
  return std::roundf(std::roundf(value / (scale / 2)) / 2) * (scale);
}

float reference_white(enum pl_color_transfer trc);

void calculate_best_window_size(const int grid_w, const int grid_h, int &win_w, int &win_h, const int display_w,
                                const int display_h, const float pixel_density, const int video_w, const int video_h);
void calculate_best_grid_size(int &grid_w, int &grid_h, const int win_w, const int win_h, const float pixel_density,
                              const int video_w, const int video_h, const int nb_videos);
}; // namespace EYEQ
