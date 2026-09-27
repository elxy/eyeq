#include "output/frame_packer.hpp"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
#include "frame_packer_test_hooks.hpp"
#endif

#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
namespace EYEQ {
namespace frame_packer_test {

Hooks &MutableHooks() {
  static Hooks hooks;
  return hooks;
}

} // namespace frame_packer_test
} // namespace EYEQ
#endif

namespace EYEQ {

namespace {

constexpr int kSlotCount = 2;
constexpr int kDrainPollIntervalMs = 1;

using Clock = std::chrono::steady_clock;

// A tiny indirection over the few libplacebo calls the fault-injection test needs
// to override. In normal builds the hooks are compiled out and these are direct
// calls; there is no product switch or runtime configuration.
bool DownloadTexture(pl_gpu gpu, const struct pl_tex_transfer_params *params) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().tex_download != nullptr) {
    return frame_packer_test::MutableHooks().tex_download(gpu, params);
  }
#endif
  return pl_tex_download(gpu, params);
}

bool PollBuffer(pl_gpu gpu, pl_buf buf, uint64_t timeout) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().buf_poll != nullptr) {
    return frame_packer_test::MutableHooks().buf_poll(gpu, buf, timeout);
  }
#endif
  return pl_buf_poll(gpu, buf, timeout);
}

bool GpuIsFailed(pl_gpu gpu) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().gpu_is_failed != nullptr) {
    return frame_packer_test::MutableHooks().gpu_is_failed(gpu);
  }
#endif
  return pl_gpu_is_failed(gpu);
}

void FlushGpu(pl_gpu gpu) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().gpu_flush != nullptr) {
    frame_packer_test::MutableHooks().gpu_flush(gpu);
    return;
  }
#endif
  pl_gpu_flush(gpu);
}

bool PollTexture(pl_gpu gpu, pl_tex tex, uint64_t timeout) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().tex_poll != nullptr) {
    return frame_packer_test::MutableHooks().tex_poll(gpu, tex, timeout);
  }
#endif
  return pl_tex_poll(gpu, tex, timeout);
}

pl_tex CreateTexture(pl_gpu gpu, const pl_tex_params *params) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().tex_create != nullptr) {
    return frame_packer_test::MutableHooks().tex_create(gpu, params);
  }
#endif
  return pl_tex_create(gpu, params);
}

pl_buf CreateBuffer(pl_gpu gpu, const pl_buf_params *params) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().buf_create != nullptr) {
    return frame_packer_test::MutableHooks().buf_create(gpu, params);
  }
#endif
  return pl_buf_create(gpu, params);
}

void DestroyTexture(pl_gpu gpu, pl_tex *tex) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().tex_destroy != nullptr) {
    frame_packer_test::MutableHooks().tex_destroy(gpu, tex);
    return;
  }
#endif
  pl_tex_destroy(gpu, tex);
}

void DestroyBuffer(pl_gpu gpu, pl_buf *buf) {
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().buf_destroy != nullptr) {
    frame_packer_test::MutableHooks().buf_destroy(gpu, buf);
    return;
  }
#endif
  pl_buf_destroy(gpu, buf);
}

// Emits exactly one diagnostic line for a failure transition. In production this
// goes to stderr; the fault-injection test can install a sink. There is no public
// error channel by design.
void EmitDiagnostic(const char *stage, uint64_t frame_id, const char *reason) {
  char buffer[384];
  std::snprintf(buffer, sizeof(buffer), "FramePacker: stage=%s frame_id=%llu: %s", stage,
                static_cast<unsigned long long>(frame_id), reason != nullptr ? reason : "unknown failure");
#ifdef EYEQ_FRAME_PACKER_TEST_HOOKS
  if (frame_packer_test::MutableHooks().diagnostic != nullptr) {
    frame_packer_test::MutableHooks().diagnostic(buffer);
    return;
  }
#endif
  std::fprintf(stderr, "%s\n", buffer);
  std::fflush(stderr);
}

