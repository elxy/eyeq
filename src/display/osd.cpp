#include "osd.hpp"

#include <cstring>
#include <stdexcept>

#include <SDL3/SDL_surface.h>

#include "log.hpp"
#include "render.hpp"

using namespace EYEQ;

std::string OsdInfo::Format() const {
  int hours = static_cast<int>(current_time_s) / 3600;
  int minutes = (static_cast<int>(current_time_s) % 3600) / 60;
  int seconds = static_cast<int>(current_time_s) % 60;
  char time_buf[16];
  std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", hours, minutes, seconds);

  int display_serial = frame_serial + frame_offset;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "#%d: %s %c %s [%d/%d]", video_id, filename.c_str(), pict_type, time_buf,
                display_serial, total_frames);
  return std::string(buf);
}

// System font search paths (prefer CJK fonts, with English fallback)
static const char *font_search_paths[] = {
    // macOS - CJK fonts
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    // macOS - English monospace fonts (fallback)
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/Courier.ttc",
    // Windows - CJK fonts
    "C:/Windows/Fonts/msyh.ttc",   // Microsoft YaHei
    "C:/Windows/Fonts/msyhbd.ttc", // Microsoft YaHei Bold
    "C:/Windows/Fonts/simhei.ttf", // SimHei
    "C:/Windows/Fonts/simsun.ttc", // SimSun
    // Windows - English monospace fonts (fallback)
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/cour.ttf",
    // Linux - CJK fonts
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    // Linux - English monospace fonts (fallback)
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    nullptr,
};

OsdManager::OsdManager(pl_gpu gpu, float pixel_density) : gpu_(gpu), pixel_density_(pixel_density) { InitFont(); }

OsdManager::~OsdManager() {
  if (overlay_tex_) {
    pl_tex_destroy(gpu_, &overlay_tex_);
  }
  if (font_) {
    TTF_CloseFont(font_);
  }
  TTF_Quit();
}

void OsdManager::InitFont() {
  if (!TTF_Init()) {
    throw std::runtime_error("Failed to initialize SDL_ttf");
  }

  float font_size = 24.0f * pixel_density_;

  for (int i = 0; font_search_paths[i] != nullptr; i++) {
    font_ = TTF_OpenFont(font_search_paths[i], font_size);
    if (font_) {
      Logger->debug("OSD font loaded: {} (size {:.1f}pt)", font_search_paths[i], font_size);
      return;
    }
  }

  throw std::runtime_error("Failed to find a suitable monospace font for OSD");
}

int OsdManager::GetTextHeight() const {
  if (!font_)
    return 0;
  return TTF_GetFontHeight(font_);
}

int OsdManager::GetTextWidth(const std::string &text) const {
  if (!font_ || text.empty())
    return 0;
  int w = 0, h = 0;
  TTF_GetStringSize(font_, text.c_str(), 0, &w, &h);
  return w;
}

