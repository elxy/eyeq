// CPU contract tests for the phase 0.5 public headers. No SDK, no GPU, no device.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "fake_frame_sink.hpp"
#include "output/frame_sink.hpp"
#include "output/output_profile.hpp"
#include "output/pixel_format.hpp"

using namespace EYEQ;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

struct StagingSlot {
  std::vector<uint8_t> bytes;
  bool ready = false;
  int reap_count = 0;
};

void Reap(StagingSlot &slot) {
  CHECK(slot.ready);
  ++slot.reap_count;
  slot.ready = false;
}

PackedFrameView View(const StagingSlot &slot) { return PackedFrameView{slot.bytes.data(), slot.bytes.size()}; }

// Drives one output item through the abstract sink. Retries the SAME submission on Busy.
ScheduleOutcome SubmitWithRetry(FrameSink &sink, StagingSlot &slot, const SubmissionInfo &info, int64_t display_time,
                                int64_t display_duration) {
  CHECK(slot.ready);
  for (int attempt = 0; attempt < 8; ++attempt) {
    const ScheduleOutcome outcome = sink.TrySchedule(View(slot), info, display_time, display_duration);
    if (outcome == ScheduleOutcome::Busy) {
      CHECK(slot.ready); // Busy must not consume or reap
      continue;
    }
    Reap(slot); // accepted or terminating outcome consumes the bytes
    return outcome;
  }
  return ScheduleOutcome::Busy;
}

OutputRequest BaseRequest() {
  OutputRequest request;
  request.mode = MonitorMode{3840, 2160, 1000, 50000, true};
  request.connection = VideoConnection::SDI;
  request.pixel_format = PixelFormat::V210;
  return request; // default color (BT709/SDR) and default hdr are coherent
}

OutputRequest PqRequest() {
  OutputRequest request = BaseRequest();
  request.color.encoding =
      ColorEncoding{ColorPrimaries::BT2020, ColorTransfer::PQ, ColorRange::Video, ChromaLocation::Left};
  request.hdr = StaticHdrMetadata{};
  request.hdr.hdr_signaling = true;
  request.hdr.primaries = ColorPrimaries::BT2020;
  request.hdr.transfer = ColorTransfer::PQ;
  return request;
}

// ---------------------------------------------------------------------------
void TestMinimalRowBytes() {
  CHECK(MinimalRowBytes(PixelFormat::V210, 3840) == 10240);
  CHECK(MinimalRowBytes(PixelFormat::R210, 3840) == 15360);
  CHECK(MinimalRowBytes(PixelFormat::V210, 1920) == 5120);
  CHECK(MinimalRowBytes(PixelFormat::R210, 1920) == 7680);
  CHECK(MinimalRowBytes(PixelFormat::V210, 6) == 16);
  CHECK(MinimalRowBytes(PixelFormat::V210, 7) == 32);
  CHECK(MinimalRowBytes(PixelFormat::V210, 0) == 0);
  CHECK(MinimalRowBytes(PixelFormat::R210, -1) == 0);
  // Unknown pixel format is invalid (0), never a silently accepted zero stride.
  CHECK(MinimalRowBytes(static_cast<PixelFormat>(999), 3840) == 0);
  // Out-of-int-range minima report invalid (0) instead of saturating to INT_MAX.
  CHECK(MinimalRowBytes(PixelFormat::R210, std::numeric_limits<int>::max()) == 0);
  CHECK(MinimalRowBytes(PixelFormat::V210, std::numeric_limits<int>::max()) == 0);
  // A large but representable width still returns a positive, word-aligned minimum.
  const int large = MinimalRowBytes(PixelFormat::R210, 100000000);
  CHECK(large > 0 && large % 4 == 0);
}