void ValidateConfig(pl_gpu gpu, const PackConfig &config) {
  if (gpu == nullptr) {
    throw std::invalid_argument("FramePacker: gpu is null");
  }
  const BufferLayout &layout = config.layout;
  if (layout.width <= 0 || layout.height <= 0) {
    throw std::invalid_argument("FramePacker: layout dimensions must be positive");
  }
  const int minimum = MinimalRowBytes(layout.pixel_format, layout.width);
  if (minimum <= 0) {
    throw std::invalid_argument("FramePacker: unknown pixel format or row stride out of range");
  }
  if (layout.row_bytes < minimum) {
    throw std::invalid_argument("FramePacker: row_bytes is below the format minimum");
  }
  if (layout.row_bytes % 4 != 0) {
    throw std::invalid_argument("FramePacker: row_bytes must be 32-bit word aligned");
  }

  const std::size_t row_bytes = static_cast<std::size_t>(layout.row_bytes);
  const std::size_t height = static_cast<std::size_t>(layout.height);
  if (row_bytes > std::numeric_limits<std::size_t>::max() / height) {
    throw std::invalid_argument("FramePacker: row_bytes * height overflows size_t");
  }
  if (layout.image_bytes != row_bytes * height) {
    throw std::invalid_argument("FramePacker: image_bytes does not equal row_bytes * height");
  }

  if (!gpu->limits.buf_transfer) {
    throw std::runtime_error("FramePacker: GPU does not support buffer transfers (pl_tex_transfer_params.buf)");
  }
  if (layout.image_bytes > gpu->limits.max_buf_size) {
    throw std::runtime_error("FramePacker: image_bytes exceeds the GPU max buffer size");
  }
  if (layout.image_bytes > gpu->limits.max_mapped_size) {
    throw std::runtime_error("FramePacker: image_bytes exceeds the GPU max mapped buffer size");
  }
  if (row_bytes / 4 > gpu->limits.max_tex_2d_dim || height > gpu->limits.max_tex_2d_dim) {
    throw std::runtime_error("FramePacker: staging carrier dimensions exceed the GPU max texture size");
  }

  const ColorEncoding &output = config.output;
  const bool is_sdr = output.primaries == ColorPrimaries::BT709 && output.transfer == ColorTransfer::SDR_BT1886;
  const bool is_hdr = output.primaries == ColorPrimaries::BT2020 && output.transfer == ColorTransfer::PQ;
  if (!is_sdr && !is_hdr) {
    throw std::invalid_argument("FramePacker: first version allows BT709/SDR_BT1886 or BT2020/PQ");
  }
  if (output.range != ColorRange::Video) {
    throw std::invalid_argument("FramePacker: first version requires ColorRange::Video");
  }
  if (output.chroma_loc != ChromaLocation::Left) {
    throw std::invalid_argument("FramePacker: first version requires ChromaLocation::Left");
  }
}

// Returns a byte-carrier format whose host layout is exactly one word (four 8-bit
// components in RGBA order) per texel. Any incompatible format is rejected instead
// of silently falling back to a CPU conversion.
pl_fmt FindCarrierFormat(pl_gpu gpu) {
  const auto caps = static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
  pl_fmt fmt = pl_find_named_fmt(gpu, "rgba8");
  if (fmt == nullptr) {
    fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, caps);
  }
  if (fmt == nullptr) {
    throw std::runtime_error("FramePacker: no RGBA8 UNORM renderable/host-readable format on this GPU");
  }
  if (fmt->type != PL_FMT_UNORM || fmt->opaque || fmt->num_components != 4) {
    throw std::runtime_error("FramePacker: carrier format is not a non-opaque 4-component UNORM format");
  }
  if (fmt->texel_size != 4) {
    throw std::runtime_error("FramePacker: carrier texel size is not 4 bytes");
  }
  if ((fmt->caps & PL_FMT_CAP_RENDERABLE) == 0 || (fmt->caps & PL_FMT_CAP_HOST_READABLE) == 0) {
    throw std::runtime_error("FramePacker: carrier format is not renderable and host-readable");
  }
  for (int i = 0; i < 4; ++i) {
    if (fmt->host_bits[i] != 8 || fmt->sample_order[i] != i) {
      throw std::runtime_error("FramePacker: carrier host layout is not RGBA8 in memory order");
    }
  }
  return fmt;
}

std::string FormatDouble(double value) {
  std::ostringstream out;
  out.precision(9);
  out << value;
  return out.str();
}

