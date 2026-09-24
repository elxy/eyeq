#pragma once

// Stateful fake DeckLink SDK for the output backend tests. The test executable that
// links this file does NOT link DeckLinkAPIDispatch.cpp: the two factory functions
// below replace the SDK dispatch, so the real DeckLinkOutput state machine runs
// against a controllable SDK boundary. No device, driver or GPU is touched, and
// every unimplemented (capture/audio/configuration write) method aborts.

#include <cstdint>
#include <vector>

namespace eyeq_decklink_test {

enum class OutputFault {
  None,
  IteratorUnavailable,  // factory returns nullptr
  ModeMissing,          // no display mode matches the request
  SupportFalse,         // DoesSupportVideoMode returns supported=false
  HdrUnsupported,       // BMDDeckLinkSupportsHDRMetadata is false
  HdrQueryFails,        // BMDDeckLinkSupportsHDRMetadata query fails
  ConnectionQueryFails, // bmdDeckLinkConfigVideoOutputConnection query fails
  ConnectionMissing,    // active connection mask lacks the requested connection
  RowBytesFails,
  EnableFails,
  SetCallbackFails,
  CreateFrameFails,
  GetSizeFails,
  StartAccessFails,
  GetBytesFails,
  EndAccessFails,
  GetBytesAndEndAccessFail, // GetBytes fails and EndAccess keeps the access state unknown
  FrameRateFails,           // IDeckLinkDisplayMode::GetFrameRate fails
  SetFlagsFails,
  MetadataFails,
  ScheduleFails,   // ScheduleVideoFrame returns E_FAIL (acceptance unknown)
  ScheduleRejects, // ScheduleVideoFrame returns E_INVALIDARG (definite reject)
  StartFails,
  StopFails,
  IsRunningFails,
  DisableFails,
  Busy,             // bmdDeckLinkStatusBusy GetInt returns S_OK with a non-zero busy state
  StatusMissing,    // IDeckLinkStatus is not exposed by the device
  StatusQueryFails, // bmdDeckLinkStatusBusy GetInt returns E_FAIL
};

struct FakeOutputStats {
  int enable_calls = 0;
  int disable_calls = 0;
  int set_callback_calls = 0;
  int create_frame_calls = 0;
  int row_bytes_calls = 0;
  int schedule_calls = 0;
  int start_calls = 0;
  int stop_calls = 0;
  int is_running_calls = 0;
  int display_sync_calls = 0;
  int start_access_calls = 0;
  int end_access_calls = 0;
  int set_flags_calls = 0;
  int metadata_set_calls = 0;
  int stream_time_calls = 0;
  int status_calls = 0;
  int support_connection = 0; // last connection passed to DoesSupportVideoMode
};

// Resets all fake state (registered callback, created frames, counters, faults).
void ResetFakeOutput();

void SetOutputFault(OutputFault fault);
const FakeOutputStats &OutputStats();

// When enabled, ScheduleVideoFrame synchronously invokes the registered completion
// callback before returning its result (models native synchronous reentrancy).
void SetScheduleReentrant(bool enabled);

// Overrides the single display mode the fake advertises. Defaults to a progressive
// 3840x2160@50 mode. Use it to inject another scanning method or exact timebase.
void SetDisplayMode(long width, long height, int64_t frame_duration, int64_t time_scale, bool progressive);

// Forces IsScheduledPlaybackRunning to report this value (native_running is also set
// by Start/StopScheduledPlayback).
void SetNativeRunning(bool running);

// Forces the output-connection query (bmdDeckLinkConfigVideoOutputConnection) to fail
// independently of OutputFault, so it can compound with a Disable failure.
void SetConnectionQueryFails(bool fails);

// When count > 0, DisableVideoOutput delivers that many PlaybackStopped notifications
// to the registered callback. Used to deterministically overflow the callback ring
// inside Close, after its entry overflow check.
void SetStoppedNotificationsOnDisable(int count);

// Injects a failure in the frame pool creation sequence. create_call_index is 1-based
// (1..9 for 8 pool frames plus the still frame); 0 disables the injection.
void SetCreateFrameFailAt(int create_call_index);
void SetGetSizeFailAt(int create_call_index);
void SetMetadataQueryFailAt(int create_call_index);

// Optional gate invoked while a delivered completion callback is in flight, with a
// reference already held. Lets a test hold a callback in flight across Close without
// sleeping or racing.
void SetCompletionGate(void (*gate)(void *), void *context);

// Metadata observability: records every Set* call so a test can assert the SDK ID,
// the setter type and the value. Metadata value type ids are kFakeMetadataType*.
struct FakeMetadataSet {
  int64_t id = 0;
  int type = 0;
  int64_t int_value = 0;
  double float_value = 0.0;
};
constexpr int kFakeMetadataTypeInt = 0;
constexpr int kFakeMetadataTypeFloat = 1;
constexpr int kFakeMetadataTypeFlag = 2;
constexpr int kFakeMetadataTypeString = 3;
constexpr int kFakeMetadataTypeBytes = 4;
const std::vector<FakeMetadataSet> &MetadataSets();

// Lifetime observability: live fake COM objects and buffers destroyed while they were
// still marked as accessed. Used to assert that a backend destroyed after rollback or
// shutdown leaves no leaked SDK interface (and no buffer released during access).
int LiveObjectCount();
int BuffersReleasedWhileAccessing();

// Test-side delivery of asynchronous notifications.
bool DeliverCompletion(int frame_index, int result); // result: 0=Completed,1=Late,2=Dropped,3=Flushed
bool DeliverPlaybackStopped();

// Number of frames the backend created (pool + still).
int CreatedFrameCount();

// Holds an extra reference to the registered callback so a test can deliver a late
// callback after the backend unregisters it.
void CaptureRegisteredCallbackForLateDelivery();
bool DeliverLateCompletion(int frame_index, int result);

// Contents of the SDK frame buffer that was last passed to ScheduleVideoFrame.
const std::vector<std::uint8_t> &LastScheduledBytes();
int LastScheduledFrameIndex();

} // namespace eyeq_decklink_test
