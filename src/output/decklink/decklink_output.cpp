// SDK-facing implementation of the DeckLink output backend: default SDK frame
// pool, CPU access/copy, scheduling callback, still frame, clock/start/stop and
// safe shutdown.
//
// Threading model (phase 1A-2): every FrameSink method is called from one consumer
// thread that owns the frame pool, the slot state machine and all resource
// reclamation. The SDK completion callback runs on a separate callback thread and
// only takes a short std::mutex to snapshot a submission identity into a fixed
// event ring. No SDK call, memcpy, GPU work, wait or large allocation happens under
// that mutex, so native synchronous reentrancy cannot deadlock. The mutex is not
// claimed to be lock-free or real-time; lock cost is measured later.
#include "output/decklink_backend.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DeckLinkAPI.h"
#include "output/decklink/decklink_sdk.hpp"
#include "output/decklink/monitor_mode.hpp"

namespace EYEQ {

namespace {

constexpr int kPoolDepth = 8;
constexpr std::size_t kEventCapacity = 128;
constexpr std::size_t kRetainedEventCapacity = 256;
constexpr int kDiagnosticMessageBytes = 192;

enum class Lifecycle { Closed, Running, Failed };
enum class PlaybackState { Idle, Running, Stopping };
enum class SlotState { Free, Submitting, Scheduled, SubmitFailed, SettledUnknown, Quarantined };

// Published before the native schedule call so a synchronous callback can snapshot
// the submission identity. Protected by CallbackState::mutex.
struct SubmissionRecord {
  bool active = false;
  uint64_t generation = 0;
  SubmissionInfo info;
  int64_t display_time = 0;
};

// One completion or playback-stopped notification, carrying the full submission
// identity captured at callback time (not re-read when consumed).
struct CompletionRecord {
  enum class Kind { FrameCompleted, PlaybackStopped };
  Kind kind = Kind::FrameCompleted;
  int slot = -1;
  uint64_t generation = 0;
  SubmissionInfo info;
  int64_t display_time = 0;
  BMDOutputFrameCompletionResult result = bmdOutputFrameCompleted;
};

// Callback-visible state. Created per Open; shared between the backend and the COM
// callback object. The mutex protects only the fixed submission records and the
// fixed event ring; the callback never touches the frame pool or the backend.
class CallbackState {
public:
  std::mutex mutex;
  SubmissionRecord records[kPoolDepth];
  IDeckLinkVideoFrame *frames[kPoolDepth] = {};
  int frame_count = 0;
  CompletionRecord events[kEventCapacity];
  std::size_t event_head = 0;
  std::size_t event_count = 0;
  bool receiving = true;
  bool overflow = false;
  bool overflow_handled = false;
  uint64_t lost_events = 0;
  uint64_t anomaly_count = 0;

  int FindSlot(IDeckLinkVideoFrame *frame) const {
    for (int i = 0; i < frame_count; ++i) {
      if (frames[i] == frame) {
        return i;
      }
    }
    return -1;
  }

  bool PushLocked(const CompletionRecord &record) {
    if (event_count == kEventCapacity) {
      overflow = true;
      ++lost_events;
      return false;
    }
    events[(event_head + event_count) % kEventCapacity] = record;
    ++event_count;
    return true;
  }

  bool PopLocked(CompletionRecord *out) {
    if (event_count == 0) {
      return false;
    }
    *out = events[event_head];
    event_head = (event_head + 1) % kEventCapacity;
    --event_count;
    return true;
  }
};

// Minimal COM RAII holder: releases on destruction. Used for the callback object
// and the SDK interfaces owned by the backend.
template <typename T> class ComPtr {
public:
  ComPtr() = default;
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr &) = delete;
  ComPtr &operator=(const ComPtr &) = delete;

  T *get() const { return pointer_; }
  T *operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }

  void reset(T *pointer = nullptr) {
    if (pointer_ != nullptr) {
      pointer_->Release();
    }
    pointer_ = pointer;
  }

  T **releaseAndGetAddressOf() {
    reset();
    return &pointer_;
  }

private:
  T *pointer_ = nullptr;
};

// COM callback object. It uses intrusive COM reference counting only; the backend
// holds it through ComPtr. It holds a shared_ptr to CallbackState for lifetime, but
// that shared_ptr is not used for synchronization.
class CallbackObject final : public IDeckLinkVideoOutputCallback {
public:
  explicit CallbackObject(std::shared_ptr<CallbackState> state) : state_(std::move(state)) {}

  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkVideoOutputCallback, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkVideoOutputCallback *>(this);
      AddRef();
      return S_OK;
    }
    const CFUUIDBytes unknown = CFUUIDGetUUIDBytes(IUnknownUUID);
    if (memcmp(&iid, &unknown, sizeof(iid)) == 0) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  ULONG AddRef() override { return ++references_; }

  ULONG Release() override {
    const ULONG count = --references_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame *completed_frame,
                                  BMDOutputFrameCompletionResult result) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->receiving) {
      return S_OK;
    }
    CompletionRecord record;
    record.kind = CompletionRecord::Kind::FrameCompleted;
    record.result = result;
    record.slot = state_->FindSlot(completed_frame);
    if (record.slot >= 0) {
      const SubmissionRecord &published = state_->records[record.slot];
      record.generation = published.generation;
      record.info = published.info;
      record.display_time = published.display_time;
    } else {
      ++state_->anomaly_count;
    }
    state_->PushLocked(record);
    return S_OK;
  }

  HRESULT ScheduledPlaybackHasStopped() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->receiving) {
      return S_OK;
    }
    CompletionRecord record;
    record.kind = CompletionRecord::Kind::PlaybackStopped;
    state_->PushLocked(record);
    return S_OK;
  }

private:
  ~CallbackObject() = default;

  std::atomic<ULONG> references_{1};
  std::shared_ptr<CallbackState> state_;
};

struct FrameHandles {
  IDeckLinkMutableVideoFrame *frame = nullptr;
  IDeckLinkVideoBuffer *buffer = nullptr;
  IDeckLinkVideoFrameMutableMetadataExtensions *metadata = nullptr;
};

enum class WriteResult { Ok, AccessFailed, EndAccessFailed, MetadataFailed };

// Outcome of the CPU copy + metadata stage. It carries the failing stage and the
// real HRESULT so TrySchedule/DisplayStill can report a fully identified error
// instead of a generic Access/0.
struct WriteOutcome {
  WriteResult result = WriteResult::Ok;
  OutputEvent::Stage stage = OutputEvent::Stage::None;
  int32_t status = 0;
  std::string message;
};

bool IsKnownCompletionResult(BMDOutputFrameCompletionResult result) {
  switch (result) {
  case bmdOutputFrameCompleted:
  case bmdOutputFrameDisplayedLate:
  case bmdOutputFrameDropped:
  case bmdOutputFrameFlushed:
    return true;
  }
  return false;
}

