#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <libplacebo/gpu.h>

#include "output/pixel_format.hpp"

namespace EYEQ {

struct StagingSlot; // opaque, owned by the packer
struct FramePackerImpl;

enum class PackPoll { NotReady, Ready, Failed };
enum class PackDrain { Idle, TimedOut, Failed };

// GPU packer for one DeckLink output. It converts a sampled, nonlinear full-range
// float RGB texture on the same pl_gpu into r210/v210 Video-range bytes, using a
// fixed ring of four staging slots and asynchronous pl_tex_download + pl_buf_poll
// readback. A private Impl owns all GPU resources; the public seam is unchanged.
//
// Ownership and failure semantics (explicit and uniform):
//   - One thread owns a FramePacker for its whole lifetime; it is neither copyable
//     nor movable. The caller owns rgb_src and must keep it alive on the same GPU
//     until the slot is Ready and Reap'ed, or until its render/download is confirmed
//     complete and the slot reclaimed. Storing the pointer does not take a reference:
//     Failed means this submission failed, not that the source may be destroyed.
//   - The packer cannot verify that rgb_src belongs to the same pl_gpu (pl_tex
//     carries no device identity); same-GPU use is a caller precondition.
//   - Submit returns nullptr only when no slot is free (or after Shutdown began).
//     Invalid input and GPU/dispatch/transfer failures return a slot instead of
//     nullptr; that slot polls Failed and never masquerades as "ring full".
//   - Bytes/Size are valid only between Ready and Reap/Abort; Size is image_bytes.
//   - Reap and Abort reject foreign or wrong-state handles with std::logic_error /
//     std::invalid_argument instead of reading an arbitrary pointer, and only
//     recycle a slot once its render and download are provably complete.
//   - Shutdown never reclaims bytes still held by the caller: it returns TimedOut
//     while a Ready slot is outstanding and only Idle once every slot is free.
//   - Each failure transition emits one diagnostic line (failing stage, frame_id,
//     reason) to stderr; there is no public error channel or event subsystem.
class FramePacker {
public:
  FramePacker(pl_gpu gpu, const PackConfig &config);
  ~FramePacker();

  FramePacker(const FramePacker &) = delete;
  FramePacker &operator=(const FramePacker &) = delete;
  FramePacker(FramePacker &&) = delete;
  FramePacker &operator=(FramePacker &&) = delete;

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
  std::unique_ptr<FramePackerImpl> impl_;
};

} // namespace EYEQ
