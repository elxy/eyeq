#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "output/frame_sink.hpp"
#include "output/output_profile.hpp"

namespace EYEQ {

// Read-only description of one DeckLink device.
struct DeviceInfo {
  DeviceId id; // id.persistent_id is empty when has_persistent_id is false
  bool has_persistent_id = false;
  int index = 0;            // enumeration index; always valid
  std::string display_name; // IDeckLink::GetDisplayName (may be empty)
  std::string model_name;   // IDeckLink::GetModelName (may be empty)
  // false -> no IDeckLinkProfileAttributes interface, so all capability fields below
  // are unknown regardless of their known flags.
  bool profile_attributes_available = false;
  // Capability fields are valid only when their *_known flag is true. A failed
  // attribute read leaves the value at its default and the known flag false; it is
  // never reported as a definite yes/no.
  bool supports_playback = false;
  bool supports_playback_known = false;
  bool supports_capture = false;
  bool supports_capture_known = false;
  bool active = false; // false when the sub-device has no active connectors
  bool active_known = false;
};

// Result of a read-only device enumeration.
struct DeviceCatalog {
  enum class Status {
    Ok,
    IteratorUnavailable, // iterator could not be created (framework/driver missing or API unavailable)
    InternalError,       // a query failed partway; devices must not be treated as complete
  };
  Status status = Status::Ok;
  std::string error;       // human-readable when status != Ok
  std::string api_version; // e.g. "16.4.0"; empty when unavailable
  std::vector<DeviceInfo> devices;
};

// SDK output backend. Phase 1A-2 implements the real frame pool, CPU copy,
// scheduling, still frame and shutdown. All DeckLink COM types stay inside the
// implementation translation unit, so this header includes no SDK type.
class DeckLinkOutput final : public FrameSink {
public:
  DeckLinkOutput();
  ~DeckLinkOutput() override;

  DeckLinkOutput(const DeckLinkOutput &) = delete;
  DeckLinkOutput &operator=(const DeckLinkOutput &) = delete;
  DeckLinkOutput(DeckLinkOutput &&) = delete;
  DeckLinkOutput &operator=(DeckLinkOutput &&) = delete;
  // Enumerates devices without opening an output or changing device configuration.
  static DeviceCatalog Enumerate();

  // Lists display modes natively supported for the given connection and pixel
  // format. Returns false with *error on a hard failure (invalid selector, device
  // not found, output interface missing, query failure). Returns true with an empty
  // list when the device exists but no mode matched. Only native support is
  // reported: a mode that would require output conversion is not included, and
  // r210 support here does not imply RGB 4:4:4 on the wire.
  static bool ListModes(const DeviceId &device, VideoConnection connection, PixelFormat format,
                        std::vector<MonitorMode> *out, std::string *error);

  // Opens the selected device, validates the request, creates the SDK frame pool
  // and still frame, and registers the completion callback. No success stub: any
  // native failure is reported and rolled back. Must be called before the output
  // methods below.
  bool Open(const OutputRequest &request, ResolvedOutput *resolved, std::string *error);
  ScheduleOutcome TrySchedule(const PackedFrameView &view, const SubmissionInfo &info, int64_t display_time,
                              int64_t display_duration) override;
  StillOutcome DisplayStill(const PackedFrameView &view, const SubmissionInfo &info) override;
  bool PollEvent(OutputEvent *out) override;
  bool StartScheduled(int64_t start_time, std::string *error) override;
  bool StopScheduled(int64_t stop_at, int64_t *actual_stop, std::string *error) override;
  bool QueryClock(int64_t time_scale, int64_t *stream_time, double *speed, std::string *error) override;
  CloseStatus Close(int drain_timeout_ms) override;

  // Diagnostic: number of pool slots that are not Free (submitted, in flight,
  // settled-unknown or quarantined). Used by shutdown diagnostics and tests.
  uint32_t unreconciled_frame_count() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace EYEQ