CompletionResult MapCompletionResult(BMDOutputFrameCompletionResult result) {
  switch (result) {
  case bmdOutputFrameDisplayedLate:
    return CompletionResult::DisplayedLate;
  case bmdOutputFrameDropped:
    return CompletionResult::Dropped;
  case bmdOutputFrameFlushed:
    return CompletionResult::Flushed;
  case bmdOutputFrameCompleted:
    return CompletionResult::Completed;
  }
  return CompletionResult::Completed;
}

int64_t SdkColorspace(ColorPrimaries primaries) {
  return primaries == ColorPrimaries::BT2020 ? static_cast<int64_t>(bmdColorspaceRec2020)
                                             : static_cast<int64_t>(bmdColorspaceRec709);
}

// Bounded copy into a fixed diagnostic buffer. Only called on the consumer thread,
// never under the callback mutex.
void CopyMessage(char *destination, std::size_t capacity, const char *message) {
  if (capacity == 0) {
    return;
  }
  const std::size_t length = std::min(std::strlen(message), capacity - 1);
  std::memcpy(destination, message, length);
  destination[length] = '\0';
}

} // namespace

struct DeckLinkOutput::Impl {
  // Consumer-thread-only pool slot.
  struct Slot {
    IDeckLinkMutableVideoFrame *frame = nullptr;
    IDeckLinkVideoBuffer *buffer = nullptr;
    IDeckLinkVideoFrameMutableMetadataExtensions *metadata = nullptr;
    SlotState state = SlotState::Free;
    uint64_t generation = 0;
    SubmissionInfo info;
    int64_t display_time = 0;
  };

  std::shared_ptr<CallbackState> state;
  ComPtr<CallbackObject> callback;
  ComPtr<IDeckLink> device;
  ComPtr<IDeckLinkOutput> output;
  FrameHandles still;
  SlotState still_state = SlotState::Free;
  Slot slots[kPoolDepth];
  int frame_count = 0;
  ResolvedOutput resolved;
  Lifecycle lifecycle = Lifecycle::Closed;
  PlaybackState playback = PlaybackState::Idle;
  bool enabled = false;
  bool callback_registered = false;
  bool closing = false;
  bool fatal = false;
  uint64_t generation_counter = 0;
  int64_t last_actual_stop = 0;
  std::deque<OutputEvent> retained_events; // consumer-thread-only, readable after Close
  uint32_t retained_dropped = 0;

  // Consumer-thread error/diagnostic state. It lives on Impl, not on CallbackState,
  // because a session can fail before CallbackState exists (for example an enable
  // followed by a connection-query failure) and the failure must still be reported.
  // No mutex is needed: the callback thread never reads or writes these fields.
  uint64_t error_count = 0;
  bool error_recorded = false;
  bool error_reported = false;
  OutputEvent::Stage error_stage = OutputEvent::Stage::None;
  int32_t error_status = 0;
  SubmissionInfo error_info;
  int64_t error_display_time = 0;
  char error_message[kDiagnosticMessageBytes] = {};
  bool callback_overflow_seen = false;
  OutputEvent retained_overflow_summary;
  bool retained_overflow_pending = false;

  // Consumer-side diagnostic event build. Used both for non-fatal rejections and for
  // later failures that must not be swallowed by the first-error snapshot.
  void EmitDiagnostic(OutputEvent::Stage stage, int32_t status, const char *message,
                      const SubmissionInfo &info = SubmissionInfo(), int64_t display_time = 0) {
    OutputEvent event;
    event.kind = OutputEvent::Kind::Error;
    event.stage = stage;
    event.os_status = status;
    event.submit_id = info.submit_id;
    event.source_frame_serial = info.source_frame_serial;
    event.display_time = display_time;
    CopyMessage(event.message, sizeof(event.message), message);
    RetainEvent(event);
  }

  // Records a failure. The first failure is snapshotted so a terminal diagnostic can
  // be surfaced exactly once; every later failure is queued on the same bounded
  // diagnostic channel instead of being silently ignored.
  void RecordError(OutputEvent::Stage stage, int32_t status, const char *message,
                   const SubmissionInfo &info = SubmissionInfo(), int64_t display_time = 0) {
    if (error_recorded) {
      EmitDiagnostic(stage, status, message, info, display_time);
      return;
    }
    error_recorded = true;
    error_stage = stage;
    error_status = status;
    error_info = info;
    error_display_time = display_time;
    CopyMessage(error_message, sizeof(error_message), message);
    ++error_count;
  }

  // Builds a fully initialized error event; never leaves fields from a previous use
  // of the caller's out parameter.
  bool SurfaceError(OutputEvent *out) {
    if (!error_recorded || error_reported) {
      return false;
    }
    error_reported = true;
    OutputEvent event;
    event.kind = OutputEvent::Kind::Error;
    event.stage = error_stage;
    event.os_status = error_status;
    event.submit_id = error_info.submit_id;
    event.source_frame_serial = error_info.source_frame_serial;
    event.display_time = error_display_time;
    CopyMessage(event.message, sizeof(event.message), error_message);
    if (out != nullptr) {
      *out = event;
    }
    return true;
  }

  // Refreshes the dedicated overflow-summary slot. It is deliberately kept outside
  // retained_events so the notice about dropped diagnostics can never itself be
  // dropped by a full queue.
  void RefreshRetainedOverflowSummary() {
    OutputEvent event;
    event.kind = OutputEvent::Kind::Error;
    event.stage = OutputEvent::Stage::None;
    std::snprintf(event.message, sizeof(event.message), "retained diagnostic queue overflowed; %u event(s) dropped",
                  static_cast<unsigned>(retained_dropped));
    retained_overflow_summary = event;
    retained_overflow_pending = true;
  }

  // Bounded consumer-side retention: keeps the newest events, drops the oldest and
  // records the drop total in a separate readable summary.
  void RetainEvent(const OutputEvent &event) {
    if (retained_events.size() >= kRetainedEventCapacity) {
      retained_events.pop_front();
      ++retained_dropped;
      RefreshRetainedOverflowSummary();
    }
    retained_events.push_back(event);
  }

  // Moves a still-unreported error into the retained event queue before the shared
  // CallbackState is released, so it stays readable through PollEvent after Close.
  void RetainPendingError() {
    OutputEvent event;
    if (!SurfaceError(&event)) {
      return;
    }
    RetainEvent(event);
  }

