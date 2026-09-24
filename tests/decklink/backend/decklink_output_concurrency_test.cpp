// Concurrency / reentrancy tests for the output backend. The callback-side state is
// protected by one std::mutex; these tests exercise producer/consumer overlap. Run
// the whole binary under TSan where available.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_decklink_output.hpp"
#include "output/decklink_backend.hpp"

using namespace EYEQ;
using eyeq_decklink_test::CaptureRegisteredCallbackForLateDelivery;
using eyeq_decklink_test::DeliverCompletion;
using eyeq_decklink_test::ResetFakeOutput;
using eyeq_decklink_test::SetCompletionGate;
using eyeq_decklink_test::SetScheduleReentrant;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

OutputRequest MakeRequest() {
  OutputRequest request;
  request.device.index = 0;
  request.mode = MonitorMode{3840, 2160, 1000, 50000, true};
  request.connection = VideoConnection::SDI;
  request.pixel_format = PixelFormat::V210;
  return request;
}

SubmissionInfo MakeInfo(uint64_t id) {
  SubmissionInfo info;
  info.submit_id = id;
  return info;
}

void TestConcurrentCompletionDelivery() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x5A);
    for (int i = 0; i < 8; ++i) {
      CHECK(output.TrySchedule(PackedFrameView{bytes.data(), bytes.size()}, MakeInfo(static_cast<uint64_t>(i)), 1000,
                               1000) == ScheduleOutcome::Accepted);
    }

    std::atomic<int> delivered{0};
    std::thread worker([&] {
      for (int i = 0; i < 8; ++i) {
        CHECK(DeliverCompletion(i, 0));
        delivered.fetch_add(1, std::memory_order_release);
      }
    });

    int received = 0;
    while (received < 8) {
      OutputEvent event;
      if (output.PollEvent(&event)) {
        CHECK(event.kind == OutputEvent::Kind::FrameCompleted);
        ++received;
      } else {
        std::this_thread::yield();
      }
    }
    worker.join();
    CHECK(delivered.load(std::memory_order_acquire) == 8);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestConcurrentDeliveryDuringScheduling() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x6B);

    // The worker only delivers a completion once the main thread has confirmed that
    // the frame was scheduled, so the fake never reports a stale frame.
    std::atomic<int> scheduled{0};
    std::thread worker([&] {
      for (int frame = 0; frame < 8; ++frame) {
        while (scheduled.load(std::memory_order_acquire) <= frame) {
          std::this_thread::yield();
        }
        CHECK(DeliverCompletion(frame, 0));
      }
    });

    for (int i = 0; i < 8; ++i) {
      CHECK(output.TrySchedule(PackedFrameView{bytes.data(), bytes.size()}, MakeInfo(static_cast<uint64_t>(i)), 1000,
                               1000) == ScheduleOutcome::Accepted);
      scheduled.store(i + 1, std::memory_order_release);
    }

    int received = 0;
    while (received < 8) {
      OutputEvent event;
      if (output.PollEvent(&event)) {
        ++received;
      } else {
        std::this_thread::yield();
      }
    }
    worker.join();
    CHECK(received == 8);
    CHECK(output.unreconciled_frame_count() == 0);
    CHECK(output.Close(0) == CloseStatus::Closed);
  }
}

void TestLateCallbackAfterBackendDestruction() {
  ResetFakeOutput();
  DeckLinkOutput *output = new DeckLinkOutput();
  ResolvedOutput resolved;
  std::string error;
  CHECK(output->Open(MakeRequest(), &resolved, &error));
  std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x7C);
  CHECK(output->TrySchedule(PackedFrameView{bytes.data(), bytes.size()}, MakeInfo(1), 1000, 1000) ==
        ScheduleOutcome::Accepted);
  eyeq_decklink_test::CaptureRegisteredCallbackForLateDelivery();
  CHECK(output->Close(0) == CloseStatus::Closed);
  delete output;
  // Deliver from a worker after the backend is gone; CallbackState is kept alive by
  // the captured reference and the callback only observes receiving=false.
  std::thread worker([] { CHECK(eyeq_decklink_test::DeliverLateCompletion(0, 0)); });
  worker.join();
}

// Deterministic Close/callback interleaving: the completion callback is held in
// flight (with its reference already held by the fake) while Close tears the session
// down. Close must not block on the callback, the backend must not be used after
// release, and the resumed callback must observe receiving=false.
struct CallbackGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
};

void GateHook(void *context) {
  auto *gate = static_cast<CallbackGate *>(context);
  std::unique_lock<std::mutex> lock(gate->mutex);
  gate->entered = true;
  gate->cv.notify_all();
  gate->cv.wait(lock, [gate] { return gate->release; });
}

void TestCloseWhileCallbackInFlight() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x8D);
    CHECK(output.TrySchedule(PackedFrameView{bytes.data(), bytes.size()}, MakeInfo(1), 1000, 1000) ==
          ScheduleOutcome::Accepted);
    CaptureRegisteredCallbackForLateDelivery();

    CallbackGate gate;
    SetCompletionGate(GateHook, &gate);
    std::thread worker([&] { CHECK(DeliverCompletion(0, 0)); });
    {
      std::unique_lock<std::mutex> lock(gate.mutex);
      gate.cv.wait(lock, [&gate] { return gate.entered; });
    }
    // The callback is in flight; Close must complete without waiting for it.
    CHECK(output.Close(0) == CloseStatus::Closed);
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.release = true;
    }
    gate.cv.notify_all();
    worker.join();
    SetCompletionGate(nullptr, nullptr);
  }
  ResetFakeOutput();
}

} // namespace

int main() {
  TestConcurrentCompletionDelivery();
  TestConcurrentDeliveryDuringScheduling();
  TestLateCallbackAfterBackendDestruction();
  TestCloseWhileCallbackInFlight();
  std::printf("decklink output concurrency test: OK\n");
  return 0;
}
