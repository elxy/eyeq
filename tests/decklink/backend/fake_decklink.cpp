// Local fake SDK dispatch for the backend fault-injection tests. See fake_decklink.hpp.
#include "fake_decklink.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DeckLinkAPI.h"

namespace eyeq_decklink_test {

FakeFault g_fault = FakeFault::None;
int g_live_objects = 0;

// 3840x2160@50 and 1920x1080@50, both 1000/50000.
constexpr BMDDisplayMode kFirstMode = bmdMode4K2160p50;
constexpr BMDDisplayMode kSecondMode = bmdModeHD1080p50;

template <typename Interface> class RefCounted : public Interface {
  ULONG refs_ = 1;

public:
  RefCounted() { ++g_live_objects; }
  virtual ~RefCounted() { --g_live_objects; }
  HRESULT QueryInterface(REFIID, void **out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override {
    const ULONG count = --refs_;
    if (count == 0) {
      delete this;
    }
    return count;
  }
};

class FakeMode : public RefCounted<IDeckLinkDisplayMode> {
  BMDDisplayMode mode_;

public:
  explicit FakeMode(BMDDisplayMode mode) : mode_(mode) {}
  HRESULT GetName(CFStringRef *out) override {
    *out = CFSTR("fake mode");
    CFRetain(*out);
    return S_OK;
  }
  BMDDisplayMode GetDisplayMode() override { return mode_; }
  long GetWidth() override { return mode_ == kFirstMode ? 3840 : 1920; }
  long GetHeight() override { return mode_ == kFirstMode ? 2160 : 1080; }
  HRESULT GetFrameRate(BMDTimeValue *frame_duration, BMDTimeScale *time_scale) override {
    if (g_fault == FakeFault::FrameRateFails) {
      return E_FAIL;
    }
    *frame_duration = g_fault == FakeFault::FrameRateInvalid ? 0 : 1000;
    *time_scale = g_fault == FakeFault::FrameRateInvalid ? 0 : 50000;
    return S_OK;
  }
  BMDFieldDominance GetFieldDominance() override { return bmdProgressiveFrame; }
  BMDDisplayModeFlags GetFlags() override { return 0; }
};

class FakeModeIterator : public RefCounted<IDeckLinkDisplayModeIterator> {
  int returned_ = 0;

public:
  HRESULT Next(IDeckLinkDisplayMode **out) override {
    *out = nullptr;
    if (g_fault == FakeFault::ModeNextFails) {
      return E_FAIL;
    }
    if (returned_ == 0) {
      ++returned_;
      *out = new FakeMode(kFirstMode);
      return S_OK;
    }
    if (g_fault == FakeFault::ModeNextFailsAfterFirst) {
      // The fault lands after one mode was already produced.
      return E_FAIL;
    }
    if (returned_ == 1) {
      ++returned_;
      *out = new FakeMode(kSecondMode);
      return S_OK;
    }
    if (g_fault == FakeFault::ModeEndOkNull) {
      // SDK 16.0 documents a null mode with S_OK as a normal end of the list.
      *out = nullptr;
      return S_OK;
    }
    return S_FALSE;
  }
};

class FakeOutput : public RefCounted<IDeckLinkOutput> {
  int support_calls_ = 0;

public:
  HRESULT DoesSupportVideoMode(BMDVideoConnection connection, BMDDisplayMode requested, BMDPixelFormat format,
                               BMDVideoOutputConversionMode conversion, BMDSupportedVideoModeFlags flags,
                               BMDDisplayMode *actual, bool *supported) override {
    // Guard against the catalog changing the query arguments silently.
    if ((connection != bmdVideoConnectionSDI && connection != bmdVideoConnectionHDMI) ||
        (format != bmdFormat10BitYUV && format != bmdFormat10BitRGB) || conversion != bmdNoVideoOutputConversion ||
        flags != bmdSupportedVideoModeDefault) {
      std::abort();
    }
    ++support_calls_;
    if (g_fault == FakeFault::SupportFails || (g_fault == FakeFault::SupportFailsAfterFirst && support_calls_ > 1)) {
      return E_FAIL;
    }
    *actual = requested;
    *supported = g_fault != FakeFault::SupportFalse;
    return S_OK;
  }
  HRESULT GetDisplayMode(BMDDisplayMode, IDeckLinkDisplayMode **out) override {
    *out = new FakeMode(kFirstMode);
    return S_OK;
  }
  HRESULT GetDisplayModeIterator(IDeckLinkDisplayModeIterator **out) override {
    *out = new FakeModeIterator;
    return S_OK;
  }

  // Every non-query path aborts: the fake must never make the tests look like device
  // output was enabled.
  HRESULT SetScreenPreviewCallback(IDeckLinkScreenPreviewCallback *) override { std::abort(); }
  HRESULT EnableVideoOutput(BMDDisplayMode, BMDVideoOutputFlags) override { std::abort(); }
  HRESULT DisableVideoOutput(void) override { std::abort(); }
  HRESULT CreateVideoFrame(int32_t, int32_t, int32_t, BMDPixelFormat, BMDFrameFlags,
                           IDeckLinkMutableVideoFrame **) override {
    std::abort();
  }
  HRESULT CreateVideoFrameWithBuffer(int32_t, int32_t, int32_t, BMDPixelFormat, BMDFrameFlags, IDeckLinkVideoBuffer *,
                                     IDeckLinkMutableVideoFrame **) override {
    std::abort();
  }
  HRESULT RowBytesForPixelFormat(BMDPixelFormat, int32_t, int32_t *) override { std::abort(); }
  HRESULT CreateAncillaryData(BMDPixelFormat, IDeckLinkVideoFrameAncillary **) override { std::abort(); }
  HRESULT DisplayVideoFrameSync(IDeckLinkVideoFrame *) override { std::abort(); }
  HRESULT ScheduleVideoFrame(IDeckLinkVideoFrame *, BMDTimeValue, BMDTimeValue, BMDTimeScale) override { std::abort(); }
  HRESULT SetScheduledFrameCompletionCallback(IDeckLinkVideoOutputCallback *) override { std::abort(); }
  HRESULT GetBufferedVideoFrameCount(uint32_t *) override { std::abort(); }
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
  HRESULT StartScheduledPlayback(BMDTimeValue, BMDTimeScale, double) override { std::abort(); }
  HRESULT StopScheduledPlayback(BMDTimeValue, BMDTimeValue *, BMDTimeScale) override { std::abort(); }
  HRESULT IsScheduledPlaybackRunning(bool *) override { std::abort(); }
  HRESULT GetScheduledStreamTime(BMDTimeScale, BMDTimeValue *, double *) override { std::abort(); }
  HRESULT GetReferenceStatus(BMDReferenceStatus *) override { std::abort(); }
  HRESULT GetHardwareReferenceClock(BMDTimeScale, BMDTimeValue *, BMDTimeValue *, BMDTimeValue *) override {
    std::abort();
  }
  HRESULT GetFrameCompletionReferenceTimestamp(IDeckLinkVideoFrame *, BMDTimeScale, BMDTimeValue *) override {
    std::abort();
  }
};

class FakeAttributes : public RefCounted<IDeckLinkProfileAttributes> {
  int64_t persistent_id_;
  int64_t video_io_;
  int64_t duplex_;

public:
  FakeAttributes(int64_t persistent_id, int64_t video_io, int64_t duplex)
      : persistent_id_(persistent_id), video_io_(video_io), duplex_(duplex) {}
  HRESULT GetFlag(BMDDeckLinkAttributeID, bool *) override { return E_INVALIDARG; }
  HRESULT GetInt(BMDDeckLinkAttributeID id, int64_t *out) override {
    if (id == BMDDeckLinkPersistentID) {
      *out = persistent_id_;
      return S_OK;
    }
    if (g_fault == FakeFault::AttributeReadFails) {
      return E_FAIL;
    }
    if (id == BMDDeckLinkVideoIOSupport) {
      *out = video_io_;
      return S_OK;
    }
    if (id == BMDDeckLinkDuplex) {
      *out = duplex_;
      return S_OK;
    }
    return E_INVALIDARG;
  }
  HRESULT GetFloat(BMDDeckLinkAttributeID, double *) override { return E_INVALIDARG; }
  HRESULT GetString(BMDDeckLinkAttributeID, CFStringRef *) override { return E_INVALIDARG; }
  HRESULT GetStringWithParam(BMDDeckLinkAttributeID, uint64_t, CFStringRef *) override { return E_INVALIDARG; }
};

class FakeDevice : public RefCounted<IDeckLink> {
  int64_t persistent_id_;
  int64_t video_io_;
  int64_t duplex_;

public:
  FakeDevice(int64_t persistent_id, int64_t video_io, int64_t duplex)
      : persistent_id_(persistent_id), video_io_(video_io), duplex_(duplex) {}
  HRESULT GetModelName(CFStringRef *out) override {
    *out = CFSTR("EyeQ fake device");
    CFRetain(*out);
    return S_OK;
  }
  HRESULT GetDisplayName(CFStringRef *out) override { return GetModelName(out); }
  HRESULT QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (std::memcmp(&iid, &IID_IDeckLinkProfileAttributes, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkProfileAttributes *>(new FakeAttributes(persistent_id_, video_io_, duplex_));
      return S_OK;
    }
    if (std::memcmp(&iid, &IID_IDeckLinkOutput, sizeof(iid)) == 0) {
      *out = static_cast<IDeckLinkOutput *>(new FakeOutput);
      return S_OK;
    }
    return E_NOINTERFACE;
  }
};

class FakeDeviceIterator : public RefCounted<IDeckLinkIterator> {
  int returned_ = 0;

public:
  HRESULT Next(IDeckLink **out) override {
    *out = nullptr;
    if (g_fault == FakeFault::DeviceNextFails) {
      return E_FAIL;
    }
    if (g_fault == FakeFault::DeviceNextFailsAfterFirst && returned_ >= 1) {
      return E_FAIL;
    }
    if (returned_ == 0) {
      ++returned_;
      // Large persistent id, matching the real UltraStudio-class devices.
      *out = new FakeDevice(2697367084LL, bmdDeviceSupportsPlayback | bmdDeviceSupportsCapture, bmdDuplexFull);
      return S_OK;
    }
    if (returned_ == 1) {
      ++returned_;
      *out = new FakeDevice(123456789LL, bmdDeviceSupportsPlayback, bmdDuplexInactive);
      return S_OK;
    }
    return S_FALSE;
  }
};

class FakeApiInformation : public RefCounted<IDeckLinkAPIInformation> {
public:
  HRESULT GetFlag(BMDDeckLinkAPIInformationID, bool *) override { return E_INVALIDARG; }
  HRESULT GetInt(BMDDeckLinkAPIInformationID id, int64_t *out) override {
    if (id != BMDDeckLinkAPIVersion) {
      return E_INVALIDARG;
    }
    *out = (int64_t{16} << 24) | (int64_t{4} << 16) | (int64_t{0} << 8);
    return S_OK;
  }
  HRESULT GetFloat(BMDDeckLinkAPIInformationID, double *) override { return E_INVALIDARG; }
  HRESULT GetString(BMDDeckLinkAPIInformationID, CFStringRef *) override { return E_INVALIDARG; }
};

void SetFault(FakeFault fault) { g_fault = fault; }

int LiveObjectCount() { return g_live_objects; }

} // namespace eyeq_decklink_test

// The two SDK dispatch entry points used by the catalog. Defined here instead of
// linking DeckLinkAPIDispatch.cpp so the tests are deterministic and driver-free.
IDeckLinkIterator *CreateDeckLinkIteratorInstance() {
  if (eyeq_decklink_test::g_fault == eyeq_decklink_test::FakeFault::IteratorUnavailable) {
    return nullptr;
  }
  return new eyeq_decklink_test::FakeDeviceIterator;
}

IDeckLinkAPIInformation *CreateDeckLinkAPIInformationInstance() { return new eyeq_decklink_test::FakeApiInformation; }