  // Reads the callback-side overflow sticky exactly once and turns it into a
  // recorded error, then stops new submissions. Consumer thread only; a missing
  // CallbackState simply means no callback can have overflowed yet.
  void CheckCallbackOverflow() {
    if (state == nullptr) {
      return;
    }
    bool newly_overflowed = false;
    uint64_t lost = 0;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->overflow && !state->overflow_handled) {
        state->overflow_handled = true;
        newly_overflowed = true;
        lost = state->lost_events;
      }
    }
    if (newly_overflowed) {
      callback_overflow_seen = true;
      char message[kDiagnosticMessageBytes];
      std::snprintf(message, sizeof(message), "callback event ring overflowed; %llu completion event(s) lost",
                    static_cast<unsigned long long>(lost));
      RecordError(OutputEvent::Stage::None, 0, message);
      fatal = true;
    }
  }

  int FindFreeSlot() const {
    for (int i = 0; i < frame_count; ++i) {
      if (slots[i].state == SlotState::Free) {
        return i;
      }
    }
    return -1;
  }

  uint32_t UnreconciledCount() const {
    uint32_t count = 0;
    for (int i = 0; i < frame_count; ++i) {
      if (slots[i].state != SlotState::Free) {
        ++count;
      }
    }
    return count;
  }

  void ClearRecord(int slot) {
    if (state == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->records[slot].active = false;
  }

  void UpdatePlaybackFromNative() {
    if (!enabled || !output) {
      return;
    }
    bool active = false;
    if (output->IsScheduledPlaybackRunning(&active) != S_OK) {
      return; // keep the current state; never fabricate Idle
    }
    playback = active ? PlaybackState::Running : PlaybackState::Idle;
  }

  // Applies frame flags and HDR metadata. On failure fills the failing stage, the
  // real HRESULT and a message into out. The SDK defines CLL/FALL as Float, so plain
  // double values are passed without integer truncation.
  bool ApplyFrameMetadata(const FrameHandles &handles, WriteOutcome *out) {
    BMDFrameFlags flags = bmdFrameFlagDefault;
    if (resolved.needs_vertical_flip) {
      flags |= bmdFrameFlagFlipVertical;
    }
    if (resolved.hdr.hdr_signaling) {
      flags |= bmdFrameContainsHDRMetadata;
    }
    const HRESULT set_flags = handles.frame->SetFlags(flags);
    if (set_flags != S_OK) {
      out->stage = OutputEvent::Stage::Metadata;
      out->status = static_cast<int32_t>(set_flags);
      out->message = "SetFlags failed";
      return false;
    }
    if (!resolved.hdr.hdr_signaling) {
      return true; // SDR: no frame metadata is sent
    }
    const HRESULT colorspace =
        handles.metadata->SetInt(bmdDeckLinkFrameMetadataColorspace, SdkColorspace(resolved.hdr.primaries));
    if (colorspace != S_OK) {
      out->stage = OutputEvent::Stage::Metadata;
      out->status = static_cast<int32_t>(colorspace);
      out->message = "metadata colorspace failed";
      return false;
    }
    // CTA-861.3: PQ = 2, traditional gamma SDR = 0.
    const HRESULT eotf = handles.metadata->SetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc, 2);
    if (eotf != S_OK) {
      out->stage = OutputEvent::Stage::Metadata;
      out->status = static_cast<int32_t>(eotf);
      out->message = "metadata EOTF failed";
      return false;
    }
    const MasteringDisplay &mastering = resolved.hdr.mastering;
    if (mastering.has_primaries) {
      const BMDDeckLinkFrameMetadataID ids[8] = {bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX,
                                                 bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY,
                                                 bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX,
                                                 bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY,
                                                 bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX,
                                                 bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY,
                                                 bmdDeckLinkFrameMetadataHDRWhitePointX,
                                                 bmdDeckLinkFrameMetadataHDRWhitePointY};
      const double values[8] = {mastering.red_x,  mastering.red_y,  mastering.green_x, mastering.green_y,
                                mastering.blue_x, mastering.blue_y, mastering.white_x, mastering.white_y};
      for (int i = 0; i < 8; ++i) {
        const HRESULT primaries = handles.metadata->SetFloat(ids[i], values[i]);
        if (primaries != S_OK) {
          out->stage = OutputEvent::Stage::Metadata;
          out->status = static_cast<int32_t>(primaries);
          out->message = "metadata mastering primaries failed";
          return false;
        }
      }
    }
    if (mastering.has_luminance) {
      const HRESULT max_luminance =
          handles.metadata->SetFloat(bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance, mastering.max_nits);
      const HRESULT min_luminance =
          handles.metadata->SetFloat(bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance, mastering.min_nits);
      if (max_luminance != S_OK || min_luminance != S_OK) {
        out->stage = OutputEvent::Stage::Metadata;
        out->status = static_cast<int32_t>(max_luminance != S_OK ? max_luminance : min_luminance);
        out->message = "metadata mastering luminance failed";
        return false;
      }
    }
    if (resolved.hdr.cll.has_values) {
      // DeckLink SDK 16.0 defines MaxCLL/MaxFALL as Float (see the official
      // SignalGenHDR sample), so they must not go through SetInt.
      const HRESULT content_light =
          handles.metadata->SetFloat(bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel, resolved.hdr.cll.max_cll);
      const HRESULT frame_average = handles.metadata->SetFloat(bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel,
                                                               resolved.hdr.cll.max_fall);
      if (content_light != S_OK || frame_average != S_OK) {
        out->stage = OutputEvent::Stage::Metadata;
        out->status = static_cast<int32_t>(content_light != S_OK ? content_light : frame_average);
        out->message = "metadata CLL failed";
        return false;
      }
    }
    return true;
  }

  WriteOutcome WriteFrame(const FrameHandles &handles, const PackedFrameView &view) {
    WriteOutcome outcome;
    const HRESULT start = handles.buffer->StartAccess(bmdBufferAccessWrite);
    if (start != S_OK) {
      outcome.result = WriteResult::AccessFailed;
      outcome.stage = OutputEvent::Stage::Access;
      outcome.status = static_cast<int32_t>(start);
      outcome.message = "StartAccess(write) failed";
      return outcome;
    }
    void *bytes = nullptr;
    const HRESULT get = handles.buffer->GetBytes(&bytes);
    if (get != S_OK || bytes == nullptr) {
      const HRESULT ended = handles.buffer->EndAccess(bmdBufferAccessWrite);
      outcome.stage = OutputEvent::Stage::Access;
      outcome.status = static_cast<int32_t>(get);
      outcome.message = "GetBytes failed";
      outcome.result = ended == S_OK ? WriteResult::AccessFailed : WriteResult::EndAccessFailed;
      return outcome;
    }
    std::memcpy(bytes, view.data, resolved.layout.image_bytes);
    const HRESULT end = handles.buffer->EndAccess(bmdBufferAccessWrite);
    if (end != S_OK) {
      outcome.result = WriteResult::EndAccessFailed;
      outcome.stage = OutputEvent::Stage::Access;
      outcome.status = static_cast<int32_t>(end);
      outcome.message = "EndAccess(write) failed";
      return outcome;
    }
    if (!ApplyFrameMetadata(handles, &outcome)) {
      outcome.result = WriteResult::MetadataFailed;
      return outcome;
    }
    return outcome;
  }

  // Builds one OutputEvent from a completion record, and reclaims the slot exactly
  // once when the record matches the current generation. An unconfirmable stop is
  // reported as an error instead of a fabricated stopped state, and an unknown
  // completion result is never mapped to Completed.
  void ProcessRecord(const CompletionRecord &record, OutputEvent *out, bool retain) {
    OutputEvent event;
    if (record.kind == CompletionRecord::Kind::PlaybackStopped) {
      bool active = false;
      const HRESULT running = output ? output->IsScheduledPlaybackRunning(&active) : E_FAIL;
      if (running != S_OK) {
        RecordError(OutputEvent::Stage::Stop, static_cast<int32_t>(running),
                    "could not confirm scheduled playback stopped");
        event.kind = OutputEvent::Kind::Error;
        event.stage = OutputEvent::Stage::Stop;
        CopyMessage(event.message, sizeof(event.message), "scheduled playback stop could not be confirmed");
      } else if (active) {
        RecordError(OutputEvent::Stage::Stop, 0, "PlaybackStopped notification while the scheduler is still active");
        event.kind = OutputEvent::Kind::Error;
        event.stage = OutputEvent::Stage::Stop;
        CopyMessage(event.message, sizeof(event.message), "scheduler still active after PlaybackStopped");
      } else {
        playback = PlaybackState::Idle;
        event.kind = OutputEvent::Kind::PlaybackStopped;
        event.actual_stop_time = last_actual_stop;
      }
    } else {
      event.submit_id = record.info.submit_id;
      event.source_frame_serial = record.info.source_frame_serial;
      event.display_time = record.display_time;

      const int slot = record.slot;
      const bool matches = slot >= 0 && slot < frame_count && slots[slot].generation == record.generation &&
                           (slots[slot].state == SlotState::Scheduled || slots[slot].state == SlotState::SubmitFailed);
      if (!matches) {
        // Stale or duplicate event: never reclaim a slot it does not own.
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          ++state->anomaly_count;
        }
        RecordError(OutputEvent::Stage::None, 0, "stale or duplicate frame completion ignored");
        event.kind = OutputEvent::Kind::Error;
        event.stage = OutputEvent::Stage::None;
      } else {
        slots[slot].state = SlotState::Free;
        ClearRecord(slot);
        if (IsKnownCompletionResult(record.result)) {
          event.kind = OutputEvent::Kind::FrameCompleted;
          event.result = MapCompletionResult(record.result);
        } else {
          // The frame is done (the callback fired) but the result code is unknown:
          // reclaim the slot and report an error rather than a fake Completed.
          RecordError(OutputEvent::Stage::Schedule, 0, "unknown SDK frame completion result");
          fatal = true;
          event.kind = OutputEvent::Kind::Error;
          event.stage = OutputEvent::Stage::Schedule;
          CopyMessage(event.message, sizeof(event.message), "unknown SDK frame completion result");
        }
      }
    }
    if (retain) {
      RetainEvent(event);
    } else if (out != nullptr) {
      *out = event;
    }
  }

  void DrainQueued(bool retain) {
    // A session can fail before CallbackState is created; then no callback can have
    // queued an event and there is nothing to drain.
    if (state == nullptr) {
      return;
    }
    while (true) {
      CompletionRecord record;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->PopLocked(&record)) {
          break;
        }
      }
      ProcessRecord(record, nullptr, retain);
    }
  }

  void DrainWithin(int drain_timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    do {
      DrainQueued(true);
      UpdatePlaybackFromNative();
      if (UnreconciledCount() == 0 && playback == PlaybackState::Idle) {
        break;
      }
      if (drain_timeout_ms <= 0) {
        break;
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
      if (elapsed.count() >= drain_timeout_ms) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
  }

  // Releases every owned resource. Iterates the fixed slot array, not frame_count, so
  // a pool that failed partway through creation still releases its earlier frames.
  void ReleaseFrames() {
    if (still.metadata != nullptr) {
      still.metadata->Release();
      still.metadata = nullptr;
    }
    if (still.buffer != nullptr) {
      still.buffer->Release();
      still.buffer = nullptr;
    }
    if (still.frame != nullptr) {
      still.frame->Release();
      still.frame = nullptr;
    }
    still_state = SlotState::Free;
    for (int i = 0; i < kPoolDepth; ++i) {
      if (slots[i].metadata != nullptr) {
        slots[i].metadata->Release();
        slots[i].metadata = nullptr;
      }
      if (slots[i].buffer != nullptr) {
        slots[i].buffer->Release();
        slots[i].buffer = nullptr;
      }
      if (slots[i].frame != nullptr) {
        slots[i].frame->Release();
        slots[i].frame = nullptr;
      }
      slots[i].state = SlotState::Free;
    }
    frame_count = 0;
  }

  // Releases the resources this session acquired, in shutdown order. Each ownership
  // flag is cleared only after the matching native call succeeds: a failed Disable or
  // callback unregister leaves the resource and its flag intact so the caller can
  // retry instead of pretending the session was cleaned up. Returns false as soon as
  // a native call fails; nothing beyond that point is touched.
  bool ReleaseSession() {
    if (still_state == SlotState::Quarantined) {
      return false; // CPU access state unknown: never release the still buffer
    }
    // Only disable output this session successfully enabled.
    if (enabled && output) {
      if (output->DisableVideoOutput() != S_OK) {
        RecordError(OutputEvent::Stage::Close, 0, "DisableVideoOutput failed");
        return false;
      }
      enabled = false;
    }
    if (callback_registered && output) {
      if (output->SetScheduledFrameCompletionCallback(nullptr) != S_OK) {
        RecordError(OutputEvent::Stage::Close, 0, "SetScheduledFrameCompletionCallback(nullptr) failed");
        return false;
      }
      callback_registered = false;
    }
    ReleaseFrames();
    callback.reset();
    output.reset();
    device.reset();
    state.reset();
    return true;
  }

  // Fails an in-progress Open: sets the message, rolls back, and leaves the object
  // either safely Closed or in a retryable closing state when native teardown could
  // not complete. Always returns false.
  bool AbortOpen(std::string *error, const std::string &message) {
    internal::SetError(error, message);
    return RollbackOpen();
  }

  // Rolls back a failed Open. A partial native teardown stays open-but-closing so a
  // later Close (or a Close retry) can finish it; it is never reported as Closed.
  bool RollbackOpen() {
    if (ReleaseSession()) {
      lifecycle = Lifecycle::Closed;
    } else {
      lifecycle = Lifecycle::Running;
      closing = true;
      fatal = true;
    }
    return false;
  }
};

DeckLinkOutput::DeckLinkOutput() : impl_(std::make_unique<Impl>()) {}

DeckLinkOutput::~DeckLinkOutput() {
  if (impl_ != nullptr && impl_->lifecycle != Lifecycle::Closed) {
    std::fprintf(stderr, "DeckLinkOutput destroyed without a successful Close; terminating\n");
    std::terminate();
  }
}

namespace {

// Finds the SDK display mode matching the requested integer rational and scanning
// method exactly. On success reports the native FourCC and the actual SDK mode
// (including its exact timebase), never a copy of the request. A GetFrameRate query
// failure is reported as a query failure instead of being silently treated as a
// missing mode.
bool FindDisplayMode(IDeckLinkOutput *output, const MonitorMode &requested, BMDDisplayMode *out_mode,
                     MonitorMode *out_monitor, std::string *error) {
  IDeckLinkDisplayModeIterator *iterator = nullptr;
  if (output->GetDisplayModeIterator(&iterator) != S_OK || iterator == nullptr) {
    internal::SetError(error, "GetDisplayModeIterator failed");
    return false;
  }
  IDeckLinkDisplayMode *mode = nullptr;
  while (true) {
    const HRESULT next = iterator->Next(&mode);
    if (next == S_FALSE) {
      break;
    }
    if (next != S_OK) {
      iterator->Release();
      internal::SetError(error,
                         "IDeckLinkDisplayModeIterator::Next failed (HRESULT " + internal::FormatHresult(next) + ")");
      return false;
    }
    if (mode == nullptr) {
      break; // S_OK + null is a documented end of the mode list
    }
    BMDTimeValue duration = 0;
    BMDTimeScale scale = 0;
    const HRESULT rate = mode->GetFrameRate(&duration, &scale);
    if (rate != S_OK) {
      mode->Release();
      iterator->Release();
      internal::SetError(error,
                         "IDeckLinkDisplayMode::GetFrameRate failed (HRESULT " + internal::FormatHresult(rate) + ")");
      return false;
    }
    MonitorMode candidate;
    candidate.width = static_cast<int>(mode->GetWidth());
    candidate.height = static_cast<int>(mode->GetHeight());
    candidate.frame_duration = duration;
    candidate.time_scale = scale;
    candidate.is_progressive = mode->GetFieldDominance() == bmdProgressiveFrame;
    // Scanning method must match exactly: a 25 frames/s interlaced mode is not a
    // valid stand-in for a 25p request even though both report 25 frames/s.
    if (duration > 0 && scale > 0 && candidate.is_progressive == requested.is_progressive &&
        MonitorModeIs(candidate, requested.width, requested.height, requested.time_scale, requested.frame_duration)) {
      *out_mode = mode->GetDisplayMode();
      *out_monitor = candidate;
      mode->Release();
      iterator->Release();
      return true;
    }
    mode->Release();
    mode = nullptr;
  }
  iterator->Release();
  internal::SetError(error, "requested display mode not found");
  return false;
}

} // namespace

bool DeckLinkOutput::Open(const OutputRequest &request, ResolvedOutput *resolved, std::string *error) {
  if (resolved == nullptr) {
    internal::SetError(error, "resolved output is null");
    return false;
  }
  Impl &impl = *impl_;
  if (impl.lifecycle != Lifecycle::Closed) {
    internal::SetError(error, "output is already open");
    return false;
  }

  // Start a clean session. A previous successful Close leaves these set for the old
  // session, so a reopen must reset them and discard the old session diagnostics.
  impl.closing = false;
  impl.fatal = false;
  impl.playback = PlaybackState::Idle;
  impl.generation_counter = 0;
  impl.last_actual_stop = 0;
  impl.still_state = SlotState::Free;
  impl.retained_events.clear();
  impl.retained_overflow_pending = false;
  impl.error_count = 0;
  impl.error_recorded = false;
  impl.error_reported = false;
  impl.error_stage = OutputEvent::Stage::None;
  impl.error_status = 0;
  impl.error_info = SubmissionInfo();
  impl.error_display_time = 0;
  impl.error_message[0] = '\0';
  impl.callback_overflow_seen = false;
  impl.retained_dropped = 0;

  if (!ValidateRequest(request, error)) {
    return false;
  }

  // 1A-2 stage constraint: first version supports only these progressive modes.
  const MonitorMode &mode = request.mode;
  if (!IsMonitorModeWhitelisted(mode)) {
    return impl.AbortOpen(error, "mode is not in the first-version whitelist (1920x1080 or 3840x2160)");
  }

  const BMDVideoConnection sdk_connection = internal::ToSdkConnection(request.connection);
  const BMDPixelFormat sdk_format = internal::ToSdkPixelFormat(request.pixel_format);

  if (!internal::ResolveDevice(request.device, impl.device.releaseAndGetAddressOf(), error)) {
    return false;
  }
  if (impl.device->QueryInterface(IID_IDeckLinkOutput,
                                  reinterpret_cast<void **>(impl.output.releaseAndGetAddressOf())) != S_OK ||
      !impl.output) {
    return impl.AbortOpen(error, "device does not expose the IDeckLinkOutput interface");
  }

  BMDDisplayMode sdk_mode = 0;
  MonitorMode actual_mode;
  if (!FindDisplayMode(impl.output.get(), mode, &sdk_mode, &actual_mode, error)) {
    return impl.RollbackOpen();
  }

  BMDDisplayMode actual = sdk_mode;
  bool supported = false;
  const HRESULT support =
      impl.output->DoesSupportVideoMode(sdk_connection, sdk_mode, sdk_format, bmdNoVideoOutputConversion,
                                        bmdSupportedVideoModeDefault, &actual, &supported);
  if (support != S_OK || !supported || actual != sdk_mode) {
    return impl.AbortOpen(error, "DoesSupportVideoMode rejected the requested connection/mode/pixel format (HRESULT " +
                                     internal::FormatHresult(support) + ")");
  }

  if (request.hdr.hdr_signaling) {
    IDeckLinkProfileAttributes *attributes = nullptr;
    const HRESULT qi =
        impl.device->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void **>(&attributes));
    if (qi != S_OK || attributes == nullptr) {
      return impl.AbortOpen(error, "device has no profile attributes to confirm HDR metadata support");
    }
    bool hdr_supported = false;
    const HRESULT flag = attributes->GetFlag(BMDDeckLinkSupportsHDRMetadata, &hdr_supported);
    attributes->Release();
    if (flag != S_OK) {
      return impl.AbortOpen(error, "BMDDeckLinkSupportsHDRMetadata query failed (HRESULT " +
                                       internal::FormatHresult(flag) + ")");
    }
    if (!hdr_supported) {
      return impl.AbortOpen(error, "device does not support HDR metadata; refusing PQ output");
    }
  }

  int32_t row_bytes = 0;
  const HRESULT row_result = impl.output->RowBytesForPixelFormat(sdk_format, actual_mode.width, &row_bytes);
  if (row_result != S_OK || row_bytes <= 0) {
    return impl.AbortOpen(error, "RowBytesForPixelFormat failed (HRESULT " + internal::FormatHresult(row_result) + ")");
  }
  BufferLayout layout;
  layout.width = actual_mode.width;
  layout.height = actual_mode.height;
  layout.pixel_format = request.pixel_format;
  layout.row_bytes = row_bytes;
  layout.image_bytes = static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(actual_mode.height);

  // Read-only occupancy snapshot before enabling anything. This is not a device lock
  // or lease: it refuses to take over a device that reports itself busy right now, but
  // it cannot rule out a concurrent grab after the query. A missing status interface
  // or a failed query also refuses, rather than probing with Enable/Disable.
  {
    IDeckLinkStatus *status = nullptr;
    const HRESULT qi = impl.device->QueryInterface(IID_IDeckLinkStatus, reinterpret_cast<void **>(&status));
    if (qi != S_OK || status == nullptr) {
      return impl.AbortOpen(error, "device does not expose IDeckLinkStatus; refusing to enable output");
    }
    int64_t busy = 0;
    const HRESULT query = status->GetInt(bmdDeckLinkStatusBusy, &busy);
    status->Release();
    if (query != S_OK) {
      return impl.AbortOpen(error, "bmdDeckLinkStatusBusy query failed; refusing to enable output (HRESULT " +
                                       internal::FormatHresult(query) + ")");
    }
    if (busy != 0) {
      return impl.AbortOpen(error, "device is busy (bmdDeckLinkStatusBusy=" + std::to_string(busy) +
                                       "); refusing to preempt another application");
    }
  }

  const HRESULT enable = impl.output->EnableVideoOutput(sdk_mode, bmdVideoOutputFlagDefault);
  if (enable != S_OK) {
    return impl.AbortOpen(error, "EnableVideoOutput failed (HRESULT " + internal::FormatHresult(enable) + ")");
  }
  impl.enabled = true;

  // The only connection check this phase performs: read the active output-connection
  // bitmask after enabling. Never write configuration or silently fall back.
  {
    IDeckLinkConfiguration *configuration = nullptr;
    const HRESULT qi =
        impl.device->QueryInterface(IID_IDeckLinkConfiguration, reinterpret_cast<void **>(&configuration));
    if (qi != S_OK || configuration == nullptr) {
      return impl.AbortOpen(error, "device has no IDeckLinkConfiguration for connection verification");
    }
    int64_t mask = 0;
    const HRESULT get = configuration->GetInt(bmdDeckLinkConfigVideoOutputConnection, &mask);
    configuration->Release();
    if (get != S_OK) {
      return impl.AbortOpen(error, "bmdDeckLinkConfigVideoOutputConnection query failed (HRESULT " +
                                       internal::FormatHresult(get) + ")");
    }
    if ((mask & static_cast<int64_t>(sdk_connection)) == 0) {
      return impl.AbortOpen(error, "requested output connection is not active after EnableVideoOutput");
    }
  }

  impl.state = std::make_shared<CallbackState>();
  impl.callback.reset(new CallbackObject(impl.state));
  if (impl.output->SetScheduledFrameCompletionCallback(impl.callback.get()) != S_OK) {
    return impl.AbortOpen(error, "SetScheduledFrameCompletionCallback failed");
  }
  impl.callback_registered = true;

  // Creates one SDK frame plus its buffer/metadata interfaces. Cleans up its own
  // partial resources on failure so the caller only has to roll back stored frames.
  auto create_frame = [&](FrameHandles *handles) -> bool {
    IDeckLinkMutableVideoFrame *frame = nullptr;
    if (impl.output->CreateVideoFrame(actual_mode.width, actual_mode.height, row_bytes, sdk_format, bmdFrameFlagDefault,
                                      &frame) != S_OK ||
        frame == nullptr) {
      internal::SetError(error, "CreateVideoFrame failed");
      return false;
    }
    IDeckLinkVideoBuffer *buffer = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void **>(&buffer)) != S_OK ||
        buffer == nullptr) {
      internal::SetError(error, "frame does not expose IDeckLinkVideoBuffer");
      frame->Release();
      return false;
    }
    IDeckLinkVideoFrameMutableMetadataExtensions *metadata = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoFrameMutableMetadataExtensions, reinterpret_cast<void **>(&metadata)) !=
            S_OK ||
        metadata == nullptr) {
      internal::SetError(error, "frame does not expose IDeckLinkVideoFrameMutableMetadataExtensions");
      buffer->Release();
      frame->Release();
      return false;
    }
    uint64_t capacity = 0;
    if (buffer->GetSize(&capacity) != S_OK) {
      internal::SetError(error, "IDeckLinkVideoBuffer::GetSize failed");
      metadata->Release();
      buffer->Release();
      frame->Release();
      return false;
    }
    std::string layout_error;
    if (!ValidateLayout(layout, static_cast<std::size_t>(capacity), &layout_error)) {
      internal::SetError(error, "resolved layout rejected: " + layout_error);
      metadata->Release();
      buffer->Release();
      frame->Release();
      return false;
    }
    handles->frame = frame;
    handles->buffer = buffer;
    handles->metadata = metadata;
    return true;
  };

  for (int i = 0; i < kPoolDepth; ++i) {
    FrameHandles handles;
    if (!create_frame(&handles)) {
      return impl.RollbackOpen();
    }
    impl.slots[i].frame = handles.frame;
    impl.slots[i].buffer = handles.buffer;
    impl.slots[i].metadata = handles.metadata;
  }
  if (!create_frame(&impl.still)) {
    return impl.RollbackOpen();
  }

  impl.frame_count = kPoolDepth;
  {
    std::lock_guard<std::mutex> lock(impl.state->mutex);
    for (int i = 0; i < kPoolDepth; ++i) {
      impl.state->frames[i] = impl.slots[i].frame;
    }
    impl.state->frame_count = kPoolDepth;
  }

  ResolvedOutput result;
  result.device = request.device;
  result.mode = actual_mode;
  result.connection = request.connection;
  result.pixel_format = request.pixel_format;
  result.layout = layout;
  result.color = request.color;
  result.hdr = request.hdr;
  result.needs_vertical_flip = false;
  impl.resolved = result;
  *resolved = result;
  impl.lifecycle = Lifecycle::Running;
  return true;
}

