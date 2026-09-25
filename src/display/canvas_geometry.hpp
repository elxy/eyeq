#pragma once

#include <cstdint>

namespace EYEQ {

/*
 * Small, window-independent geometry for the fixed comparison canvas and its
 * desktop preview rectangle (V2.1 section 4.4 / 5.2).
 *
 * The comparison core composes one image at a fixed canvas size that does not
 * depend on the window size. The desktop presenter scales that canvas into a
 * letterboxed preview rectangle; every pointer gesture is converted from window
 * space into canvas space through this mapping. Keeping the arithmetic here (no
 * SDL, no GPU) is what makes the coordinate system testable on the CPU.
 *
 * Conventions:
 *   - Window coordinates are *device* pixels (the same space as the swapchain and
 *     `SDL_GetWindowSizeInPixels`). SDL delivers mouse coordinates in logical
 *     pixels, so callers multiply by @p pixel_density before using them here.
 *   - Canvas coordinates are canvas pixels, origin at the canvas top-left.
 *   - The preview rectangle preserves the canvas aspect ratio and is centered.
 */
struct RectF {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;

  bool IsEmpty() const { return w <= 0.0f || h <= 0.0f; }
  bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct CanvasLayout {
  int canvas_w = 0;
  int canvas_h = 0;
  int window_w = 0; // device pixels
  int window_h = 0; // device pixels
  float pixel_density = 1.0f;
  RectF preview; // device window pixels
};

// Letterboxed ("contain") fit of the fixed canvas into the window. A degenerate
// canvas (<= 0) yields an empty preview rectangle; callers must not then divide
// by the scale.
CanvasLayout ComputeCanvasLayout(int canvas_w, int canvas_h, int window_w, int window_h, float pixel_density = 1.0f);

// Device window pixel -> canvas pixel. Returns false when the point is outside
// the preview rectangle (the canvas value is still written for diagnostic use).
bool WindowToCanvas(const CanvasLayout &layout, float window_x, float window_y, float *canvas_x, float *canvas_y);

// Canvas pixel -> device window pixel. Throws std::logic_error for a degenerate
// layout (a programming error).
void CanvasToWindow(const CanvasLayout &layout, float canvas_x, float canvas_y, float *window_x, float *window_y);

// Clamps a device window point into the preview rectangle before converting it.
// Used while a drag gesture that started inside the preview leaves the window.
void ClampWindowToCanvas(const CanvasLayout &layout, float window_x, float window_y, float *canvas_x, float *canvas_y);

// The preview rectangle in normalized [0,1] target coordinates (for the encoder
// and the color-managed present hop).
RectF NormalizedPreview(const CanvasLayout &layout);

} // namespace EYEQ
