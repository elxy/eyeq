#pragma once

#include <cstdint>

#include <libplacebo/gpu.h>

#include "mapping_target.hpp"

namespace EYEQ {

/*
 * Identity of a composed scene result. These are deliberately independent of the
 * desktop window state: a window resize or an OSD toggle must not look like a new
 * scene.
 *   - scene_revision: bumped when a rendering-relevant input changes (a new frame
 *     set is fed, the comparison mode/geometry/id sets change, a precision
 *     reconfiguration happens). Pure UI events do not bump it.
 *   - render_id: bumped on every successful composition, even if scene_revision
 *     did not change (e.g. a forced refresh).
 *   - source_serials: a stable summary of the per-video frame serials that were
 *     composed, so a saved image can be traced back to its sources.
 */
struct RenderIdentity {
  uint64_t scene_revision = 0;
  uint64_t render_id = 0;
  uint64_t source_serials = 0;
};

/*
 * Lightweight description of one composed common result (V2.1 section 2.1:
 * "public result description"). @p texture is owned by the coordinator's renderer
 * and is a borrow with *validity* semantics, not a reference-counted handle: it
 * stays valid until the next composition or FeedFrames call on the same
 * coordinator.
 *
 * There is no acquire/release protocol. A consumer that reads the texture
 * synchronously (the monitor output encodes it into its own target inside
 * Submit) is safe against a later recomposition: on one pl_gpu there is one
 * queue, submission order is execution order, and the read was submitted first.
 * The desktop present path has always relied on the same property.
 *
 * It is intentionally not a bare pl_tex and does not promise RGBA32F: the storage
 * precision is carried explicitly.
 */
struct RenderedFrame {
  pl_tex texture = nullptr;
  int width = 0;
  int height = 0;
  struct pl_color_space color{};
  WorkPrecision precision = WorkPrecision::F16;
  // Mapping-target luminance levels of the display-linear data, carried so a
  // consumer (monitor encode) uses the same SDR white/black as the composer.
  float sdr_white_nits = 203.0f;
  float black_nits = 0.0f;
  RenderIdentity identity{};

  bool IsValid() const { return texture != nullptr && width > 0 && height > 0; }
};

} // namespace EYEQ