ScheduleOutcome DeckLinkOutput::TrySchedule(const PackedFrameView &view, const SubmissionInfo &info,
                                            int64_t display_time, int64_t display_duration) {
  Impl &impl = *impl_;
  impl.CheckCallbackOverflow();
  if (impl.lifecycle != Lifecycle::Running || impl.closing || impl.fatal) {
    return ScheduleOutcome::Closed;
  }
  if (impl.playback == PlaybackState::Stopping) {
    return ScheduleOutcome::Closed;
  }
  if (view.data == nullptr || view.size < impl.resolved.layout.image_bytes) {
    return ScheduleOutcome::Rejected;
  }
  if (display_duration <= 0 || display_duration != impl.resolved.mode.frame_duration) {
    return ScheduleOutcome::Rejected;
  }
  // First version accepts only non-negative display times whose end time fits in
  // int64 and whose generation cannot wrap. Reject before touching the frame buffer.
  if (display_time < 0 || display_time > std::numeric_limits<int64_t>::max() - display_duration ||
      impl.generation_counter == std::numeric_limits<uint64_t>::max()) {
    return ScheduleOutcome::Rejected;
  }
  const int slot_index = impl.FindFreeSlot();
  if (slot_index < 0) {
    return ScheduleOutcome::Busy;
  }
  Impl::Slot &slot = impl.slots[slot_index];
  const uint64_t generation = ++impl.generation_counter;
  slot.generation = generation;
  slot.info = info;
  slot.display_time = display_time;
  slot.state = SlotState::Submitting;

  {
    std::lock_guard<std::mutex> lock(impl.state->mutex);
    SubmissionRecord &record = impl.state->records[slot_index];
    record.active = true;
    record.generation = generation;
    record.info = info;
    record.display_time = display_time;
  }

  FrameHandles handles{slot.frame, slot.buffer, slot.metadata};
  const WriteOutcome write = impl.WriteFrame(handles, view);
  if (write.result != WriteResult::Ok) {
    impl.fatal = true;
    impl.RecordError(write.stage, write.status, write.message.c_str(), info, display_time);
    if (write.result == WriteResult::EndAccessFailed) {
      // CPU access state is unknown: never reuse this frame.
      slot.state = SlotState::Quarantined;
    } else {
      slot.state = SlotState::Free;
    }
    impl.ClearRecord(slot_index);
    return ScheduleOutcome::DeviceError;
  }

  const HRESULT schedule =
      impl.output->ScheduleVideoFrame(slot.frame, display_time, display_duration, impl.resolved.mode.time_scale);
  if (schedule != S_OK) {
    // Keep this generation's record and SDK frame fixed; a completion event or a
    // successful Disable settles it exactly once later. Never guess whether a
    // callback already entered.
    slot.state = SlotState::SubmitFailed;
    impl.fatal = true;
    impl.RecordError(OutputEvent::Stage::Schedule, static_cast<int32_t>(schedule), "ScheduleVideoFrame failed", info,
                     display_time);
    return ScheduleOutcome::DeviceError;
  }
  slot.state = SlotState::Scheduled;
  return ScheduleOutcome::Accepted;
}