pl_tex OsdManager::ComposeOverlay(int window_w, int window_h, const std::vector<OsdEntry> &entries, float progress) {
  // Build cache key: all entries' text + position + progress
  std::string cache_key;
  for (const auto &entry : entries) {
    cache_key += entry.text;
    cache_key += std::to_string(entry.x) + "," + std::to_string(entry.y) + ";";
  }

  // Check cache: reuse when content and dimensions haven't changed
  if (cache_key == last_composed_ && progress == last_progress_ && window_w == last_overlay_w_ &&
      window_h == last_overlay_h_ && overlay_tex_) {
    return overlay_tex_;
  }

  last_composed_ = cache_key;
  last_progress_ = progress;
  last_overlay_w_ = window_w;
  last_overlay_h_ = window_h;

  // Create a transparent RGBA surface
  SDL_Surface *overlay = SDL_CreateSurface(window_w, window_h, SDL_PIXELFORMAT_RGBA32);
  if (!overlay) {
    Logger->error("Failed to create OSD overlay surface: {}", SDL_GetError());
    return nullptr;
  }
  // Zero out (fully transparent)
  SDL_memset(overlay->pixels, 0, overlay->pitch * overlay->h);

  // Semi-transparent black background to improve text readability
  SDL_Color text_fg = {220, 220, 220, 255};
  SDL_Color bg_color = {0, 0, 0, 160};

  // Render each OSD entry
  for (const auto &entry : entries) {
    if (entry.text.empty())
      continue;

    SDL_Surface *text_surface = TTF_RenderText_Blended(font_, entry.text.c_str(), 0, text_fg);
    if (!text_surface) {
      Logger->warn("Failed to render OSD text '{}': {}", entry.text, SDL_GetError());
      continue;
    }

    // Ensure consistent format
    SDL_Surface *converted = SDL_ConvertSurface(text_surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(text_surface);
    if (!converted)
      continue;

    int padding = static_cast<int>(2 * pixel_density_);

    // Draw semi-transparent background rectangle
    SDL_Rect bg_rect = {entry.x - padding, entry.y - padding, converted->w + padding * 2, converted->h + padding * 2};
    // Clip to overlay bounds
    if (bg_rect.x < 0)
      bg_rect.x = 0;
    if (bg_rect.y < 0)
      bg_rect.y = 0;
    if (bg_rect.x + bg_rect.w > window_w)
      bg_rect.w = window_w - bg_rect.x;
    if (bg_rect.y + bg_rect.h > window_h)
      bg_rect.h = window_h - bg_rect.y;

    // Manually fill background pixels
    uint8_t *pixels = static_cast<uint8_t *>(overlay->pixels);
    for (int row = bg_rect.y; row < bg_rect.y + bg_rect.h && row < window_h; row++) {
      for (int col = bg_rect.x; col < bg_rect.x + bg_rect.w && col < window_w; col++) {
        int offset = row * overlay->pitch + col * 4;
        pixels[offset + 0] = bg_color.r;
        pixels[offset + 1] = bg_color.g;
        pixels[offset + 2] = bg_color.b;
        pixels[offset + 3] = bg_color.a;
      }
    }

    // Blit text onto overlay
    SDL_Rect dst_rect = {entry.x, entry.y, converted->w, converted->h};
    SDL_BlitSurface(converted, nullptr, overlay, &dst_rect);
    SDL_DestroySurface(converted);
  }

  // Draw progress bar (bottom)
  int bar_height = static_cast<int>(kProgressBarHeight * pixel_density_);
  int bar_y = window_h - bar_height;
  int progress_x = static_cast<int>(progress * window_w);
  uint8_t *pixels = static_cast<uint8_t *>(overlay->pixels);
  for (int row = bar_y; row < window_h; row++) {
    for (int col = 0; col < window_w; col++) {
      int offset = row * overlay->pitch + col * 4;
      if (col <= progress_x) {
        // Played portion: bright color
        pixels[offset + 0] = 200; // R
        pixels[offset + 1] = 200; // G
        pixels[offset + 2] = 200; // B
        pixels[offset + 3] = 200; // A
      } else {
        // Unplayed portion: dark color
        pixels[offset + 0] = 60;  // R
        pixels[offset + 1] = 60;  // G
        pixels[offset + 2] = 60;  // B
        pixels[offset + 3] = 120; // A
      }
    }
  }

  // Upload to GPU texture
  struct pl_tex_params tex_params = {
      .w = window_w,
      .h = window_h,
      .format = pl_find_named_fmt(gpu_, "rgba8"),
      .sampleable = true,
      .host_writable = true,
  };
  if (!pl_tex_recreate(gpu_, &overlay_tex_, &tex_params)) {
    Logger->error("Failed to create OSD overlay texture");
    SDL_DestroySurface(overlay);
    return nullptr;
  }

  struct pl_tex_transfer_params upload = {
      .tex = overlay_tex_,
      .row_pitch = static_cast<size_t>(overlay->pitch),
      .ptr = overlay->pixels,
  };
  if (!pl_tex_upload(gpu_, &upload)) {
    Logger->error("Failed to upload OSD overlay texture");
  }

  SDL_DestroySurface(overlay);
  return overlay_tex_;
}