void TestValidateLayout() {
  std::string error;

  BufferLayout v210{3840, 2160, PixelFormat::V210, 10240, static_cast<std::size_t>(10240) * 2160};
  CHECK(ValidateLayout(v210, v210.image_bytes, &error));
  CHECK(ValidateLayout(v210, v210.image_bytes + 4096, &error)); // larger capacity is valid
  CHECK(!ValidateLayout(v210, v210.image_bytes - 1, &error));   // capacity too small
  CHECK(!error.empty());

  BufferLayout r210{3840, 2160, PixelFormat::R210, 15360, static_cast<std::size_t>(15360) * 2160};
  CHECK(ValidateLayout(r210, r210.image_bytes, &error));

  // Below the format minimum.
  BufferLayout undersized = v210;
  undersized.row_bytes = 10239;
  undersized.image_bytes = static_cast<std::size_t>(undersized.row_bytes) * undersized.height;
  CHECK(!ValidateLayout(undersized, undersized.image_bytes, &error));

  // Not 32-bit word aligned.
  BufferLayout misaligned = v210;
  misaligned.row_bytes = 10241;
  misaligned.image_bytes = static_cast<std::size_t>(misaligned.row_bytes) * misaligned.height;
  CHECK(!ValidateLayout(misaligned, misaligned.image_bytes, &error));

  // image_bytes inconsistent with row_bytes * height.
  BufferLayout inconsistent = v210;
  inconsistent.image_bytes = v210.image_bytes - 1;
  CHECK(!ValidateLayout(inconsistent, v210.image_bytes, &error));

  // Non-positive dimensions.
  BufferLayout zero_width = v210;
  zero_width.width = 0;
  CHECK(!ValidateLayout(zero_width, v210.image_bytes, &error));
  BufferLayout zero_height = v210;
  zero_height.height = 0;
  CHECK(!ValidateLayout(zero_height, v210.image_bytes, &error));

  // Unknown pixel format with zero stride must be rejected even with zero capacity.
  BufferLayout unknown{3840, 2160, static_cast<PixelFormat>(999), 0, 0};
  CHECK(!ValidateLayout(unknown, 0, &error));
  CHECK(error.find("unknown pixel format") != std::string::npos);

  // Zero stride with a known format is below the minimum and rejected.
  BufferLayout zero_stride = v210;
  zero_stride.row_bytes = 0;
  zero_stride.image_bytes = 0;
  CHECK(!ValidateLayout(zero_stride, 0, &error));
}

void TestCheckedMultiplyOverflow() {
  std::string error;
  // With int row_bytes and int height, row_bytes * height cannot exceed 64-bit
  // size_t on this platform, so the overflow guard is only reachable where
  // size_t is 32-bit. This branch is reported as such; it is not a silent skip of
  // a reachable case. The stride is word-aligned so the overflow check, not the
  // alignment check, is what rejects it.
  if (sizeof(std::size_t) < 8) {
    const int aligned_max = (std::numeric_limits<int>::max() / 4) * 4;
    BufferLayout huge{6, 3, PixelFormat::V210, aligned_max, 0};
    CHECK(aligned_max % 4 == 0 && aligned_max >= MinimalRowBytes(PixelFormat::V210, 6));
    CHECK(!ValidateLayout(huge, std::numeric_limits<std::size_t>::max(), &error));
    CHECK(error.find("overflow") != std::string::npos);
  }
  // A large but representable layout must still pass the multiply and capacity check.
  BufferLayout big{6, 2, PixelFormat::V210, 1 << 30, static_cast<std::size_t>(1 << 30) * 2};
  CHECK(ValidateLayout(big, big.image_bytes, &error));
}

