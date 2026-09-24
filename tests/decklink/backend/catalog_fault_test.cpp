// Deterministic backend tests. They replace the SDK dispatch with the local fake in
// fake_decklink.cpp, so the real catalog error handling is exercised without a
// device, driver or GPU. Every test also asserts that no fake COM object leaked.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fake_decklink.hpp"
#include "output/decklink_backend.hpp"

using namespace EYEQ;
using eyeq_decklink_test::FakeFault;
using eyeq_decklink_test::LiveObjectCount;
using eyeq_decklink_test::SetFault;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

// A hard failure must carry an HRESULT/hex status, not just an opaque message.
bool MentionsStatus(const std::string &error) { return error.find("0x") != std::string::npos; }

void CheckNoLeaks() { CHECK(LiveObjectCount() == 0); }

DeviceId ByIndex(int index) {
  DeviceId id;
  id.index = index;
  return id;
}

DeviceId ByPersistentId(const std::string &persistent_id) {
  DeviceId id;
  id.persistent_id = persistent_id;
  return id;
}

std::vector<MonitorMode> QueryModes(const DeviceId &selector, bool *ok, std::string *error) {
  std::vector<MonitorMode> modes;
  error->clear();
  *ok = DeckLinkOutput::ListModes(selector, VideoConnection::SDI, PixelFormat::V210, &modes, error);
  return modes;
}

} // namespace