// Shader helpers are emitted only for v210 and carry the fixed image width plus the
// luma matrix for the configured primaries. The source (nonlinear, full-range RGB)
// is never transfer-decoded here.
std::string BuildShaderHeader(const PackConfig &config) {
  std::ostringstream out;
  out << "const int IMG_W = " << config.layout.width << ";\n";
  if (config.layout.pixel_format != PixelFormat::V210) {
    return out.str();
  }

  double kr = 0.0;
  double kb = 0.0;
  switch (config.output.primaries) {
  case ColorPrimaries::BT709:
    kr = 0.2126;
    kb = 0.0722;
    break;
  case ColorPrimaries::BT2020:
    kr = 0.2627;
    kb = 0.0593;
    break;
  default:
    throw std::invalid_argument("FramePacker: unsupported primaries for the luma matrix");
  }
  out << "const float KR = " << FormatDouble(kr) << ";\n";
  out << "const float KB = " << FormatDouble(kb) << ";\n";
  out << "const float KG = 1.0 - KR - KB;\n";
  out << R"(
vec3 FetchRGB(int col, int row) {
  int c = clamp(col, 0, IMG_W - 1);
  return clamp(texelFetch(src_tex, ivec2(c, row), 0).rgb, 0.0, 1.0);
}
uint LumaCode(int col, int row) {
  if (col < 0 || col >= IMG_W) return 0u;
  vec3 c = FetchRGB(col, row);
  float y = KR * c.r + KG * c.g + KB * c.b;
  return uint(clamp(floor(64.0 + 876.0 * y + 0.5), 64.0, 940.0));
}
float ChromaValue(int col, int row, bool is_cb) {
  vec3 c = FetchRGB(col, row);
  float y = KR * c.r + KG * c.g + KB * c.b;
  return is_cb ? (c.b - y) / (2.0 * (1.0 - KB)) : (c.r - y) / (2.0 * (1.0 - KR));
}
uint ChromaCode(int even_col, int row, bool is_cb) {
  if (even_col < 0 || even_col >= IMG_W) return 0u;
  float v = 0.25 * (ChromaValue(even_col - 1, row, is_cb) +
                    2.0 * ChromaValue(even_col, row, is_cb) +
                    ChromaValue(even_col + 1, row, is_cb));
  return uint(clamp(floor(512.0 + 896.0 * v + 0.5), 64.0, 960.0));
}
)";
  return out.str();
}

// Body of the per-texel packing fragment shader. `color` is the PL_SHADER_SIG_COLOR
// output; each carrier texel stores exactly one packed 32-bit word. Words outside
// the real pixel data (r210 padding columns, v210 padding words and absent fields)
// are written as zero.
std::string BuildShaderBody(PixelFormat format) {
  if (format == PixelFormat::R210) {
    return R"(
  int x = int(gl_FragCoord.x);
  int row = int(gl_FragCoord.y);
  if (x >= IMG_W) {
    return vec4(0.0);
  }
  vec3 c = clamp(texelFetch(src_tex, ivec2(x, row), 0).rgb, 0.0, 1.0);
  uvec3 code = uvec3(clamp(floor(vec3(64.0) + vec3(876.0) * c + vec3(0.5)), vec3(64.0), vec3(940.0)));
  uint w = (code.r << 20u) | (code.g << 10u) | code.b;
  color = vec4(uvec4((w >> 24u) & 255u, (w >> 16u) & 255u, (w >> 8u) & 255u, w & 255u)) / 255.0;
)";
  }
  return R"(
  int x = int(gl_FragCoord.x);
  int row = int(gl_FragCoord.y);
  if (x >= ((IMG_W + 5) / 6) * 4) {
    return vec4(0.0);
  }
  int base = (x / 4) * 6;
  int index = x % 4;
  uint w;
  if (index == 0) {
    w = ChromaCode(base + 0, row, true) | (LumaCode(base + 0, row) << 10u) | (ChromaCode(base + 0, row, false) << 20u);
  } else if (index == 1) {
    w = LumaCode(base + 1, row) | (ChromaCode(base + 2, row, true) << 10u) | (LumaCode(base + 2, row) << 20u);
  } else if (index == 2) {
    w = ChromaCode(base + 2, row, false) | (LumaCode(base + 3, row) << 10u) | (ChromaCode(base + 4, row, true) << 20u);
  } else {
    w = LumaCode(base + 4, row) | (ChromaCode(base + 4, row, false) << 10u) | (LumaCode(base + 5, row) << 20u);
  }
  color = vec4(uvec4(w & 255u, (w >> 8u) & 255u, (w >> 16u) & 255u, (w >> 24u) & 255u)) / 255.0;
)";
}

} // namespace