void TestValidateRequest() {
  std::string error;

  CHECK(ValidateRequest(BaseRequest(), &error)); // BT709 / BT1886 / SDR
  CHECK(ValidateRequest(PqRequest(), &error));   // BT2020 / PQ / HDR, unknown mastering/CLL

  {
    OutputRequest request = PqRequest();
    request.color.encoding.primaries = ColorPrimaries::BT709; // PQ pixels + BT709 primaries
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest(); // SDR pixels
    request.hdr.hdr_signaling = true;
    request.hdr.transfer = ColorTransfer::PQ;
    request.hdr.primaries = ColorPrimaries::BT2020;
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.hdr_signaling = false; // PQ pixels without HDR signaling
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.color.encoding.transfer = ColorTransfer::PQ; // unsupported BT709 + PQ
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.color.encoding.range = ColorRange::Full;
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.color.encoding.chroma_loc = ChromaLocation::Center;
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.mode.is_progressive = false;
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.color.target_nits = -1.0;
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.mastering.has_primaries = true;
    request.hdr.mastering.red_x = 1.5; // out of [0, 1]
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.mastering.has_luminance = true;
    request.hdr.mastering.min_nits = 0.5;
    request.hdr.mastering.max_nits = 0.1; // min >= max
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.mastering.has_luminance = true;
    request.hdr.mastering.min_nits = 0.00001; // below SDK range
    request.hdr.mastering.max_nits = 1000.0;
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.cll.has_values = true;
    request.hdr.cll.max_cll = 100.0;
    request.hdr.cll.max_fall = 200.0; // fall > cll
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest(); // valid PQ with known mastering/CLL within range
    request.hdr.mastering.has_luminance = true;
    request.hdr.mastering.min_nits = 0.005;
    request.hdr.mastering.max_nits = 1000.0;
    request.hdr.cll.has_values = true;
    request.hdr.cll.max_cll = 1000.0;
    request.hdr.cll.max_fall = 400.0;
    CHECK(ValidateRequest(request, &error));
  }
  // Unknown enum values are rejected, not treated as a supported mode.
  {
    OutputRequest request = BaseRequest();
    request.pixel_format = static_cast<PixelFormat>(999);
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.connection = static_cast<VideoConnection>(999);
    CHECK(!ValidateRequest(request, &error));
  }
  // Non-finite colour fields are rejected.
  {
    OutputRequest request = BaseRequest();
    request.color.target_nits = std::numeric_limits<double>::quiet_NaN();
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = BaseRequest();
    request.color.target_nits = std::numeric_limits<double>::infinity();
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.mastering.has_primaries = true;
    request.hdr.mastering.red_x = std::numeric_limits<double>::quiet_NaN();
    CHECK(!ValidateRequest(request, &error));
  }
  {
    OutputRequest request = PqRequest();
    request.hdr.cll.has_values = true;
    request.hdr.cll.max_cll = std::numeric_limits<double>::infinity();
    request.hdr.cll.max_fall = 0.0;
    CHECK(!ValidateRequest(request, &error));
  }
}

void TestToPackConfig() {
  ResolvedOutput resolved;
  resolved.mode = MonitorMode{3840, 2160, 1000, 50000, true};
  resolved.pixel_format = PixelFormat::V210;
  resolved.layout = BufferLayout{3840, 2160, PixelFormat::V210, 10240, static_cast<std::size_t>(10240) * 2160};
  resolved.color.encoding =
      ColorEncoding{ColorPrimaries::BT2020, ColorTransfer::PQ, ColorRange::Video, ChromaLocation::Left};
  resolved.color.target_nits = 600.0;

  const PackConfig config = ToPackConfig(resolved);
  CHECK(config.layout.width == 3840 && config.layout.height == 2160);
  CHECK(config.layout.row_bytes == 10240 && config.layout.image_bytes == resolved.layout.image_bytes);
  CHECK(config.output.primaries == ColorPrimaries::BT2020 && config.output.transfer == ColorTransfer::PQ);
  CHECK(config.output.range == ColorRange::Video && config.output.chroma_loc == ChromaLocation::Left);
}

// Fixture A: Busy / retry. No Close is called in the middle.
void TestFixtureBusyRetry() {
  test::FakeFrameSink sink(/*pool_slots=*/1);
  FrameSink &api = sink;
  std::string error;

  CHECK(api.StartScheduled(0, &error));

  const SubmissionInfo info1{1, 1, 7, 1000, 555};
  StagingSlot slot1;
  slot1.bytes.assign(16, 0x11);
  slot1.ready = true;
  CHECK(SubmitWithRetry(api, slot1, info1, 0, 1000) == ScheduleOutcome::Accepted);
  CHECK(sink.return_count(info1.submit_id) == 0); // completion not consumed yet

  const SubmissionInfo info2{2, 1, 7, 1001, 556};
  StagingSlot slot2;
  slot2.bytes.assign(16, 0x22);
  slot2.ready = true;
  CHECK(api.TrySchedule(View(slot2), info2, 1000, 1000) == ScheduleOutcome::Busy);
  CHECK(sink.busy_count() == 1);
  CHECK(slot2.ready && slot2.reap_count == 0);
  CHECK(!sink.has_copy(info2.submit_id));
  CHECK(sink.accepted_count() == 1);

  OutputEvent event{};
  CHECK(api.PollEvent(&event));
  CHECK(event.kind == OutputEvent::Kind::FrameCompleted && event.submit_id == info1.submit_id);
  CHECK(event.output_index == info1.output_index && event.source_frame_serial == info1.source_frame_serial);
  CHECK(sink.return_count(info1.submit_id) == 1);

  CHECK(SubmitWithRetry(api, slot2, info2, 1000, 1000) == ScheduleOutcome::Accepted);
  CHECK(sink.return_count(info2.submit_id) == 0);
  const SubmissionInfo &echo = sink.last_info();
  CHECK(echo.submit_id == info2.submit_id && echo.batch_seq == info2.batch_seq &&
        echo.timeline_epoch == info2.timeline_epoch && echo.output_index == info2.output_index &&
        echo.source_frame_serial == info2.source_frame_serial);

  CHECK(api.PollEvent(&event));
  CHECK(event.submit_id == info2.submit_id && event.output_index == info2.output_index);
  CHECK(sink.return_count(info2.submit_id) == 1);

  CHECK(api.StopScheduled(0, nullptr, &error));
  CHECK(api.Close(1000) == CloseStatus::Closed);
  CHECK(sink.state() == test::FakeFrameSink::State::Closed);
}

