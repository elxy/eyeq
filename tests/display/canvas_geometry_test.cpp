// Pure-CPU tests for the fixed-canvas / preview-rectangle geometry. No GPU, no
// window, no SDL: this is exactly the coordinate transform the desktop event loop
// uses for mouse x/y, drag deltas, zoom centers, the Slide split and HiDPI.

#include <cmath>
#include <cstdio>
#include <string>

#include "display/canvas_geometry.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (condition) {
    std::fprintf(stdout, "  ok   %s\n", what.c_str());
  } else {
    std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

bool Near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

void TestLetterboxAndPillarbox() {
  {
    // Window wider than the canvas: pillarbox (full height, bars left/right).
    const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 1280, 600);
    Check(Near(layout.preview.h, 600.0f), "pillarbox uses the full window height");
    Check(Near(layout.preview.w, 600.0f * 1920.0f / 1080.0f), "pillarbox width keeps the canvas aspect");
    Check(Near(layout.preview.x, (1280.0f - layout.preview.w) * 0.5f), "pillarbox is centered horizontally");
    Check(Near(layout.preview.y, 0.0f), "pillarbox has no vertical bar");
  }
  {
    // Window taller than the canvas: letterbox (full width, bars top/bottom).
    const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 1000, 800);
    Check(Near(layout.preview.w, 1000.0f), "letterbox uses the full window width");
    Check(Near(layout.preview.h, 1000.0f * 1080.0f / 1920.0f), "letterbox height keeps the canvas aspect");
    Check(Near(layout.preview.y, (800.0f - layout.preview.h) * 0.5f), "letterbox is centered vertically");
    Check(Near(layout.preview.x, 0.0f), "letterbox has no horizontal bar");
  }
  {
    // Matching aspect: the preview fills the window exactly.
    const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 960, 540);
    Check(Near(layout.preview.x, 0.0f) && Near(layout.preview.y, 0.0f) && Near(layout.preview.w, 960.0f) &&
              Near(layout.preview.h, 540.0f),
          "matching aspect fills the window");
  }
}

void TestDegenerate() {
  const EYEQ::CanvasLayout no_canvas = EYEQ::ComputeCanvasLayout(0, 1080, 1280, 720);
  Check(no_canvas.preview.IsEmpty(), "a zero canvas gives an empty preview");
  const EYEQ::CanvasLayout no_window = EYEQ::ComputeCanvasLayout(1920, 1080, 0, 720);
  Check(no_window.preview.IsEmpty(), "a zero window gives an empty preview");
  bool threw = false;
  try {
    float x = 0.0f;
    float y = 0.0f;
    EYEQ::CanvasToWindow(no_canvas, 0.0f, 0.0f, &x, &y);
  } catch (const std::logic_error &) {
    threw = true;
  }
  Check(threw, "CanvasToWindow fails fast on a degenerate layout");
}

void TestRoundTripAndOutside() {
  const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 1000, 800);
  const struct {
    float cx, cy;
  } points[] = {{0.0f, 0.0f}, {960.0f, 540.0f}, {1919.0f, 1079.0f}, {100.0f, 700.0f}};
  for (const auto &p : points) {
    float wx = 0.0f;
    float wy = 0.0f;
    EYEQ::CanvasToWindow(layout, p.cx, p.cy, &wx, &wy);
    float rx = 0.0f;
    float ry = 0.0f;
    const bool inside = EYEQ::WindowToCanvas(layout, wx, wy, &rx, &ry);
    Check(inside && Near(rx, p.cx) && Near(ry, p.cy), "canvas->window->canvas round-trips");
  }
  // A point in the letterbox bar is outside the canvas.
  float cx = 0.0f;
  float cy = 0.0f;
  const bool inside = EYEQ::WindowToCanvas(layout, 500.0f, 5.0f, &cx, &cy);
  Check(!inside, "a point in the letterbox bar is reported outside the preview");
  // The coordinates are still computed for diagnostics (negative canvas y).
  Check(cy < 0.0f, "an outside point still yields a diagnostic canvas coordinate");
}

void TestClampWhileDragging() {
  const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 1000, 800);
  float cx = 0.0f;
  float cy = 0.0f;
  EYEQ::ClampWindowToCanvas(layout, -500.0f, -500.0f, &cx, &cy);
  Check(Near(cx, 0.0f) && Near(cy, 0.0f), "a drag clamped from the top-left lands on the canvas origin");
  EYEQ::ClampWindowToCanvas(layout, 5000.0f, 5000.0f, &cx, &cy);
  Check(Near(cx, 1920.0f) && Near(cy, 1080.0f), "a drag clamped from the bottom-right lands on the canvas corner");
}

void TestNormalizedPreviewAndHidpi() {
  const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 1000, 800, 2.0f);
  const EYEQ::RectF np = EYEQ::NormalizedPreview(layout);
  // The preview is (1000 x 562.5) at (0, 118.75) in a 1000x800 window.
  Check(Near(np.x, 0.0f) && Near(np.y, 118.75f / 800.0f) && Near(np.w, 1.0f) && Near(np.h, 562.5f / 800.0f),
        "normalized preview is relative to the device-pixel window");
  // HiDPI density does not change the normalized rectangle (it is a ratio).
  const EYEQ::CanvasLayout layout_hidpi = EYEQ::ComputeCanvasLayout(1920, 1080, 2000, 1600, 2.0f);
  const EYEQ::RectF np_hidpi = EYEQ::NormalizedPreview(layout_hidpi);
  Check(Near(np.x, np_hidpi.x) && Near(np.y, np_hidpi.y) && Near(np.w, np_hidpi.w) && Near(np.h, np_hidpi.h),
        "the normalized preview depends only on the aspect ratio, not the pixel density");
  // Device-pixel input: a physical point maps through the same scale.
  float cx = 0.0f;
  float cy = 0.0f;
  EYEQ::WindowToCanvas(layout, 0.0f, 118.75f, &cx, &cy);
  Check(Near(cx, 0.0f) && Near(cy, 0.0f), "the top-left of the preview maps to the canvas origin");
  EYEQ::WindowToCanvas(layout, 1000.0f, 118.75f + 562.5f, &cx, &cy);
  Check(Near(cx, 1920.0f) && Near(cy, 1080.0f), "the bottom-right of the preview maps to the canvas corner");
}

void TestSlideSplitMapping() {
  // The Slide split position is a canvas fraction derived from the mouse x.
  const EYEQ::CanvasLayout layout = EYEQ::ComputeCanvasLayout(1920, 1080, 1000, 800);
  float cx = 0.0f;
  float cy = 0.0f;
  EYEQ::WindowToCanvas(layout, 500.0f, 400.0f, &cx, &cy);
  const float split = cx / 1920.0f;
  Check(Near(split, 0.5f, 1e-3f), "the window center maps to a 0.5 slide split");
}

} // namespace

int main() {
  TestLetterboxAndPillarbox();
  TestDegenerate();
  TestRoundTripAndOutside();
  TestClampWhileDragging();
  TestNormalizedPreviewAndHidpi();
  TestSlideSplitMapping();
  std::fprintf(stdout, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
