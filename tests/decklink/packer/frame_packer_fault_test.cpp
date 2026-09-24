// Fault-injection tests for the FramePacker lifecycle. The real frame_packer.cpp is
// compiled into this target with EYEQ_FRAME_PACKER_TEST_HOOKS, so a few libplacebo
// calls can be overridden deterministically. There is no product switch; the hooks
// live only in this test target.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "frame_packer_test_hooks.hpp"
#include "gpu_test_device.hpp"
#include "packer_test_support.hpp"

using namespace EYEQ;
using namespace eyeq_test;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

using Hooks = frame_packer_test::Hooks;

int g_flush_calls = 0;
int g_download_calls = 0;
int g_tex_created = 0;
int g_buf_created = 0;
int g_tex_destroyed = 0;
int g_buf_destroyed = 0;
bool g_gpu_failed = false;
std::vector<std::string> g_diagnostics;

struct HooksGuard {
  ~HooksGuard() { frame_packer_test::MutableHooks() = Hooks{}; }
};

void ResetCounters() {
  g_flush_calls = 0;
  g_download_calls = 0;
  g_tex_created = 0;
  g_buf_created = 0;
  g_tex_destroyed = 0;
  g_buf_destroyed = 0;
  g_gpu_failed = false;
  g_diagnostics.clear();
}

bool HookGpuFailed(pl_gpu) { return g_gpu_failed; }

bool HookFlipFailureOnPoll(pl_gpu, pl_buf, uint64_t) {
  g_gpu_failed = true;
  return false; // not busy, but the poll just discovered device loss
}

bool HookAlwaysBusyTexPoll(pl_gpu, pl_tex, uint64_t) { return true; }

bool HookCountFailDownload(pl_gpu, const struct pl_tex_transfer_params *) {
  ++g_download_calls;
  return false;
}

void HookCountFlush(pl_gpu) { ++g_flush_calls; }

pl_tex HookCountCreateTex(pl_gpu gpu, const struct pl_tex_params *params) {
  ++g_tex_created;
  return pl_tex_create(gpu, params);
}

pl_buf HookCountCreateBuf(pl_gpu gpu, const struct pl_buf_params *params) {
  ++g_buf_created;
  return pl_buf_create(gpu, params);
}

pl_buf HookFailNthCreateBuf(pl_gpu gpu, const struct pl_buf_params *params) {
  ++g_buf_created;
  if (g_buf_created == 3) {
    return nullptr;
  }
  return pl_buf_create(gpu, params);
}

pl_tex HookFailSecondCreateTex(pl_gpu gpu, const struct pl_tex_params *params) {
  ++g_tex_created;
  if (g_tex_created == 2) {
    return nullptr;
  }
  return pl_tex_create(gpu, params);
}

void HookCountDestroyTex(pl_gpu gpu, pl_tex *tex) {
  ++g_tex_destroyed;
  pl_tex_destroy(gpu, tex);
}

void HookCountDestroyBuf(pl_gpu gpu, pl_buf *buf) {
  ++g_buf_destroyed;
  pl_buf_destroy(gpu, buf);
}

void HookCaptureDiagnostic(const char *message) { g_diagnostics.emplace_back(message); }

bool DiagContains(const std::string &needle) {
  return std::any_of(g_diagnostics.begin(), g_diagnostics.end(),
                     [&](const std::string &line) { return line.find(needle) != std::string::npos; });
}

std::vector<float> FlatPattern(int width, int height) { return MakePattern(width, height, 3, false); }

PackConfig SmallConfig(int width = 13, int height = 3) {
  return MakeConfig(width, height, PixelFormat::V210, Sdr709(), 0);
}

