#pragma once

#include <cstdint>

#include <libplacebo/gpu.h>

namespace EYEQ {
namespace frame_packer_test {

// Test-only overrides for a few libplacebo calls used by src/output/frame_packer.cpp.
// Every null member delegates to the real function. This header is only included by
// frame_packer.cpp when the fault-injection test target defines
// EYEQ_FRAME_PACKER_TEST_HOOKS; normal builds, the player and the GPU test link the
// real implementation and never see these hooks.
struct Hooks {
  bool (*tex_download)(pl_gpu, const struct pl_tex_transfer_params *) = nullptr;
  bool (*tex_poll)(pl_gpu, pl_tex, uint64_t) = nullptr;
  bool (*buf_poll)(pl_gpu, pl_buf, uint64_t) = nullptr;
  bool (*gpu_is_failed)(pl_gpu) = nullptr;
  void (*gpu_flush)(pl_gpu) = nullptr;
  // Allocation/destruction overrides, used to exercise constructor rollback. The
  // destroy hooks must forward to the real libplacebo function after counting.
  pl_tex (*tex_create)(pl_gpu, const struct pl_tex_params *) = nullptr;
  pl_buf (*buf_create)(pl_gpu, const struct pl_buf_params *) = nullptr;
  void (*tex_destroy)(pl_gpu, pl_tex *) = nullptr;
  void (*buf_destroy)(pl_gpu, pl_buf *) = nullptr;
  // Receives one formatted failure diagnostic per transition (stage/frame_id/reason).
  void (*diagnostic)(const char *message) = nullptr;
};

// Returns the process-wide hook table. Defined in frame_packer.cpp only for the
// fault-injection build; linking it otherwise is an error.
Hooks &MutableHooks();

} // namespace frame_packer_test
} // namespace EYEQ
