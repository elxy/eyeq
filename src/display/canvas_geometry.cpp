#include "canvas_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EYEQ {

CanvasLayout ComputeCanvasLayout(int canvas_w, int canvas_h, int window_w, int window_h, float pixel_density) {
  CanvasLayout layout;
  layout.canvas_w = canvas_w;
  layout.canvas_h = canvas_h;
  layout.window_w = window_w;
  layout.window_h = window_h;
  layout.pixel_density = (std::isfinite(pixel_density) && pixel_density > 0.0f) ? pixel_density : 1.0f;

  if (canvas_w <= 0 || canvas_h <= 0 || window_w <= 0 || window_h <= 0) {
    layout.preview = {};
    return layout;
  }

  const float canvas_aspect = static_cast<float>(canvas_w) / static_cast<float>(canvas_h);
  const float window_aspect = static_cast<float>(window_w) / static_cast<float>(window_h);
  float preview_w;
  float preview_h;
  if (window_aspect > canvas_aspect) {
    // Window is wider than the canvas: full height, pillarboxed.
    preview_h = static_cast<float>(window_h);
    preview_w = preview_h * canvas_aspect;
  } else {
    // Window is taller than the canvas: full width, letterboxed.
    preview_w = static_cast<float>(window_w);
    preview_h = preview_w / canvas_aspect;
  }
  layout.preview.x = (static_cast<float>(window_w) - preview_w) * 0.5f;
  layout.preview.y = (static_cast<float>(window_h) - preview_h) * 0.5f;
  layout.preview.w = preview_w;
  layout.preview.h = preview_h;
  return layout;
}

bool WindowToCanvas(const CanvasLayout &layout, float window_x, float window_y, float *canvas_x, float *canvas_y) {
  if (layout.preview.IsEmpty()) {
    if (canvas_x) {
      *canvas_x = 0.0f;
    }
    if (canvas_y) {
      *canvas_y = 0.0f;
    }
    return false;
  }
  const float sx = layout.preview.w / static_cast<float>(layout.canvas_w);
  const float sy = layout.preview.h / static_cast<float>(layout.canvas_h);
  if (canvas_x) {
    *canvas_x = (window_x - layout.preview.x) / sx;
  }
  if (canvas_y) {
    *canvas_y = (window_y - layout.preview.y) / sy;
  }
  return layout.preview.Contains(window_x, window_y);
}

void CanvasToWindow(const CanvasLayout &layout, float canvas_x, float canvas_y, float *window_x, float *window_y) {
  if (layout.preview.IsEmpty()) {
    throw std::logic_error("CanvasToWindow called with an empty preview rectangle");
  }
  const float sx = layout.preview.w / static_cast<float>(layout.canvas_w);
  const float sy = layout.preview.h / static_cast<float>(layout.canvas_h);
  if (window_x) {
    *window_x = layout.preview.x + canvas_x * sx;
  }
  if (window_y) {
    *window_y = layout.preview.y + canvas_y * sy;
  }
}

void ClampWindowToCanvas(const CanvasLayout &layout, float window_x, float window_y, float *canvas_x, float *canvas_y) {
  if (layout.preview.IsEmpty()) {
    if (canvas_x) {
      *canvas_x = 0.0f;
    }
    if (canvas_y) {
      *canvas_y = 0.0f;
    }
    return;
  }
  const float clamped_x = std::min(std::max(window_x, layout.preview.x), layout.preview.x + layout.preview.w);
  const float clamped_y = std::min(std::max(window_y, layout.preview.y), layout.preview.y + layout.preview.h);
  WindowToCanvas(layout, clamped_x, clamped_y, canvas_x, canvas_y);
}

RectF NormalizedPreview(const CanvasLayout &layout) {
  if (layout.window_w <= 0 || layout.window_h <= 0 || layout.preview.IsEmpty()) {
    return RectF{};
  }
  return RectF{
      layout.preview.x / static_cast<float>(layout.window_w), layout.preview.y / static_cast<float>(layout.window_h),
      layout.preview.w / static_cast<float>(layout.window_w), layout.preview.h / static_cast<float>(layout.window_h)};
}

} // namespace EYEQ