void TestRingFull() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  StagingSlot *slots[4] = {};
  for (int i = 0; i < 4; ++i) {
    slots[i] = packer.Submit(source, static_cast<uint64_t>(i));
    CHECK(slots[i] != nullptr);
  }
  CHECK(packer.Submit(source, 4) == nullptr); // ring full, no new input consumed

  for (int i = 0; i < 4; ++i) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(20000);
    while (packer.Poll(slots[i]) == PackPoll::NotReady) {
      CHECK(Clock::now() < deadline);
    }
    CHECK(packer.Poll(slots[i]) == PackPoll::Ready);
    CHECK(packer.Size(slots[i]) == config.layout.image_bytes);
    CHECK(packer.Bytes(slots[i]) != nullptr);
  }
  CHECK(packer.Submit(source, 5) == nullptr); // still full while Ready
  for (int i = 0; i < 4; ++i) {
    packer.Reap(slots[i]);
  }
  CHECK(packer.Submit(source, 6) != nullptr);
  CHECK(packer.Shutdown(20000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

void TestTransferFailureIsNotRingFull() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  Hooks hooks;
  hooks.tex_download = HookCountFailDownload;
  hooks.diagnostic = HookCaptureDiagnostic;
  frame_packer_test::MutableHooks() = hooks;

  StagingSlot *slot = packer.Submit(source, 1);
  CHECK(slot != nullptr); // NOT nullptr: a transfer failure is a slot failure
  CHECK(packer.Poll(slot) == PackPoll::Failed);
  CHECK(packer.Bytes(slot) == nullptr);
  CHECK(packer.Size(slot) == 0);
  CHECK(DiagContains("stage=download"));
  CHECK(DiagContains("frame_id=1"));
  // The preceding render is still in flight; Shutdown drains it rather than reporting
  // Idle immediately.
  frame_packer_test::MutableHooks() = Hooks{};
  CHECK(packer.Shutdown(20000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

// F1: a rejected download does not prove the already submitted render finished, so
// Reap/Abort/Shutdown must not reuse the slot or report Idle prematurely.
void TestRenderInFlightAfterFailedDownload() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);
  (void)RunOneFrame(packer, source, 0, 20000); // warm up shader compilation

  Hooks hooks;
  hooks.tex_download = HookCountFailDownload;
  hooks.tex_poll = HookAlwaysBusyTexPoll; // model the render still being in flight
  frame_packer_test::MutableHooks() = hooks;

  StagingSlot *slot = packer.Submit(source, 71);
  CHECK(slot != nullptr);
  CHECK(packer.Poll(slot) == PackPoll::Failed);

  bool threw = false;
  try {
    packer.Reap(slot);
  } catch (const std::logic_error &) {
    threw = true;
  }
  CHECK(threw); // render still in flight: no premature recycle

  packer.Abort(slot); // must keep the slot until the render is provably done
  CHECK(packer.Bytes(slot) == nullptr);
  CHECK(packer.Shutdown(0) == PackDrain::TimedOut);
  CHECK(packer.Submit(source, 72) == nullptr); // Shutdown refuses new work

  frame_packer_test::MutableHooks() = Hooks{};
  CHECK(packer.Shutdown(20000) == PackDrain::Idle); // real polls now complete
  pl_tex_destroy(device.gpu(), &source);
}

void TestTimeoutThenRecovery() {
  const PackConfig config = MakeConfig(13, 3, PixelFormat::R210, Sdr709(), 0);
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  Hooks hooks;
  hooks.buf_poll = [](pl_gpu, pl_buf, uint64_t) { return true; }; // always busy
  frame_packer_test::MutableHooks() = hooks;

  StagingSlot *slot = packer.Submit(source, 1);
  CHECK(slot != nullptr);
  CHECK(packer.Poll(slot) == PackPoll::NotReady);
  CHECK(packer.Shutdown(20) == PackDrain::TimedOut);
  CHECK(packer.Submit(source, 2) == nullptr); // Shutdown refuses new work
  frame_packer_test::MutableHooks() = Hooks{};
  CHECK(packer.Shutdown(20000) == PackDrain::Idle); // object stays valid and retries
  pl_tex_destroy(device.gpu(), &source);
}

// F2: device loss discovered while polling must not be reported as Ready/Idle.
void TestGpuFailsDuringPoll() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  StagingSlot *slot = packer.Submit(source, 81);
  CHECK(slot != nullptr);
  pl_gpu_finish(device.gpu()); // drain the real work before modelling a poll-time loss
  ResetCounters();
  Hooks hooks;
  hooks.gpu_is_failed = HookGpuFailed;
  hooks.buf_poll = HookFlipFailureOnPoll;
  hooks.diagnostic = HookCaptureDiagnostic;
  frame_packer_test::MutableHooks() = hooks;

  CHECK(packer.Poll(slot) == PackPoll::Failed);
  CHECK(packer.Bytes(slot) == nullptr);
  CHECK(DiagContains("stage=poll"));
  frame_packer_test::MutableHooks() = Hooks{};
  g_gpu_failed = false;
  packer.Abort(slot);
  CHECK(packer.Shutdown(20000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

void TestGpuFailsDuringShutdown() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  StagingSlot *slot = packer.Submit(source, 82);
  CHECK(slot != nullptr);
  pl_gpu_finish(device.gpu());
  ResetCounters();
  Hooks hooks;
  hooks.gpu_is_failed = HookGpuFailed;
  hooks.buf_poll = HookFlipFailureOnPoll;
  frame_packer_test::MutableHooks() = hooks;

  CHECK(packer.Shutdown(0) == PackDrain::Failed);
  frame_packer_test::MutableHooks() = Hooks{};
  g_gpu_failed = false;
  CHECK(packer.Shutdown(20000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

// F2: Submit must not dispatch/download on an already failed GPU.
void TestAlreadyFailedGpuSubmit() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  ResetCounters();
  g_gpu_failed = true;
  Hooks hooks;
  hooks.gpu_is_failed = HookGpuFailed;
  hooks.tex_download = HookCountFailDownload;
  hooks.diagnostic = HookCaptureDiagnostic;
  frame_packer_test::MutableHooks() = hooks;

  StagingSlot *slot = packer.Submit(source, 91);
  CHECK(slot != nullptr);
  CHECK(g_download_calls == 0); // no new GPU work was enqueued
  CHECK(packer.Poll(slot) == PackPoll::Failed);
  CHECK(DiagContains("stage=submit"));
  CHECK(DiagContains("frame_id=91"));
  frame_packer_test::MutableHooks() = Hooks{};
  g_gpu_failed = false;
  packer.Abort(slot);
  CHECK(packer.Shutdown(20000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

void TestAbortPendingAndReady() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  StagingSlot *ready = packer.Submit(source, 1);
  CHECK(ready != nullptr);
  const auto deadline = Clock::now() + std::chrono::milliseconds(20000);
  while (packer.Poll(ready) == PackPoll::NotReady) {
    CHECK(Clock::now() < deadline);
  }
  CHECK(packer.Bytes(ready) != nullptr);
  packer.Abort(ready);
  CHECK(packer.Bytes(ready) == nullptr);
  CHECK(packer.Size(ready) == 0);

  StagingSlot *pending = packer.Submit(source, 2);
  CHECK(pending != nullptr);
  packer.Abort(pending);
  CHECK(packer.Bytes(pending) == nullptr);
  CHECK(packer.Shutdown(20000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

void TestReapAndAbortMisuse() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);
  auto foreign = reinterpret_cast<StagingSlot *>(static_cast<uintptr_t>(0x1000));

  bool threw = false;
  try {
    packer.Reap(foreign);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    packer.Abort(foreign);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
  CHECK(packer.Bytes(foreign) == nullptr);
  CHECK(packer.Size(foreign) == 0);
  CHECK(packer.Poll(foreign) == PackPoll::Failed);

  StagingSlot *slot = packer.Submit(source, 1);
  CHECK(slot != nullptr);
  threw = false;
  try {
    packer.Reap(slot); // still pending
  } catch (const std::logic_error &) {
    threw = true;
  }
  CHECK(threw);

  const auto deadline = Clock::now() + std::chrono::milliseconds(20000);
  while (packer.Poll(slot) == PackPoll::NotReady) {
    CHECK(Clock::now() < deadline);
  }
  packer.Reap(slot);

  threw = false;
  try {
    packer.Reap(slot); // already free
  } catch (const std::logic_error &) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    packer.Abort(slot); // free slot
  } catch (const std::logic_error &) {
    threw = true;
  }
  CHECK(threw);
  CHECK(packer.Shutdown(2000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

void TestReapReuseKeepsOtherSlots() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  std::vector<float> pattern_a = MakePattern(config.layout.width, config.layout.height, 11, false);
  std::vector<float> pattern_b = MakePattern(config.layout.width, config.layout.height, 22, false);
  pl_tex source_a = UploadSource(device.gpu(), config.layout.width, config.layout.height, pattern_a, false);
  pl_tex source_b = UploadSource(device.gpu(), config.layout.width, config.layout.height, pattern_b, false);

  auto wait_ready = [&](StagingSlot *slot) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(20000);
    while (packer.Poll(slot) == PackPoll::NotReady) {
      CHECK(Clock::now() < deadline);
    }
    CHECK(packer.Poll(slot) == PackPoll::Ready);
  };

  StagingSlot *first = packer.Submit(source_a, 1);
  StagingSlot *second = packer.Submit(source_b, 2);
  CHECK(first != nullptr && second != nullptr && first != second);
  wait_ready(first);
  wait_ready(second);
  const std::vector<uint8_t> second_bytes(packer.Bytes(second), packer.Bytes(second) + packer.Size(second));

  packer.Reap(first);
  StagingSlot *reused = packer.Submit(source_a, 3);
  CHECK(reused != nullptr);
  wait_ready(reused);
  CHECK(packer.Poll(second) == PackPoll::Ready);
  CHECK(std::equal(second_bytes.begin(), second_bytes.end(), packer.Bytes(second)));
  packer.Reap(second);
  packer.Reap(reused);
  CHECK(packer.Shutdown(2000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source_a);
  pl_tex_destroy(device.gpu(), &source_b);
}

void TestInvalidSourceFails() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  Hooks hooks;
  hooks.diagnostic = HookCaptureDiagnostic;
  frame_packer_test::MutableHooks() = hooks;
  StagingSlot *slot = packer.Submit(nullptr, 12345);
  CHECK(slot != nullptr);
  CHECK(packer.Poll(slot) == PackPoll::Failed);
  CHECK(DiagContains("stage=validate"));
  CHECK(DiagContains("frame_id=12345"));
  packer.Reap(slot); // no render was enqueued for an invalid source
  frame_packer_test::MutableHooks() = Hooks{};
  CHECK(packer.Shutdown(2000) == PackDrain::Idle);
}

// The constructor must reject malformed configs and clean up on allocation failure.
void TestConstructorValidationAndRollback() {
  GpuDevice device;
  pl_tex source = UploadSource(device.gpu(), 13, 3, FlatPattern(13, 3), false);

  {
    PackConfig bad = SmallConfig();
    bad.layout.width = 0;
    bool threw = false;
    try {
      FramePacker packer(device.gpu(), bad);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    CHECK(threw);
  }
  {
    PackConfig bad = SmallConfig();
    bad.output.range = ColorRange::Full;
    bool threw = false;
    try {
      FramePacker packer(device.gpu(), bad);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    CHECK(threw);
  }
  {
    PackConfig bad = SmallConfig();
    bad.output.chroma_loc = ChromaLocation::Center;
    bool threw = false;
    try {
      FramePacker packer(device.gpu(), bad);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    CHECK(threw);
  }

  // Failing the third buffer allocation must destroy the carriers/buffers already
  // created for the first two slots (and the third carrier).
  ResetCounters();
  {
    Hooks hooks;
    hooks.tex_create = HookCountCreateTex;
    hooks.buf_create = HookFailNthCreateBuf;
    hooks.tex_destroy = HookCountDestroyTex;
    hooks.buf_destroy = HookCountDestroyBuf;
    frame_packer_test::MutableHooks() = hooks;
    bool threw = false;
    try {
      FramePacker packer(device.gpu(), SmallConfig());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    CHECK(threw);
    CHECK(g_tex_created == 3);
    CHECK(g_buf_created == 3);
    CHECK(g_tex_destroyed == 3);
    CHECK(g_buf_destroyed == 2);
    frame_packer_test::MutableHooks() = Hooks{};
  }

  // Failing the second carrier allocation must destroy the first slot's carrier/buffer.
  ResetCounters();
  {
    Hooks hooks;
    hooks.tex_create = HookFailSecondCreateTex;
    hooks.buf_create = HookCountCreateBuf;
    hooks.tex_destroy = HookCountDestroyTex;
    hooks.buf_destroy = HookCountDestroyBuf;
    frame_packer_test::MutableHooks() = hooks;
    bool threw = false;
    try {
      FramePacker packer(device.gpu(), SmallConfig());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    CHECK(threw);
    CHECK(g_tex_created == 2);
    CHECK(g_buf_created == 1);
    CHECK(g_tex_destroyed == 1);
    CHECK(g_buf_destroyed == 1);
    frame_packer_test::MutableHooks() = Hooks{};
  }

  pl_tex_destroy(device.gpu(), &source);
}

// Ready bytes are owned by the caller: Shutdown must not silently reclaim them.
void TestReadyHeldShutdownTimesOut() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  StagingSlot *slot = packer.Submit(source, 1);
  CHECK(slot != nullptr);
  const auto deadline = Clock::now() + std::chrono::milliseconds(20000);
  while (packer.Poll(slot) == PackPoll::NotReady) {
    CHECK(Clock::now() < deadline);
  }
  CHECK(packer.Poll(slot) == PackPoll::Ready);
  CHECK(packer.Shutdown(0) == PackDrain::TimedOut); // borrowed bytes block Idle
  CHECK(packer.Bytes(slot) != nullptr);
  packer.Reap(slot);
  CHECK(packer.Shutdown(2000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

void TestShutdownRefusesSubmitAndFlushRuns() {
  const PackConfig config = SmallConfig();
  GpuDevice device;
  FramePacker packer(device.gpu(), config);
  pl_tex source = UploadSource(device.gpu(), config.layout.width, config.layout.height,
                               FlatPattern(config.layout.width, config.layout.height), false);

  ResetCounters();
  Hooks hooks;
  hooks.gpu_flush = HookCountFlush;
  frame_packer_test::MutableHooks() = hooks;
  StagingSlot *slot = packer.Submit(source, 1);
  CHECK(slot != nullptr);
  frame_packer_test::MutableHooks() = Hooks{};
  CHECK(g_flush_calls > 0);
  const auto deadline = Clock::now() + std::chrono::milliseconds(20000);
  while (packer.Poll(slot) == PackPoll::NotReady) {
    CHECK(Clock::now() < deadline);
  }
  packer.Reap(slot);
  CHECK(packer.Shutdown(2000) == PackDrain::Idle);
  CHECK(packer.Submit(source, 2) == nullptr); // Shutdown began
  pl_tex_destroy(device.gpu(), &source);
}

void TestRepeatedCreateDestroy() {
  const PackConfig config = MakeConfig(11, 2, PixelFormat::R210, Sdr709(), 0);
  GpuDevice device;
  for (int i = 0; i < 20; ++i) {
    FramePacker packer(device.gpu(), config);
    CHECK(packer.Shutdown(0) == PackDrain::Idle);
  }
}

} // namespace

int main() {
  try {
    HooksGuard guard;
    TestRingFull();
    TestTransferFailureIsNotRingFull();
    TestRenderInFlightAfterFailedDownload();
    TestTimeoutThenRecovery();
    TestGpuFailsDuringPoll();
    TestGpuFailsDuringShutdown();
    TestAlreadyFailedGpuSubmit();
    TestAbortPendingAndReady();
    TestReapAndAbortMisuse();
    TestReapReuseKeepsOtherSlots();
    TestInvalidSourceFails();
    TestConstructorValidationAndRollback();
    TestReadyHeldShutdownTimesOut();
    TestShutdownRefusesSubmitAndFlushRuns();
    TestRepeatedCreateDestroy();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "frame_packer_fault FAILED: %s\n", error.what());
    return 1;
  }
  std::printf("frame_packer_fault: OK\n");
  return 0;
}