enum class SlotState { Free, Pending, Ready, Failed, Aborted };

// One staging slot: a persistent byte-carrier texture plus a persistent mapped
// host-readable buffer. Both survive the whole packer lifetime; only the state
// changes. `source` keeps the caller's texture alive until the slot is reclaimed.
// The two `*_enqueued` flags record whether a render to the carrier and/or a
// download to the mapped buffer may still be in flight, so reclamation never relies
// on the (possibly unused) mapped buffer to prove the render finished.
struct StagingSlot {
  SlotState state = SlotState::Free;
  bool render_enqueued = false;
  bool download_enqueued = false;
  pl_tex carrier = nullptr;
  pl_buf buffer = nullptr;
  pl_tex source = nullptr;
  uint64_t frame_id = 0;
};

struct FramePackerImpl {
  std::array<StagingSlot, kSlotCount> slots;
  pl_dispatch dispatch = nullptr;
  std::string shader_header;
  std::string shader_body;
  bool shutting_down = false;
};

namespace {

bool OwnsSlot(const FramePackerImpl &impl, const StagingSlot *slot) {
  const auto base = reinterpret_cast<uintptr_t>(impl.slots.data());
  const auto addr = reinterpret_cast<uintptr_t>(slot);
  const auto span = sizeof(StagingSlot) * impl.slots.size();
  return addr >= base && addr < base + span && (addr - base) % sizeof(StagingSlot) == 0;
}

void ResetSlot(StagingSlot &slot) {
  slot.state = SlotState::Free;
  slot.render_enqueued = false;
  slot.download_enqueued = false;
  slot.source = nullptr;
  slot.frame_id = 0;
}

// True only when every GPU activity this submission could have enqueued has
// provably completed. A failed download does not imply the preceding render is done,
// so the carrier (render target) and the caller's source (render input) are polled
// directly. Any poll can discover device loss, so the failure check is repeated after
// them.
bool SlotResourcesIdle(pl_gpu gpu, const StagingSlot &slot) {
  if (GpuIsFailed(gpu)) {
    return false;
  }
  if (slot.render_enqueued) {
    if (PollTexture(gpu, slot.carrier, 0)) {
      return false;
    }
    if (slot.source != nullptr && PollTexture(gpu, slot.source, 0)) {
      return false;
    }
  }
  if (slot.download_enqueued && PollBuffer(gpu, slot.buffer, 0)) {
    return false;
  }
  return !GpuIsFailed(gpu);
}

void DestroyResources(pl_gpu gpu, FramePackerImpl &impl) {
  for (StagingSlot &slot : impl.slots) {
    if (slot.carrier != nullptr) {
      DestroyTexture(gpu, &slot.carrier);
    }
    if (slot.buffer != nullptr) {
      DestroyBuffer(gpu, &slot.buffer);
    }
    ResetSlot(slot);
  }
  if (impl.dispatch != nullptr) {
    pl_dispatch_destroy(&impl.dispatch);
  }
}

// Rejects any input that is not a sampled 2D float texture on the same GPU with the
// exact configured dimensions. No scaling, flipping or UNORM substitution happens.
bool ValidateSource(pl_tex src, const BufferLayout &layout, std::string *error) {
  if (src == nullptr) {
    *error = "source texture is null";
    return false;
  }
  if (!src->params.sampleable) {
    *error = "source texture is not sampleable";
    return false;
  }
  if (src->params.w != layout.width || src->params.h != layout.height || src->params.d != 0) {
    *error = "source texture dimensions do not match the configured layout";
    return false;
  }
  pl_fmt fmt = src->params.format;
  if (fmt == nullptr || fmt->type != PL_FMT_FLOAT || fmt->opaque) {
    *error = "source texture is not a non-opaque float format";
    return false;
  }
  if (fmt->num_components < 3) {
    *error = "source texture has fewer than 3 components";
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (fmt->component_depth[i] < 16) {
      *error = "source texture component precision is below 16 bits";
      return false;
    }
  }
  return true;
}

bool DispatchPack(pl_gpu gpu, FramePackerImpl &impl, pl_tex src, pl_tex carrier) {
  (void)gpu;
  pl_shader shader = pl_dispatch_begin(impl.dispatch);
  if (shader == nullptr) {
    return false;
  }
  pl_shader_desc desc{};
  desc.desc.name = "src_tex";
  desc.desc.type = PL_DESC_SAMPLED_TEX;
  desc.binding.object = src;
  desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
  desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

  pl_custom_shader custom{};
  custom.header = impl.shader_header.c_str();
  custom.body = impl.shader_body.c_str();
  custom.output = PL_SHADER_SIG_COLOR;
  custom.descriptors = &desc;
  custom.num_descriptors = 1;
  if (!pl_shader_custom(shader, &custom)) {
    pl_dispatch_abort(impl.dispatch, &shader);
    return false;
  }

  pl_dispatch_params params{};
  params.shader = &shader;
  params.target = carrier;
  // On failure pl_dispatch_finish already released its reference to the shader.
  return pl_dispatch_finish(impl.dispatch, &params);
}

} // namespace