int main() {
  // --- F2: enumeration failures are not EOF, and partial lists are discarded. ---
  {
    SetFault(FakeFault::None);
    const DeviceCatalog catalog = DeckLinkOutput::Enumerate();
    CHECK(catalog.status == DeviceCatalog::Status::Ok);
    CHECK(catalog.error.empty());
    CHECK(catalog.api_version == "16.4.0");
    CHECK(catalog.devices.size() == 2);
    CHECK(catalog.devices[0].index == 0);
    CHECK(catalog.devices[0].has_persistent_id && catalog.devices[0].id.persistent_id == "2697367084");
    CHECK(catalog.devices[0].profile_attributes_available);
    CHECK(catalog.devices[0].supports_playback_known && catalog.devices[0].supports_playback);
    CHECK(catalog.devices[0].supports_capture_known && catalog.devices[0].supports_capture);
    CHECK(catalog.devices[0].active_known && catalog.devices[0].active);
    CHECK(catalog.devices[1].index == 1);
    CHECK(catalog.devices[1].has_persistent_id && catalog.devices[1].id.persistent_id == "123456789");
    CHECK(catalog.devices[1].supports_playback_known && catalog.devices[1].supports_playback);
    CHECK(catalog.devices[1].supports_capture_known && !catalog.devices[1].supports_capture);
    CHECK(catalog.devices[1].active_known && !catalog.devices[1].active);
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::IteratorUnavailable);
    const DeviceCatalog catalog = DeckLinkOutput::Enumerate();
    CHECK(catalog.status == DeviceCatalog::Status::IteratorUnavailable);
    CHECK(catalog.devices.empty());
    CHECK(!catalog.error.empty());
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::DeviceNextFails);
    const DeviceCatalog catalog = DeckLinkOutput::Enumerate();
    CHECK(catalog.status == DeviceCatalog::Status::InternalError);
    CHECK(catalog.devices.empty());
    CHECK(!catalog.error.empty() && MentionsStatus(catalog.error));
    CheckNoLeaks();
  }
  {
    // One device was read before the failure; it must not be presented as complete.
    SetFault(FakeFault::DeviceNextFailsAfterFirst);
    const DeviceCatalog catalog = DeckLinkOutput::Enumerate();
    CHECK(catalog.status == DeviceCatalog::Status::InternalError);
    CHECK(catalog.devices.empty());
    CHECK(!catalog.error.empty() && MentionsStatus(catalog.error));
    CheckNoLeaks();
  }
  {
    // F3: a failed capability read is unknown, not a definite no/yes.
    SetFault(FakeFault::AttributeReadFails);
    const DeviceCatalog catalog = DeckLinkOutput::Enumerate();
    CHECK(catalog.status == DeviceCatalog::Status::Ok);
    CHECK(catalog.devices.size() == 2);
    const DeviceInfo &device = catalog.devices[0];
    CHECK(device.profile_attributes_available);
    CHECK(device.has_persistent_id && device.id.persistent_id == "2697367084");
    CHECK(!device.supports_playback_known);
    CHECK(!device.supports_capture_known);
    CHECK(!device.active_known);
    CheckNoLeaks();
  }

  // --- F2: mode-query failures are hard failures, not empty successes. ---
  bool ok = false;
  std::string error;
  {
    SetFault(FakeFault::None);
    const std::vector<MonitorMode> modes = QueryModes(ByIndex(0), &ok, &error);
    CHECK(ok && modes.size() == 2);
    CHECK(modes[0].frame_duration == 1000 && modes[0].time_scale == 50000);
    CheckNoLeaks();
    // Selecting by the large persistent id works too.
    const std::vector<MonitorMode> by_id = QueryModes(ByPersistentId("2697367084"), &ok, &error);
    CHECK(ok && by_id.size() == 2);
    CheckNoLeaks();
    // Index 1 is a valid boundary and selects the second device.
    QueryModes(ByIndex(1), &ok, &error);
    CHECK(ok);
    CheckNoLeaks();
  }
  {
    // R2: S_OK with a null mode is also a normal end of the display-mode list; the modes
    // read before it must be kept (contrast with the S_FALSE end used above).
    SetFault(FakeFault::ModeEndOkNull);
    const std::vector<MonitorMode> modes = QueryModes(ByIndex(0), &ok, &error);
    CHECK(ok && modes.size() == 2);
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::ModeNextFails);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty() && MentionsStatus(error));
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::ModeNextFailsAfterFirst);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty() && MentionsStatus(error));
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::SupportFails);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty() && MentionsStatus(error));
    CheckNoLeaks();
  }
  {
    // The first mode was already accepted before the failure: nothing is published.
    SetFault(FakeFault::SupportFailsAfterFirst);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty() && MentionsStatus(error));
    CheckNoLeaks();
  }
  {
    // S_OK with supported=false is a legitimate empty result, not a failure.
    SetFault(FakeFault::SupportFalse);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(ok);
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::FrameRateFails);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty() && MentionsStatus(error));
    CheckNoLeaks();
  }
  {
    // A zero time base must never be published as a valid mode.
    SetFault(FakeFault::FrameRateInvalid);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty());
    CheckNoLeaks();
  }
  {
    SetFault(FakeFault::IteratorUnavailable);
    CHECK(QueryModes(ByIndex(0), &ok, &error).empty());
    CHECK(!ok && !error.empty());
    CheckNoLeaks();
  }

  // --- R1/F1/F2: selector handling at the library boundary. ---
  {
    SetFault(FakeFault::None);
    // A persistent id that looks like an index must not fall back to index 0.
    CHECK(QueryModes(ByPersistentId("4294967296"), &ok, &error).empty());
    CHECK(!ok && error.find("persistent id") != std::string::npos);
    CheckNoLeaks();

    // The DeviceId that Enumerate() produces carries both a persistent id and an index;
    // it must be directly usable. The frozen DeviceId contract prefers persistent_id and
    // uses index only as a fallback, so coexistence is not an error.
    const DeviceCatalog catalog = DeckLinkOutput::Enumerate();
    CHECK(catalog.devices.size() == 2);
    for (const DeviceInfo &device : catalog.devices) {
      CHECK(!device.id.persistent_id.empty());
      CHECK(device.id.index == device.index);
      const std::vector<MonitorMode> modes = QueryModes(device.id, &ok, &error);
      CHECK(ok && modes.size() == 2);
      CheckNoLeaks();
    }

    // A valid persistent id wins even when paired with a stale/wrong index.
    DeviceId stale_index = ByPersistentId("2697367084");
    stale_index.index = 1 << 20;
    CHECK(QueryModes(stale_index, &ok, &error).size() == 2);
    CHECK(ok);
    CheckNoLeaks();

    // An unknown persistent id fails even when a valid index is also set: no fallback.
    DeviceId unknown_id = ByPersistentId("4294967296");
    unknown_id.index = 0;
    CHECK(QueryModes(unknown_id, &ok, &error).empty());
    CHECK(!ok && error.find("persistent id") != std::string::npos);
    CheckNoLeaks();

    // Out-of-range index is not found, not truncated.
    CHECK(QueryModes(ByIndex(1 << 20), &ok, &error).empty());
    CHECK(!ok && !error.empty());
    CheckNoLeaks();
  }

  std::printf("decklink catalog fault test: OK\n");
  return 0;
}