StillOutcome DeckLinkOutput::DisplayStill(const PackedFrameView &view, const SubmissionInfo &info) {
  Impl &impl = *impl_;
  impl.CheckCallbackOverflow();
  if (impl.lifecycle != Lifecycle::Running || impl.closing || impl.fatal) {
    return StillOutcome::Closed;
  }
  if (impl.still_state == SlotState::Quarantined) {
    // The previous still frame's CPU access never ended: it can neither be reused
    // nor safely released, so no further still frame is possible.
    return StillOutcome::Closed;
  }
  // Still frames are only allowed on a confirmed-idle scheduler with no outstanding
  // preroll, so the still frame cannot race scheduled frames.
  if (impl.playback != PlaybackState::Idle) {
    impl.EmitDiagnostic(OutputEvent::Stage::DisplayStill, 0, "still frame requires confirmed idle scheduled playback",
                        info, 0);
    return StillOutcome::DeviceError;
  }
  if (impl.UnreconciledCount() != 0) {
    impl.EmitDiagnostic(OutputEvent::Stage::DisplayStill, 0, "still frame requires no outstanding scheduled frames",
                        info, 0);
    return StillOutcome::DeviceError;
  }
  if (view.data == nullptr || view.size < impl.resolved.layout.image_bytes) {
    impl.EmitDiagnostic(OutputEvent::Stage::DisplayStill, 0, "still frame view is null or undersized", info, 0);
    return StillOutcome::DeviceError;
  }
  const WriteOutcome write = impl.WriteFrame(impl.still, view);
  if (write.result != WriteResult::Ok) {
    impl.RecordError(write.stage, write.status, write.message.c_str(), info, 0);
    if (write.result == WriteResult::EndAccessFailed) {
      // Unknown CPU access state: keep the still frame, never reuse or release it.
      impl.still_state = SlotState::Quarantined;
      impl.fatal = true;
    }
    return StillOutcome::DeviceError;
  }
  const HRESULT display = impl.output->DisplayVideoFrameSync(impl.still.frame);
  if (display != S_OK) {
    impl.RecordError(OutputEvent::Stage::DisplayStill, static_cast<int32_t>(display), "DisplayVideoFrameSync failed",
                     info, 0);
    return StillOutcome::DeviceError;
  }
  return StillOutcome::Displayed;
}