FramePacker::FramePacker(pl_gpu gpu, const PackConfig &config) : gpu_(gpu), config_(config) {
  ValidateConfig(gpu, config);

  auto impl = std::make_unique<FramePackerImpl>();
  pl_fmt carrier_format = FindCarrierFormat(gpu);
  if (carrier_format->texel_align != 0 &&
      static_cast<std::size_t>(config.layout.row_bytes) % carrier_format->texel_align != 0) {
    throw std::runtime_error("FramePacker: carrier row_bytes is not a multiple of the format texel alignment");
  }
  impl->shader_header = BuildShaderHeader(config);
  impl->shader_body = BuildShaderBody(config.layout.pixel_format);
  impl->dispatch = pl_dispatch_create(gpu->log, gpu);
  if (impl->dispatch == nullptr) {
    throw std::runtime_error("FramePacker: pl_dispatch_create failed");
  }

  const int carrier_width = config.layout.row_bytes / 4;
  try {
    for (StagingSlot &slot : impl->slots) {
      pl_tex_params tex_params{};
      tex_params.w = carrier_width;
      tex_params.h = config.layout.height;
      tex_params.format = carrier_format;
      tex_params.renderable = true;
      tex_params.host_readable = true;
      slot.carrier = CreateTexture(gpu, &tex_params);
      if (slot.carrier == nullptr) {
        throw std::runtime_error("FramePacker: staging carrier texture allocation failed");
      }

      pl_buf_params buf_params{};
      buf_params.size = config.layout.image_bytes;
      buf_params.host_readable = true;
      buf_params.host_mapped = true;
      buf_params.memory_type = PL_BUF_MEM_HOST;
      slot.buffer = CreateBuffer(gpu, &buf_params);
      if (slot.buffer == nullptr || slot.buffer->data == nullptr) {
        throw std::runtime_error("FramePacker: staging mapped buffer allocation failed");
      }
    }
  } catch (...) {
    DestroyResources(gpu, *impl);
    throw;
  }
  impl_ = std::move(impl);
}

// The destructor is safe for a packer that was never used, but terminates loudly if
// any slot is still pending or borrowed instead of leaking or hiding the problem.
FramePacker::~FramePacker() {
  if (impl_ == nullptr) {
    return;
  }
  int owned = 0;
  for (const StagingSlot &slot : impl_->slots) {
    if (slot.state != SlotState::Free) {
      ++owned;
    }
  }
  if (owned != 0) {
    std::fprintf(stderr, "FramePacker destroyed with %d slot(s) still owned or pending; terminating\n", owned);
    std::fflush(stderr);
    std::terminate();
  }
  DestroyResources(gpu_, *impl_);
}

