// Stateful fake DeckLink SDK implementation for the output backend tests. See the
// header for the contract. Every method the backend is not expected to call
// (capture, audio, configuration writes, ancillary, custom allocators) aborts.
#include "fake_decklink_output.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "DeckLinkAPI.h"

namespace eyeq_decklink_test {
namespace {

struct FakeFrameData {
  int32_t width = 0;
  int32_t height = 0;
  int32_t row_bytes = 0;
  BMDPixelFormat format = bmdFormat10BitYUV;
  BMDFrameFlags flags = bmdFrameFlagDefault;
  std::vector<uint8_t> bytes;
  bool accessing = false;
};

struct FakeDisplayModeSpec {
  long width = 3840;
  long height = 2160;
  int64_t frame_duration = 1000;
  int64_t time_scale = 50000;
  bool progressive = true;
  BMDDisplayMode mode = bmdMode4K2160p50;
};

struct FakeState {
  OutputFault fault = OutputFault::None;
  FakeOutputStats stats;
  bool enabled = false;
  bool schedule_reentrant = false;
  bool native_running = false;
  FakeDisplayModeSpec display_mode;
  int create_frame_fail_at = 0;
  int get_size_fail_at = 0;
  int metadata_query_fail_at = 0;
  bool connection_query_fails = false;
  int stopped_notifications_on_disable = 0;
  void (*completion_gate)(void *) = nullptr;
  void *completion_gate_context = nullptr;
  std::vector<FakeMetadataSet> metadata_sets;
  IDeckLinkVideoOutputCallback *registered = nullptr;
  IDeckLinkVideoOutputCallback *late = nullptr;
  std::vector<IDeckLinkMutableVideoFrame *> created;
  std::vector<uint8_t> last_scheduled_bytes;
  int last_scheduled = -1;
  int64_t connection_mask = static_cast<int64_t>(bmdVideoConnectionSDI) | static_cast<int64_t>(bmdVideoConnectionHDMI);
};

FakeState g_state;

// Delivers one completion to a callback while holding an extra reference for the
// duration of the call, the way a well-behaved SDK does. The optional gate runs with
// that reference held so a test can block the callback and still tear down safely.
void DeliverScheduledFrameCompleted(IDeckLinkVideoOutputCallback *callback, IDeckLinkVideoFrame *frame,
                                    BMDOutputFrameCompletionResult result) {
  callback->AddRef();
  if (g_state.completion_gate != nullptr) {
    g_state.completion_gate(g_state.completion_gate_context);
  }
  callback->ScheduledFrameCompleted(frame, result);
  callback->Release();
}

void DeliverScheduledPlaybackStopped(IDeckLinkVideoOutputCallback *callback) {
  callback->AddRef();
  callback->ScheduledPlaybackHasStopped();
  callback->Release();
}

// Lifetime observability for tests: counts every live fake COM object and every
// buffer destroyed while it still reported an active CPU access.
std::atomic<int> g_live_objects{0};
std::atomic<int> g_buffers_released_accessing{0};

template <typename Derived> class RefCounted {
public:
  RefCounted() { ++g_live_objects; }
  ULONG AddRef() { return ++references_; }
  ULONG Release() {
    const ULONG count = --references_;
    if (count == 0) {
      delete static_cast<Derived *>(this);
    }
    return count;
  }
  virtual ~RefCounted() { --g_live_objects; }

private:
  std::atomic<ULONG> references_{1};
};

// Each fake interface must override the COM reference counting explicitly: the CRTP
// base member alone is ambiguous with the interface's inherited pure virtual
// AddRef/Release and would leave the class abstract.
#define FAKE_COM_REFCOUNT(Class)                                                                                       \
  ULONG AddRef() override { return RefCounted<Class>::AddRef(); }                                                      \
  ULONG Release() override { return RefCounted<Class>::Release(); }

class FakeBuffer final : public RefCounted<FakeBuffer>, public IDeckLinkVideoBuffer {
public:
  FAKE_COM_REFCOUNT(FakeBuffer)
  explicit FakeBuffer(std::shared_ptr<FakeFrameData> data) : data_(std::move(data)) {}
  ~FakeBuffer() override {
    if (data_->accessing) {
      ++g_buffers_released_accessing;
    }
  }
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkVideoBuffer, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkVideoBuffer *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetBytes(void **buffer) override {
    if (buffer == nullptr) {
      return E_POINTER;
    }
    if (g_state.fault == OutputFault::GetBytesFails || g_state.fault == OutputFault::GetBytesAndEndAccessFail) {
      *buffer = nullptr;
      return E_FAIL;
    }
    if (!data_->accessing) {
      *buffer = nullptr;
      return E_ACCESSDENIED;
    }
    *buffer = data_->bytes.data();
    return S_OK;
  }
  HRESULT GetSize(uint64_t *size) override {
    if (size == nullptr) {
      return E_POINTER;
    }
    if (g_state.fault == OutputFault::GetSizeFails ||
        (g_state.get_size_fail_at != 0 && g_state.stats.create_frame_calls == g_state.get_size_fail_at)) {
      return E_FAIL;
    }
    *size = data_->bytes.size();
    return S_OK;
  }
  HRESULT StartAccess(BMDBufferAccessFlags) override {
    ++g_state.stats.start_access_calls;
    if (g_state.fault == OutputFault::StartAccessFails) {
      return E_FAIL;
    }
    data_->accessing = true;
    return S_OK;
  }
  HRESULT EndAccess(BMDBufferAccessFlags) override {
    ++g_state.stats.end_access_calls;
    if (!data_->accessing) {
      return E_INVALIDARG;
    }
    if (g_state.fault == OutputFault::EndAccessFails || g_state.fault == OutputFault::GetBytesAndEndAccessFail) {
      // A failed EndAccess deliberately leaves the access state unknown; a test that
      // clears it here would hide the unsafe release it must catch.
      return E_FAIL;
    }
    data_->accessing = false;
    return S_OK;
  }

private:
  std::shared_ptr<FakeFrameData> data_;
};

class FakeMetadata final : public RefCounted<FakeMetadata>, public IDeckLinkVideoFrameMutableMetadataExtensions {
public:
  FAKE_COM_REFCOUNT(FakeMetadata)
  explicit FakeMetadata(std::shared_ptr<FakeFrameData> data) : data_(std::move(data)) {}
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkVideoFrameMutableMetadataExtensions, sizeof(iid)) == 0 ||
        memcmp(&iid, &IID_IDeckLinkVideoFrameMetadataExtensions, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkVideoFrameMutableMetadataExtensions *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetInt(BMDDeckLinkFrameMetadataID, int64_t *) override { return E_INVALIDARG; }
  HRESULT GetFloat(BMDDeckLinkFrameMetadataID, double *) override { return E_INVALIDARG; }
  HRESULT GetFlag(BMDDeckLinkFrameMetadataID, bool *) override { return E_INVALIDARG; }
  HRESULT GetString(BMDDeckLinkFrameMetadataID, CFStringRef *) override { return E_INVALIDARG; }
  HRESULT GetBytes(BMDDeckLinkFrameMetadataID, void *, uint32_t *) override { return E_INVALIDARG; }
  HRESULT SetInt(BMDDeckLinkFrameMetadataID id, int64_t value) override {
    // The SDK defines CLL/FALL as Float. Reject SetInt so a wrong setter cannot pass.
    if (id == bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel ||
        id == bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel) {
      return E_INVALIDARG;
    }
    return Record(kFakeMetadataTypeInt, id, value, 0.0);
  }
  HRESULT SetFloat(BMDDeckLinkFrameMetadataID id, double value) override {
    return Record(kFakeMetadataTypeFloat, id, 0, value);
  }
  HRESULT SetFlag(BMDDeckLinkFrameMetadataID id, bool value) override {
    return Record(kFakeMetadataTypeFlag, id, value ? 1 : 0, 0.0);
  }
  HRESULT SetString(BMDDeckLinkFrameMetadataID id, CFStringRef) override {
    return Record(kFakeMetadataTypeString, id, 0, 0.0);
  }
  HRESULT SetBytes(BMDDeckLinkFrameMetadataID id, void *, uint32_t) override {
    return Record(kFakeMetadataTypeBytes, id, 0, 0.0);
  }

private:
  HRESULT Record(int type, BMDDeckLinkFrameMetadataID id, int64_t int_value, double float_value) {
    ++g_state.stats.metadata_set_calls;
    FakeMetadataSet set;
    set.id = static_cast<int64_t>(id);
    set.type = type;
    set.int_value = int_value;
    set.float_value = float_value;
    g_state.metadata_sets.push_back(set);
    return g_state.fault == OutputFault::MetadataFails ? E_FAIL : S_OK;
  }
  std::shared_ptr<FakeFrameData> data_;
};

class FakeFrame final : public RefCounted<FakeFrame>, public IDeckLinkMutableVideoFrame {
public:
  FAKE_COM_REFCOUNT(FakeFrame)
  explicit FakeFrame(std::shared_ptr<FakeFrameData> data) : data_(std::move(data)) {}
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkMutableVideoFrame, sizeof(iid)) == 0 ||
        memcmp(&iid, &IID_IDeckLinkVideoFrame, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkMutableVideoFrame *>(this);
      return S_OK;
    }
    if (memcmp(&iid, &IID_IDeckLinkVideoBuffer, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkVideoBuffer *>(new FakeBuffer(data_));
      return S_OK;
    }
    if (memcmp(&iid, &IID_IDeckLinkVideoFrameMutableMetadataExtensions, sizeof(iid)) == 0 ||
        memcmp(&iid, &IID_IDeckLinkVideoFrameMetadataExtensions, sizeof(iid)) == 0) {
      if (g_state.metadata_query_fail_at != 0 && g_state.stats.create_frame_calls == g_state.metadata_query_fail_at) {
        *out = nullptr;
        return E_NOINTERFACE;
      }
      *out = static_cast<IDeckLinkVideoFrameMutableMetadataExtensions *>(new FakeMetadata(data_));
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  long GetWidth() override { return data_->width; }
  long GetHeight() override { return data_->height; }
  long GetRowBytes() override { return data_->row_bytes; }
  BMDPixelFormat GetPixelFormat() override { return data_->format; }
  BMDFrameFlags GetFlags() override { return data_->flags; }
  HRESULT GetTimecode(BMDTimecodeFormat, IDeckLinkTimecode **) override { return E_NOTIMPL; }
  HRESULT GetAncillaryData(IDeckLinkVideoFrameAncillary **) override { return E_NOTIMPL; }
  HRESULT SetFlags(BMDFrameFlags new_flags) override {
    ++g_state.stats.set_flags_calls;
    if (g_state.fault == OutputFault::SetFlagsFails) {
      return E_FAIL;
    }
    data_->flags = new_flags;
    return S_OK;
  }
  HRESULT SetTimecode(BMDTimecodeFormat, IDeckLinkTimecode *) override { return E_NOTIMPL; }
  HRESULT SetTimecodeFromComponents(BMDTimecodeFormat, uint8_t, uint8_t, uint8_t, uint8_t, BMDTimecodeFlags) override {
    return E_NOTIMPL;
  }
  HRESULT SetAncillaryData(IDeckLinkVideoFrameAncillary *) override { return E_NOTIMPL; }
  HRESULT SetTimecodeUserBits(BMDTimecodeFormat, BMDTimecodeUserBits) override { return E_NOTIMPL; }
  HRESULT SetInterfaceProvider(REFIID, IUnknown *) override { return E_NOTIMPL; }

  const std::vector<uint8_t> &Bytes() const { return data_->bytes; }

private:
  std::shared_ptr<FakeFrameData> data_;
};

class FakeDisplayMode final : public RefCounted<FakeDisplayMode>, public IDeckLinkDisplayMode {
public:
  FAKE_COM_REFCOUNT(FakeDisplayMode)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkDisplayMode, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkDisplayMode *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetName(CFStringRef *name) override {
    *name = CFSTR("Fake UHD50");
    CFRetain(*name);
    return S_OK;
  }
  BMDDisplayMode GetDisplayMode() override { return g_state.display_mode.mode; }
  long GetWidth() override { return g_state.display_mode.width; }
  long GetHeight() override { return g_state.display_mode.height; }
  HRESULT GetFrameRate(BMDTimeValue *duration, BMDTimeScale *scale) override {
    if (g_state.fault == OutputFault::FrameRateFails) {
      return E_FAIL;
    }
    if (g_state.fault == OutputFault::ModeMissing) {
      *duration = 1000;
      *scale = 25000;
    } else {
      *duration = g_state.display_mode.frame_duration;
      *scale = g_state.display_mode.time_scale;
    }
    return S_OK;
  }
  BMDFieldDominance GetFieldDominance() override {
    return g_state.display_mode.progressive ? bmdProgressiveFrame : bmdUpperFieldFirst;
  }
  BMDDisplayModeFlags GetFlags() override { return 0; }
};

class FakeDisplayModeIterator final : public RefCounted<FakeDisplayModeIterator>, public IDeckLinkDisplayModeIterator {
  bool returned_ = false;

public:
  FAKE_COM_REFCOUNT(FakeDisplayModeIterator)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkDisplayModeIterator, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkDisplayModeIterator *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT Next(IDeckLinkDisplayMode **mode) override {
    if (mode == nullptr) {
      return E_POINTER;
    }
    if (returned_) {
      *mode = nullptr;
      return S_FALSE;
    }
    returned_ = true;
    *mode = new FakeDisplayMode;
    return S_OK;
  }
};

class FakeOutput final : public RefCounted<FakeOutput>, public IDeckLinkOutput {
public:
  FAKE_COM_REFCOUNT(FakeOutput)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkOutput, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkOutput *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT DoesSupportVideoMode(BMDVideoConnection connection, BMDDisplayMode requested, BMDPixelFormat format,
                               BMDVideoOutputConversionMode conversion, BMDSupportedVideoModeFlags flags,
                               BMDDisplayMode *actual, bool *supported) override {
    if (supported == nullptr || actual == nullptr) {
      return E_INVALIDARG;
    }
    if ((connection != bmdVideoConnectionSDI && connection != bmdVideoConnectionHDMI) ||
        (format != bmdFormat10BitYUV && format != bmdFormat10BitRGB) || conversion != bmdNoVideoOutputConversion ||
        flags != bmdSupportedVideoModeDefault) {
      std::abort();
    }
    *actual = requested;
    *supported = g_state.fault != OutputFault::SupportFalse;
    return S_OK;
  }
  HRESULT GetDisplayMode(BMDDisplayMode, IDeckLinkDisplayMode **result) override {
    *result = new FakeDisplayMode;
    return S_OK;
  }
  HRESULT GetDisplayModeIterator(IDeckLinkDisplayModeIterator **iterator) override {
    *iterator = new FakeDisplayModeIterator;
    return S_OK;
  }
  HRESULT EnableVideoOutput(BMDDisplayMode, BMDVideoOutputFlags) override {
    ++g_state.stats.enable_calls;
    if (g_state.fault == OutputFault::EnableFails) {
      return E_FAIL;
    }
    g_state.enabled = true;
    return S_OK;
  }
  HRESULT DisableVideoOutput(void) override {
    ++g_state.stats.disable_calls;
    if (g_state.fault == OutputFault::DisableFails) {
      return E_FAIL;
    }
    g_state.enabled = false;
    // Models a native flush that delivers stopped notifications while the callback
    // is still registered. Used to overflow the ring inside Close deterministically.
    if (g_state.stopped_notifications_on_disable > 0 && g_state.registered != nullptr) {
      for (int i = 0; i < g_state.stopped_notifications_on_disable; ++i) {
        DeliverScheduledPlaybackStopped(g_state.registered);
      }
    }
    return S_OK;
  }
  HRESULT CreateVideoFrame(int32_t width, int32_t height, int32_t row_bytes, BMDPixelFormat format, BMDFrameFlags flags,
                           IDeckLinkMutableVideoFrame **out_frame) override {
    ++g_state.stats.create_frame_calls;
    if (out_frame == nullptr) {
      return E_POINTER;
    }
    if (g_state.fault == OutputFault::CreateFrameFails ||
        (g_state.create_frame_fail_at != 0 && g_state.stats.create_frame_calls == g_state.create_frame_fail_at)) {
      *out_frame = nullptr;
      return E_FAIL;
    }
    auto data = std::make_shared<FakeFrameData>();
    data->width = width;
    data->height = height;
    data->row_bytes = row_bytes;
    data->format = format;
    data->flags = flags;
    data->bytes.assign(static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(height), 0);
    auto *frame = new FakeFrame(data);
    frame->AddRef(); // the fake keeps its own reference for test delivery
    g_state.created.push_back(frame);
    *out_frame = frame;
    return S_OK;
  }
  HRESULT RowBytesForPixelFormat(BMDPixelFormat format, int32_t width, int32_t *row_bytes) override {
    ++g_state.stats.row_bytes_calls;
    if (row_bytes == nullptr) {
      return E_POINTER;
    }
    if (g_state.fault == OutputFault::RowBytesFails) {
      return E_FAIL;
    }
    if (format == bmdFormat10BitRGB) {
      *row_bytes = width * 4;
    } else if (format == bmdFormat10BitYUV) {
      *row_bytes = ((width + 5) / 6) * 16;
    } else {
      return E_INVALIDARG;
    }
    return S_OK;
  }
  HRESULT DisplayVideoFrameSync(IDeckLinkVideoFrame *) override {
    ++g_state.stats.display_sync_calls;
    return S_OK;
  }
  HRESULT ScheduleVideoFrame(IDeckLinkVideoFrame *frame, BMDTimeValue, BMDTimeValue, BMDTimeScale) override {
    ++g_state.stats.schedule_calls;
    g_state.last_scheduled = -1;
    for (std::size_t i = 0; i < g_state.created.size(); ++i) {
      if (g_state.created[i] == frame) {
        g_state.last_scheduled = static_cast<int>(i);
        break;
      }
    }
    if (g_state.last_scheduled >= 0) {
      const auto *fake_frame = static_cast<FakeFrame *>(frame);
      g_state.last_scheduled_bytes = fake_frame->Bytes();
    }
    if (g_state.schedule_reentrant && g_state.registered != nullptr) {
      DeliverScheduledFrameCompleted(g_state.registered, frame, bmdOutputFrameCompleted);
    }
    if (g_state.fault == OutputFault::ScheduleFails) {
      return E_FAIL;
    }
    if (g_state.fault == OutputFault::ScheduleRejects) {
      return E_INVALIDARG;
    }
    return S_OK;
  }
  HRESULT SetScheduledFrameCompletionCallback(IDeckLinkVideoOutputCallback *callback) override {
    ++g_state.stats.set_callback_calls;
    if (g_state.fault == OutputFault::SetCallbackFails) {
      return E_FAIL;
    }
    if (g_state.registered != nullptr) {
      g_state.registered->Release();
    }
    g_state.registered = callback;
    if (callback != nullptr) {
      callback->AddRef();
    }
    return S_OK;
  }
  HRESULT GetBufferedVideoFrameCount(uint32_t *count) override {
    *count = 0;
    return S_OK;
  }
  HRESULT StartScheduledPlayback(BMDTimeValue, BMDTimeScale, double) override {
    ++g_state.stats.start_calls;
    if (g_state.fault == OutputFault::StartFails) {
      return E_FAIL;
    }
    g_state.native_running = true;
    return S_OK;
  }
  HRESULT StopScheduledPlayback(BMDTimeValue, BMDTimeValue *actual_stop, BMDTimeScale) override {
    ++g_state.stats.stop_calls;
    if (g_state.fault == OutputFault::StopFails) {
      return E_FAIL;
    }
    g_state.native_running = false;
    if (actual_stop != nullptr) {
      *actual_stop = 4242;
    }
    return S_OK;
  }
  HRESULT IsScheduledPlaybackRunning(bool *active) override {
    ++g_state.stats.is_running_calls;
    if (active == nullptr) {
      return E_POINTER;
    }
    if (g_state.fault == OutputFault::IsRunningFails) {
      return E_FAIL;
    }
    *active = g_state.native_running;
    return S_OK;
  }
  HRESULT GetScheduledStreamTime(BMDTimeScale, BMDTimeValue *stream_time, double *speed) override {
    ++g_state.stats.stream_time_calls;
    if (stream_time != nullptr) {
      *stream_time = 1234;
    }
    if (speed != nullptr) {
      *speed = 1.0;
    }
    return S_OK;
  }

  // Not part of the output path: abort so an accidental call is caught in tests.
  HRESULT SetScreenPreviewCallback(IDeckLinkScreenPreviewCallback *) override { std::abort(); }
  HRESULT CreateVideoFrameWithBuffer(int32_t, int32_t, int32_t, BMDPixelFormat, BMDFrameFlags, IDeckLinkVideoBuffer *,
                                     IDeckLinkMutableVideoFrame **) override {
    std::abort();
  }
  HRESULT CreateAncillaryData(BMDPixelFormat, IDeckLinkVideoFrameAncillary **) override { std::abort(); }
  HRESULT EnableAudioOutput(BMDAudioSampleRate, BMDAudioSampleType, uint32_t, BMDAudioOutputStreamType) override {
    std::abort();
  }
  HRESULT DisableAudioOutput(void) override { std::abort(); }
  HRESULT WriteAudioSamplesSync(void *, uint32_t, uint32_t *) override { std::abort(); }
  HRESULT BeginAudioPreroll(void) override { std::abort(); }
  HRESULT EndAudioPreroll(void) override { std::abort(); }
  HRESULT ScheduleAudioSamples(void *, uint32_t, BMDTimeValue, BMDTimeScale, uint32_t *) override { std::abort(); }
  HRESULT GetBufferedAudioSampleFrameCount(uint32_t *) override { std::abort(); }
  HRESULT FlushBufferedAudioSamples(void) override { std::abort(); }
  HRESULT SetAudioCallback(IDeckLinkAudioOutputCallback *) override { std::abort(); }
  HRESULT GetReferenceStatus(BMDReferenceStatus *) override { std::abort(); }
  HRESULT GetHardwareReferenceClock(BMDTimeScale, BMDTimeValue *, BMDTimeValue *, BMDTimeValue *) override {
    std::abort();
  }
  HRESULT GetFrameCompletionReferenceTimestamp(IDeckLinkVideoFrame *, BMDTimeScale, BMDTimeValue *) override {
    std::abort();
  }
};

class FakeProfileAttributes final : public RefCounted<FakeProfileAttributes>, public IDeckLinkProfileAttributes {
public:
  FAKE_COM_REFCOUNT(FakeProfileAttributes)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkProfileAttributes, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkProfileAttributes *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetFlag(BMDDeckLinkAttributeID id, bool *value) override {
    if (value == nullptr) {
      return E_POINTER;
    }
    if (id != BMDDeckLinkSupportsHDRMetadata) {
      return E_INVALIDARG;
    }
    if (g_state.fault == OutputFault::HdrQueryFails) {
      return E_FAIL;
    }
    *value = g_state.fault != OutputFault::HdrUnsupported;
    return S_OK;
  }
  HRESULT GetInt(BMDDeckLinkAttributeID id, int64_t *value) override {
    if (value == nullptr) {
      return E_POINTER;
    }
    if (id == BMDDeckLinkPersistentID) {
      *value = 2697367084LL;
      return S_OK;
    }
    return E_INVALIDARG;
  }
  HRESULT GetFloat(BMDDeckLinkAttributeID, double *) override { return E_INVALIDARG; }
  HRESULT GetString(BMDDeckLinkAttributeID, CFStringRef *) override { return E_INVALIDARG; }
  HRESULT GetStringWithParam(BMDDeckLinkAttributeID, uint64_t, CFStringRef *) override { return E_INVALIDARG; }
};

class FakeConfiguration final : public RefCounted<FakeConfiguration>, public IDeckLinkConfiguration {
public:
  FAKE_COM_REFCOUNT(FakeConfiguration)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkConfiguration, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkConfiguration *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetInt(BMDDeckLinkConfigurationID id, int64_t *value) override {
    if (value == nullptr) {
      return E_POINTER;
    }
    if (id != bmdDeckLinkConfigVideoOutputConnection) {
      return E_INVALIDARG;
    }
    if (g_state.fault == OutputFault::ConnectionQueryFails || g_state.connection_query_fails) {
      return E_FAIL;
    }
    *value = g_state.fault == OutputFault::ConnectionMissing ? 0 : g_state.connection_mask;
    return S_OK;
  }
  HRESULT SetFlag(BMDDeckLinkConfigurationID, bool) override { std::abort(); }
  HRESULT GetFlag(BMDDeckLinkConfigurationID, bool *) override { return E_INVALIDARG; }
  HRESULT SetInt(BMDDeckLinkConfigurationID, int64_t) override { std::abort(); }
  HRESULT SetFloat(BMDDeckLinkConfigurationID, double) override { std::abort(); }
  HRESULT GetFloat(BMDDeckLinkConfigurationID, double *) override { return E_INVALIDARG; }
  HRESULT SetString(BMDDeckLinkConfigurationID, CFStringRef) override { std::abort(); }
  HRESULT GetString(BMDDeckLinkConfigurationID, CFStringRef *) override { return E_INVALIDARG; }
  HRESULT SetFlagWithParam(BMDDeckLinkConfigurationID, uint64_t, bool) override { std::abort(); }
  HRESULT GetFlagWithParam(BMDDeckLinkConfigurationID, uint64_t, bool *) override { return E_INVALIDARG; }
  HRESULT SetIntWithParam(BMDDeckLinkConfigurationID, uint64_t, int64_t) override { std::abort(); }
  HRESULT GetIntWithParam(BMDDeckLinkConfigurationID, uint64_t, int64_t *) override { return E_INVALIDARG; }
  HRESULT SetFloatWithParam(BMDDeckLinkConfigurationID, uint64_t, double) override { std::abort(); }
  HRESULT GetFloatWithParam(BMDDeckLinkConfigurationID, uint64_t, double *) override { return E_INVALIDARG; }
  HRESULT SetStringWithParam(BMDDeckLinkConfigurationID, uint64_t, CFStringRef) override { std::abort(); }
  HRESULT GetStringWithParam(BMDDeckLinkConfigurationID, uint64_t, CFStringRef *) override { return E_INVALIDARG; }
  HRESULT WriteConfigurationToPreferences(void) override { std::abort(); }
};

class FakeDevice final : public RefCounted<FakeDevice>, public IDeckLink {
public:
  FAKE_COM_REFCOUNT(FakeDevice)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkOutput, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkOutput *>(new FakeOutput);
      return S_OK;
    }
    if (memcmp(&iid, &IID_IDeckLinkProfileAttributes, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkProfileAttributes *>(new FakeProfileAttributes);
      return S_OK;
    }
    if (memcmp(&iid, &IID_IDeckLinkConfiguration, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkConfiguration *>(new FakeConfiguration);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetModelName(CFStringRef *name) override {
    *name = CFSTR("EyeQ fake output device");
    CFRetain(*name);
    return S_OK;
  }
  HRESULT GetDisplayName(CFStringRef *name) override { return GetModelName(name); }
};

class FakeDeviceIterator final : public RefCounted<FakeDeviceIterator>, public IDeckLinkIterator {
  bool returned_ = false;

public:
  FAKE_COM_REFCOUNT(FakeDeviceIterator)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkIterator, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkIterator *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT Next(IDeckLink **device) override {
    if (device == nullptr) {
      return E_POINTER;
    }
    if (returned_) {
      *device = nullptr;
      return S_FALSE;
    }
    returned_ = true;
    *device = new FakeDevice;
    return S_OK;
  }
};

class FakeApiInformation final : public RefCounted<FakeApiInformation>, public IDeckLinkAPIInformation {
public:
  FAKE_COM_REFCOUNT(FakeApiInformation)
  HRESULT QueryInterface(REFIID iid, void **out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (memcmp(&iid, &IID_IDeckLinkAPIInformation, sizeof(iid)) == 0) {
      AddRef();
      *out = static_cast<IDeckLinkAPIInformation *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT GetFlag(BMDDeckLinkAPIInformationID, bool *) override { return E_INVALIDARG; }
  HRESULT GetInt(BMDDeckLinkAPIInformationID id, int64_t *value) override {
    if (id != BMDDeckLinkAPIVersion) {
      return E_INVALIDARG;
    }
    *value = (int64_t{16} << 24) | (int64_t{4} << 16);
    return S_OK;
  }
  HRESULT GetFloat(BMDDeckLinkAPIInformationID, double *) override { return E_INVALIDARG; }
  HRESULT GetString(BMDDeckLinkAPIInformationID, CFStringRef *) override { return E_INVALIDARG; }
};

} // namespace

void ResetFakeOutput() {
  if (g_state.registered != nullptr) {
    g_state.registered->Release();
    g_state.registered = nullptr;
  }
  if (g_state.late != nullptr) {
    g_state.late->Release();
    g_state.late = nullptr;
  }
  for (IDeckLinkMutableVideoFrame *frame : g_state.created) {
    frame->Release();
  }
  g_state = FakeState();
  g_state.connection_mask = static_cast<int64_t>(bmdVideoConnectionSDI) | static_cast<int64_t>(bmdVideoConnectionHDMI);
}

void SetOutputFault(OutputFault fault) { g_state.fault = fault; }

const FakeOutputStats &OutputStats() { return g_state.stats; }

void SetScheduleReentrant(bool enabled) { g_state.schedule_reentrant = enabled; }

bool DeliverCompletion(int frame_index, int result) {
  if (g_state.registered == nullptr || frame_index < 0 || frame_index >= static_cast<int>(g_state.created.size())) {
    return false;
  }
  DeliverScheduledFrameCompleted(g_state.registered, g_state.created[frame_index],
                                 static_cast<BMDOutputFrameCompletionResult>(result));
  return true;
}

bool DeliverPlaybackStopped() {
  if (g_state.registered == nullptr) {
    return false;
  }
  DeliverScheduledPlaybackStopped(g_state.registered);
  return true;
}

int CreatedFrameCount() { return static_cast<int>(g_state.created.size()); }

void CaptureRegisteredCallbackForLateDelivery() {
  if (g_state.registered == nullptr) {
    return;
  }
  if (g_state.late != nullptr) {
    g_state.late->Release();
  }
  g_state.late = g_state.registered;
  g_state.late->AddRef();
}

bool DeliverLateCompletion(int frame_index, int result) {
  if (g_state.late == nullptr || frame_index < 0 || frame_index >= static_cast<int>(g_state.created.size())) {
    return false;
  }
  DeliverScheduledFrameCompleted(g_state.late, g_state.created[frame_index],
                                 static_cast<BMDOutputFrameCompletionResult>(result));
  return true;
}

const std::vector<uint8_t> &LastScheduledBytes() { return g_state.last_scheduled_bytes; }

int LastScheduledFrameIndex() { return g_state.last_scheduled; }

void SetNativeRunning(bool running) { g_state.native_running = running; }

void SetConnectionQueryFails(bool fails) { g_state.connection_query_fails = fails; }

void SetStoppedNotificationsOnDisable(int count) { g_state.stopped_notifications_on_disable = count; }

void SetDisplayMode(long width, long height, int64_t frame_duration, int64_t time_scale, bool progressive) {
  g_state.display_mode.width = width;
  g_state.display_mode.height = height;
  g_state.display_mode.frame_duration = frame_duration;
  g_state.display_mode.time_scale = time_scale;
  g_state.display_mode.progressive = progressive;
  g_state.display_mode.mode = progressive ? bmdMode4K2160p50 : bmdModeHD1080i50;
}

void SetCreateFrameFailAt(int create_call_index) { g_state.create_frame_fail_at = create_call_index; }

void SetGetSizeFailAt(int create_call_index) { g_state.get_size_fail_at = create_call_index; }

void SetMetadataQueryFailAt(int create_call_index) { g_state.metadata_query_fail_at = create_call_index; }

void SetCompletionGate(void (*gate)(void *), void *context) {
  g_state.completion_gate = gate;
  g_state.completion_gate_context = context;
}

const std::vector<FakeMetadataSet> &MetadataSets() { return g_state.metadata_sets; }

int LiveObjectCount() { return g_live_objects.load(); }

int BuffersReleasedWhileAccessing() { return g_buffers_released_accessing.load(); }

// Named helpers so the global C factory functions can reach the anonymous-namespace
// fake types without relying on qualified lookup through a using-directive.
IDeckLinkIterator *MakeFakeIterator() {
  if (g_state.fault == OutputFault::IteratorUnavailable) {
    return nullptr;
  }
  return new FakeDeviceIterator;
}

IDeckLinkAPIInformation *MakeFakeApiInformation() { return new FakeApiInformation; }

} // namespace eyeq_decklink_test

// SDK dispatch entry points replaced by the fake.
IDeckLinkIterator *CreateDeckLinkIteratorInstance() { return eyeq_decklink_test::MakeFakeIterator(); }

IDeckLinkAPIInformation *CreateDeckLinkAPIInformationInstance() { return eyeq_decklink_test::MakeFakeApiInformation(); }