bool DeckLinkOutput::PollEvent(OutputEvent *out) {
  if (out == nullptr) {
    return false;
  }
  Impl &impl = *impl_;
  impl.CheckCallbackOverflow();
  // Terminal diagnostics are surfaced first so ordinary completions cannot starve them.
  if (impl.SurfaceError(out)) {
    return true;
  }
  if (!impl.retained_events.empty()) {
    *out = impl.retained_events.front();
    impl.retained_events.pop_front();
    return true;
  }
  if (impl.retained_overflow_pending) {
    // The queue bound dropped some diagnostics; report the total on a dedicated slot
    // that a full queue can never discard.
    *out = impl.retained_overflow_summary;
    impl.retained_overflow_pending = false;
    return true;
  }
  if (impl.state == nullptr) {
    return false;
  }
  CompletionRecord record;
  {
    std::lock_guard<std::mutex> lock(impl.state->mutex);
    if (!impl.state->PopLocked(&record)) {
      return false;
    }
  }
  impl.ProcessRecord(record, out, false);
  return true;
}

bool DeckLinkOutput::StartScheduled(int64_t start_time, std::string *error) {
  Impl &impl = *impl_;
  // A pending callback overflow means completion/reclamation events may already be
  // missing; refuse to start native playback instead of requiring a PollEvent first.
  impl.CheckCallbackOverflow();
  if (impl.lifecycle != Lifecycle::Running || impl.closing || impl.fatal || !impl.enabled) {
    internal::SetError(error, impl.callback_overflow_seen ? "callback event ring overflowed; scheduled start refused"
                                                          : "output is not running");
    return false;
  }
  if (impl.playback != PlaybackState::Idle) {
    internal::SetError(error, "scheduled playback is not idle");
    return false;
  }
  const HRESULT start = impl.output->StartScheduledPlayback(start_time, impl.resolved.mode.time_scale, 1.0);
  if (start != S_OK) {
    internal::SetError(error, "StartScheduledPlayback failed (HRESULT " + internal::FormatHresult(start) + ")");
    return false;
  }
  impl.playback = PlaybackState::Running;
  return true;
}

