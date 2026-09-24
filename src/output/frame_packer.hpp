#pragma once

#include <cstddef>
#include <cstdint>

#include <libplacebo/gpu.h>

#include "output/pixel_format.hpp"

namespace EYEQ {

struct StagingSlot; // opaque, owned by the packer

enum class PackPoll { NotReady, Ready, Failed };
enum class PackDrain { Idle, TimedOut, Failed };

// Declaration only in phase 0.5: no GPU packing implementation exists yet. There is
// intentionally no runtime stub, so calling an unimplemented method fails to link
// instead of silently reporting success.
class FramePacker {
public:
  FramePacker(pl_gpu gpu, const PackConfig &config);
  ~FramePacker();

  StagingSlot *Submit(pl_tex rgb_src, uint64_t frame_id); // nullptr when the ring is full
  PackPoll Poll(StagingSlot *slot);                       // non-blocking
  const uint8_t *Bytes(const StagingSlot *slot) const;    // valid Ready..Reap/Abort
  std::size_t Size(const StagingSlot *slot) const;
  void Reap(StagingSlot *slot);       // only after the downstream no longer needs the bytes
  void Abort(StagingSlot *slot);      // logical cancel; releases after GPU completion
  PackDrain Shutdown(int timeout_ms); // TimedOut -> the object stays alive

  const PackConfig &config() const;

private:
  pl_gpu gpu_ = nullptr;
  PackConfig config_;
};

} // namespace EYEQ