StagingSlot *FramePacker::Submit(pl_tex rgb_src, uint64_t frame_id) {
  if (impl_ == nullptr || impl_->shutting_down) {
    return nullptr;
  }

  // Opportunistically reclaim aborted slots whose render/download have completed.
  for (StagingSlot &slot : impl_->slots) {
    if (slot.state == SlotState::Aborted && SlotResourcesIdle(gpu_, slot)) {
      ResetSlot(slot);
    }
  }

  StagingSlot *slot = nullptr;
  for (StagingSlot &candidate : impl_->slots) {
    if (candidate.state == SlotState::Free) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    return nullptr;
  }

  slot->state = SlotState::Pending;
  slot->source = rgb_src;
  slot->frame_id = frame_id;
  slot->render_enqueued = false;
  slot->download_enqueued = false;

  // Never enqueue new work on an already failed GPU; report it as a slot failure so
  // it cannot be mistaken for "ring full".
  if (GpuIsFailed(gpu_)) {
    slot->state = SlotState::Failed;
    EmitDiagnostic("submit", frame_id, "GPU is already failed; not enqueueing dispatch/download");
    return slot;
  }

  std::string error;
  if (!ValidateSource(rgb_src, config_.layout, &error)) {
    slot->state = SlotState::Failed;
    EmitDiagnostic("validate", frame_id, error.c_str());
    return slot;
  }

  pl_dispatch_reset_frame(impl_->dispatch);
  // Mark the render before dispatch: even a partially enqueued or failed dispatch
  // still requires the carrier (and source) to be provably idle before reuse.
  slot->render_enqueued = true;
  if (!DispatchPack(gpu_, *impl_, rgb_src, slot->carrier)) {
    slot->state = SlotState::Failed;
    EmitDiagnostic("dispatch", frame_id, "shader dispatch failed");
    return slot;
  }

  pl_tex_transfer_params transfer{};
  transfer.tex = slot->carrier;
  transfer.row_pitch = static_cast<std::size_t>(config_.layout.row_bytes);
  transfer.buf = slot->buffer;
  transfer.buf_offset = 0;
  slot->download_enqueued = true;
  if (!DownloadTexture(gpu_, &transfer)) {
    slot->state = SlotState::Failed;
    EmitDiagnostic("download", frame_id, "asynchronous texture download was rejected");
    return slot;
  }

  FlushGpu(gpu_);
  slot->state = SlotState::Pending;
  return slot;
}

PackPoll FramePacker::Poll(StagingSlot *slot) {
  if (impl_ == nullptr || !OwnsSlot(*impl_, slot)) {
    return PackPoll::Failed;
  }
  switch (slot->state) {
  case SlotState::Free:
  case SlotState::Failed:
    return PackPoll::Failed;
  case SlotState::Ready:
    return PackPoll::Ready;
  case SlotState::Aborted:
    // The caller gave up this slot; reclaim it once all its work is done.
    if (SlotResourcesIdle(gpu_, *slot)) {
      ResetSlot(*slot);
    }
    return PackPoll::Failed;
  case SlotState::Pending:
    break;
  }

  // A failed GPU can report a buffer as "not busy" without meaning the bytes are
  // usable, so the failure check must come first.
  if (GpuIsFailed(gpu_)) {
    slot->state = SlotState::Failed;
    EmitDiagnostic("poll", slot->frame_id, "GPU failed before readback");
    return PackPoll::Failed;
  }
  if (PollBuffer(gpu_, slot->buffer, 0)) {
    return PackPoll::NotReady;
  }
  // pl_buf_poll itself may make progress and discover device loss, so re-check
  // before promoting the slot to Ready.
  if (GpuIsFailed(gpu_)) {
    slot->state = SlotState::Failed;
    EmitDiagnostic("poll", slot->frame_id, "GPU failed while polling the readback buffer");
    return PackPoll::Failed;
  }
  slot->state = SlotState::Ready;
  return PackPoll::Ready;
}

const uint8_t *FramePacker::Bytes(const StagingSlot *slot) const {
  if (impl_ == nullptr || !OwnsSlot(*impl_, slot) || slot->state != SlotState::Ready || slot->buffer == nullptr) {
    return nullptr;
  }
  return static_cast<const uint8_t *>(slot->buffer->data);
}