bool DeckLinkOutput::StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) {
  Impl &impl = *impl_;
  if (impl.lifecycle != Lifecycle::Running || !impl.enabled) {
    internal::SetError(error, "output is not running");
    return false;
  }
  if (impl.playback == PlaybackState::Idle) {
    if (actual_stop != nullptr) {
      *actual_stop = impl.last_actual_stop;
    }
    return true;
  }
  BMDTimeValue actual = 0;
  const HRESULT stop = impl.output->StopScheduledPlayback(stop_at, &actual, impl.resolved.mode.time_scale);
  if (stop != S_OK) {
    internal::SetError(error, "StopScheduledPlayback failed (HRESULT " + internal::FormatHresult(stop) + ")");
    return false;
  }
  // S_OK only means the request was accepted: wait for the stopped notification or a
  // native running query before reporting idle. Never fabricate the stopped state.
  impl.last_actual_stop = actual;
  impl.playback = PlaybackState::Stopping;
  if (actual_stop != nullptr) {
    *actual_stop = actual;
  }
  return true;
}

bool DeckLinkOutput::QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) {
  Impl &impl = *impl_;
  if (impl.lifecycle != Lifecycle::Running || !impl.enabled || !impl.output) {
    internal::SetError(error, "output is not running");
    return false;
  }
  BMDTimeValue native_stream_time = 0;
  double native_speed = 0.0;
  const HRESULT result = impl.output->GetScheduledStreamTime(time_scale, &native_stream_time, &native_speed);
  if (result != S_OK) {
    internal::SetError(error, "GetScheduledStreamTime failed (HRESULT " + internal::FormatHresult(result) + ")");
    return false;
  }
  if (stream_time != nullptr) {
    *stream_time = native_stream_time;
  }
  if (speed != nullptr) {
    *speed = native_speed;
  }
  return true;
}