// Fixture B: Close / drain. Draining rejects new work, still drains events, then closes.
void TestFixtureCloseDrain() {
  test::FakeFrameSink sink(/*pool_slots=*/1);
  FrameSink &api = sink;
  std::string error;

  CHECK(api.StartScheduled(0, &error));

  const SubmissionInfo info1{10, 1, 3, 0, 99};
  StagingSlot slot1;
  slot1.bytes.assign(16, 0xAA);
  slot1.ready = true;
  CHECK(SubmitWithRetry(api, slot1, info1, 0, 1000) == ScheduleOutcome::Accepted);
  CHECK(sink.free_slots() == 0);

  // Close with an outstanding frame -> Draining / DrainTimeout. No state reset.
  CHECK(api.Close(1000) == CloseStatus::DrainTimeout);
  CHECK(sink.state() == test::FakeFrameSink::State::Draining);

  // New work is rejected without consuming a device frame.
  const int free_before = sink.free_slots();
  const SubmissionInfo info2{11, 1, 3, 1, 100};
  StagingSlot slot2;
  slot2.bytes.assign(16, 0xBB);
  slot2.ready = true;
  CHECK(api.TrySchedule(View(slot2), info2, 1000, 1000) == ScheduleOutcome::Closed);
  CHECK(sink.free_slots() == free_before);
  CHECK(!sink.has_copy(info2.submit_id));
  // The caller still owns its staging slot: it was never handed to the sink.
  CHECK(slot2.ready && slot2.reap_count == 0);
  CHECK(api.DisplayStill(View(slot2), info2) == StillOutcome::Closed);
  CHECK(!api.StartScheduled(0, &error));

  // Outstanding events can still be consumed, returning the budget.
  OutputEvent event{};
  CHECK(api.PollEvent(&event));
  CHECK(event.submit_id == info1.submit_id);
  CHECK(sink.return_count(info1.submit_id) == 1);
  CHECK(sink.free_slots() == sink.pool_slots());

  // Now Close succeeds.
  CHECK(api.Close(1000) == CloseStatus::Closed);
  CHECK(sink.state() == test::FakeFrameSink::State::Closed);
}

// Byte ownership, identity echo, rejection and failure accounting.
void TestByteOwnershipAndFailure() {
  {
    test::FakeFrameSink sink(/*pool_slots=*/2);
    FrameSink &api = sink;
    StagingSlot slot;
    slot.bytes = {1, 2, 3, 4};
    slot.ready = true;
    const SubmissionInfo info{20, 2, 5, 42, 4242};
    CHECK(SubmitWithRetry(api, slot, info, 0, 1000) == ScheduleOutcome::Accepted);
    const std::vector<uint8_t> *copy = sink.copy(info.submit_id);
    CHECK(copy != nullptr && copy->size() == 4);
    const uint8_t first = (*copy)[0];
    slot.bytes[0] = 0xFF; // overwrite staging after acceptance
    CHECK((*sink.copy(info.submit_id))[0] == first);

    OutputEvent event{};
    CHECK(api.PollEvent(&event));
    CHECK(event.submit_id == info.submit_id && event.batch_seq == info.batch_seq);

    // Empty view is rejected without consuming.
    CHECK(api.TrySchedule(PackedFrameView{nullptr, 0}, SubmissionInfo{21, 0, 0, 0, 0}, 0, 1000) ==
          ScheduleOutcome::Rejected);
  }
  {
    test::FakeFrameSink sink(/*pool_slots=*/1);
    FrameSink &api = sink;
    StagingSlot slot;
    slot.bytes.assign(16, 0x33);
    slot.ready = true;
    const SubmissionInfo info{30, 1, 1, 0, 1};
    sink.fail_next_before_consumption();
    CHECK(SubmitWithRetry(api, slot, info, 0, 1000) == ScheduleOutcome::DeviceError);
    CHECK(slot.reap_count == 1);
    CHECK(sink.return_count(info.submit_id) == 0);
    CHECK(sink.accepted_count() == 0);
    CHECK(sink.free_slots() == sink.pool_slots());
    CHECK(api.Close(1000) == CloseStatus::Closed);
  }
}