std::size_t FramePacker::Size(const StagingSlot *slot) const {
  if (impl_ == nullptr || !OwnsSlot(*impl_, slot) || slot->state != SlotState::Ready) {
    return 0;
  }
  return config_.layout.image_bytes;
}

void FramePacker::Reap(StagingSlot *slot) {
  if (impl_ == nullptr || !OwnsSlot(*impl_, slot)) {
    throw std::invalid_argument("FramePacker::Reap: slot does not belong to this packer");
  }
  if (slot->state == SlotState::Ready) {
    // Ready is only reported after a trusted poll, so the bytes are final.
    ResetSlot(*slot);
    return;
  }
  if (slot->state == SlotState::Failed) {
    // Failed does not mean finished: the render (and possibly the download) may still
    // be in flight, so the slot is only reusable once every relevant GPU activity is
    // provably idle.
    if (!SlotResourcesIdle(gpu_, *slot)) {
      throw std::logic_error("FramePacker::Reap: Failed slot resources are still in use");
    }
    ResetSlot(*slot);
    return;
  }
  throw std::logic_error("FramePacker::Reap: slot is neither Ready nor Failed");
}

void FramePacker::Abort(StagingSlot *slot) {
  if (impl_ == nullptr || !OwnsSlot(*impl_, slot)) {
    throw std::invalid_argument("FramePacker::Abort: slot does not belong to this packer");
  }
  switch (slot->state) {
  case SlotState::Ready:
    // The download completed before Ready was reported: safe to reuse immediately.
    ResetSlot(*slot);
    return;
  case SlotState::Failed:
    // Failed does not mean the render/download completed; keep the slot (and the
    // source) retained until every relevant GPU activity is provably idle.
    if (SlotResourcesIdle(gpu_, *slot)) {
      ResetSlot(*slot);
    } else {
      slot->state = SlotState::Aborted;
    }
    return;
  case SlotState::Pending:
    // Keep the slot (and its source texture) until the download completes.
    slot->state = SlotState::Aborted;
    return;
  case SlotState::Aborted:
    throw std::logic_error("FramePacker::Abort: slot was already aborted");
  case SlotState::Free:
    throw std::logic_error("FramePacker::Abort: slot is not in use");
  }
}

PackDrain FramePacker::Shutdown(int timeout_ms) {
  if (impl_ == nullptr) {
    return PackDrain::Idle;
  }
  impl_->shutting_down = true;
  const int bounded = std::max(0, timeout_ms);
  const auto deadline = Clock::now() + std::chrono::milliseconds(bounded);
  for (;;) {
    if (GpuIsFailed(gpu_)) {
      return PackDrain::Failed;
    }
    std::array<StagingSlot *, kSlotCount> reclaim{};
    int reclaim_count = 0;
    bool all_free = true;
    for (StagingSlot &slot : impl_->slots) {
      if (slot.state == SlotState::Free) {
        continue;
      }
      // Pending/Aborted/Failed slots hold no borrowed bytes and can be drained;
      // Ready slots are still owned by the caller and block Idle.
      const bool cancellable =
          slot.state == SlotState::Pending || slot.state == SlotState::Aborted || slot.state == SlotState::Failed;
      if (cancellable && SlotResourcesIdle(gpu_, slot)) {
        reclaim[static_cast<std::size_t>(reclaim_count++)] = &slot;
        continue;
      }
      // Ready (borrowed) slots, and slots whose render/download is still in flight.
      all_free = false;
    }
    // A poll inside SlotResourcesIdle may have discovered device loss; never recycle
    // resources on the strength of an untrustworthy completion result.
    if (GpuIsFailed(gpu_)) {
      return PackDrain::Failed;
    }
    for (int i = 0; i < reclaim_count; ++i) {
      ResetSlot(*reclaim[static_cast<std::size_t>(i)]);
    }
    if (all_free) {
      return PackDrain::Idle;
    }
    if (Clock::now() >= deadline) {
      return PackDrain::TimedOut;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kDrainPollIntervalMs));
  }
}

const PackConfig &FramePacker::config() const { return config_; }

} // namespace EYEQ