CloseStatus DeckLinkOutput::Close(int drain_timeout_ms) {
  Impl &impl = *impl_;
  if (impl.lifecycle == Lifecycle::Closed) {
    return CloseStatus::Closed;
  }
  if (impl.lifecycle == Lifecycle::Failed) {
    return CloseStatus::DeviceError;
  }

  impl.closing = true;
  impl.CheckCallbackOverflow();

  if (impl.enabled && impl.playback != PlaybackState::Idle) {
    BMDTimeValue actual = 0;
    if (impl.output->StopScheduledPlayback(0, &actual, impl.resolved.mode.time_scale) == S_OK) {
      impl.last_actual_stop = actual;
      impl.playback = PlaybackState::Stopping;
    } else {
      impl.RecordError(OutputEvent::Stage::Close, 0, "StopScheduledPlayback during close failed");
    }
  }

  // Bounded application-side drain; the native Disable below is the real flush.
  impl.DrainWithin(drain_timeout_ms);

  // Always stop output this session enabled, even for zero frames or all-Free pools.
  // A failed Disable does not advance any flag and does not unregister the callback:
  // the session stays retryable and reports DeviceError instead of pretending the
  // output was stopped.
  if (impl.enabled) {
    if (!impl.output || impl.output->DisableVideoOutput() != S_OK) {
      impl.RecordError(OutputEvent::Stage::Close, 0, "DisableVideoOutput failed");
      return CloseStatus::DeviceError;
    }
    impl.enabled = false;
    impl.playback = PlaybackState::Idle;
  }

  // Unregister only after the output is confirmed stopped. A failed unregister keeps
  // the registration and the resources so the next Close can retry it.
  if (impl.callback_registered) {
    if (!impl.output || impl.output->SetScheduledFrameCompletionCallback(nullptr) != S_OK) {
      impl.RecordError(OutputEvent::Stage::Close, 0, "SetScheduledFrameCompletionCallback(nullptr) failed");
      return CloseStatus::DeviceError;
    }
    impl.callback_registered = false;
  }

  // Close event reception and take the events that already arrived, then settle the
  // remaining slots. Real events are retained for PollEvent; unknown results are
  // reported, never fabricated as Completed. The copy uses a fixed array so no
  // dynamic allocation happens under the callback mutex.
  CompletionRecord arrived[kEventCapacity];
  std::size_t arrived_count = 0;
  if (impl.state != nullptr) {
    std::lock_guard<std::mutex> lock(impl.state->mutex);
    impl.state->receiving = false;
    CompletionRecord record;
    while (arrived_count < kEventCapacity && impl.state->PopLocked(&record)) {
      arrived[arrived_count++] = record;
    }
  }
  for (std::size_t i = 0; i < arrived_count; ++i) {
    impl.ProcessRecord(arrived[i], nullptr, true);
  }

  // Event reception is now closed, so a final unsticky check catches an overflow
  // that happened after the entry check but before receiving was disabled. It runs
  // while CallbackState is still alive, so the notice is never lost with the state.
  impl.CheckCallbackOverflow();

  bool quarantined = impl.still_state == SlotState::Quarantined;
  for (int i = 0; i < impl.frame_count; ++i) {
    if (impl.slots[i].state == SlotState::Free) {
      continue;
    }
    if (impl.slots[i].state == SlotState::Quarantined) {
      quarantined = true;
      continue;
    }
    impl.slots[i].state = SlotState::SettledUnknown;
    impl.RecordError(OutputEvent::Stage::Close, 0, "SDK frame settled without a completion event");
  }

  if (quarantined) {
    // A frame whose CPU access state is unknown can neither be reused nor safely
    // released. Keep it and the session so the failure stays visible.
    impl.lifecycle = Lifecycle::Failed;
    return CloseStatus::DeviceError;
  }

  impl.RetainPendingError();
  impl.ReleaseFrames();
  impl.callback.reset();
  impl.output.reset();
  impl.device.reset();
  impl.state.reset();
  impl.lifecycle = Lifecycle::Closed;
  return CloseStatus::Closed;
}

uint32_t DeckLinkOutput::unreconciled_frame_count() const {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->UnreconciledCount();
}

} // namespace EYEQ