// Still-frame contract: only valid while scheduled playback is stopped.
void TestDisplayStill() {
  // Stopped: an independent copy is retained, no pool budget is consumed, and no
  // scheduled completion event is produced.
  {
    test::FakeFrameSink sink(/*pool_slots=*/1);
    FrameSink &api = sink;
    StagingSlot slot;
    slot.bytes = {7, 8, 9};
    slot.ready = true;
    const SubmissionInfo info{40, 1, 1, 0, 7};

    CHECK(api.DisplayStill(View(slot), info) == StillOutcome::Displayed);
    CHECK(sink.still_display_count() == 1);
    CHECK(sink.free_slots() == sink.pool_slots()); // no video frame budget consumed
    const std::vector<uint8_t> *copy = sink.copy(info.submit_id);
    CHECK(copy != nullptr && copy->size() == 3);
    const uint8_t first = (*copy)[0];
    slot.bytes[0] = 0xFF; // overwrite staging after the still copy
    CHECK((*sink.copy(info.submit_id))[0] == first);

    OutputEvent event{};
    CHECK(!api.PollEvent(&event)); // still frames do not produce completion events
    CHECK(sink.state() == test::FakeFrameSink::State::Running);
    CHECK(api.Close(1000) == CloseStatus::Closed);
  }

  // Scheduled playback running: the still call must be refused.
  {
    test::FakeFrameSink sink(/*pool_slots=*/1);
    FrameSink &api = sink;
    std::string error;
    CHECK(api.StartScheduled(0, &error));
    StagingSlot slot;
    slot.bytes = {1, 2, 3};
    slot.ready = true;
    const SubmissionInfo info{41, 1, 1, 0, 8};
    CHECK(api.DisplayStill(View(slot), info) == StillOutcome::DeviceError);
    CHECK(!sink.has_copy(info.submit_id));
    CHECK(sink.still_display_count() == 0);
  }

  // Draining / Closed: refused and no bytes copied.
  {
    test::FakeFrameSink sink(/*pool_slots=*/1);
    FrameSink &api = sink;
    StagingSlot slot;
    slot.bytes = {1, 2, 3};
    slot.ready = true;
    const SubmissionInfo info{42, 1, 1, 0, 9};
    // Keep one device frame outstanding so Close enters Draining.
    StagingSlot scheduled;
    scheduled.bytes.assign(16, 0x55);
    scheduled.ready = true;
    CHECK(SubmitWithRetry(api, scheduled, SubmissionInfo{43, 1, 1, 0, 10}, 0, 1000) == ScheduleOutcome::Accepted);
    CHECK(api.Close(1000) == CloseStatus::DrainTimeout);
    CHECK(api.DisplayStill(View(slot), info) == StillOutcome::Closed);
    CHECK(!sink.has_copy(info.submit_id));

    OutputEvent event{};
    CHECK(api.PollEvent(&event));
    CHECK(api.Close(1000) == CloseStatus::Closed);
    CHECK(api.DisplayStill(View(slot), info) == StillOutcome::Closed);
    CHECK(!sink.has_copy(info.submit_id));
  }
}

} // namespace

int main() {
  TestMinimalRowBytes();
  TestValidateLayout();
  TestCheckedMultiplyOverflow();
  TestValidateRequest();
  TestToPackConfig();
  TestFixtureBusyRetry();
  TestFixtureCloseDrain();
  TestByteOwnershipAndFailure();
  TestDisplayStill();
  std::printf("decklink-contract-test: OK\n");
  return 0;
}
