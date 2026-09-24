// Death / lifecycle tests: the frozen contract requires a fatal termination when an
// un-closed DeckLinkOutput is destroyed, while a fully rolled-back Open stays safe.
// These run in forked children so the parent can observe the termination.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fake_decklink_output.hpp"
#include "output/decklink_backend.hpp"

using namespace EYEQ;
using eyeq_decklink_test::BuffersReleasedWhileAccessing;
using eyeq_decklink_test::OutputFault;
using eyeq_decklink_test::ResetFakeOutput;
using eyeq_decklink_test::SetOutputFault;

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

int RunChild(void (*child)()) {
  const pid_t pid = fork();
  if (pid == 0) {
    child();
    _exit(0);
  }
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  return status;
}

// An unknown CPU-access state (EndAccess failed) cannot be safely reclaimed, so the
// session stays Failed and destroying it must terminate.
void ChildEndAccessFailure() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x01);
    SetOutputFault(OutputFault::EndAccessFails);
    const ScheduleOutcome outcome =
        output.TrySchedule(PackedFrameView{bytes.data(), bytes.size()}, SubmissionInfo{}, 1000, 1000);
    if (outcome != ScheduleOutcome::DeviceError) {
      _exit(3);
    }
    if (output.Close(0) != CloseStatus::DeviceError) {
      _exit(4);
    }
  } // destructor must terminate here
}

// A failed still-frame EndAccess leaves the buffer access state unknown: Close must
// not report Closed, the still buffer must never be released during access, and the
// destructor must terminate rather than free a buffer the device may still own.
void ChildStillEndAccessFailure() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x02);
    SetOutputFault(OutputFault::EndAccessFails);
    if (output.DisplayStill(PackedFrameView{bytes.data(), bytes.size()}, SubmissionInfo{}) !=
        StillOutcome::DeviceError) {
      _exit(6);
    }
    if (output.Close(0) != CloseStatus::DeviceError) {
      _exit(7);
    }
    if (BuffersReleasedWhileAccessing() != 0) {
      _exit(8);
    }
  } // destructor must terminate here
}

// GetBytes failing and EndAccess failing together is also an unknown access state.
void ChildGetBytesEndAccessFailure() {
  ResetFakeOutput();
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    CHECK(output.Open(MakeRequest(), &resolved, &error));
    std::vector<uint8_t> bytes(resolved.layout.image_bytes, 0x03);
    SetOutputFault(OutputFault::GetBytesAndEndAccessFail);
    if (output.TrySchedule(PackedFrameView{bytes.data(), bytes.size()}, SubmissionInfo{}, 1000, 1000) !=
        ScheduleOutcome::DeviceError) {
      _exit(9);
    }
    if (output.Close(0) != CloseStatus::DeviceError) {
      _exit(10);
    }
    if (BuffersReleasedWhileAccessing() != 0) {
      _exit(11);
    }
  } // destructor must terminate here
}

// A failed Open that fully rolls back to Closed may be destroyed safely.
void ChildRollbackSucceeds() {
  ResetFakeOutput();
  SetOutputFault(OutputFault::SetCallbackFails);
  {
    DeckLinkOutput output;
    ResolvedOutput resolved;
    std::string error;
    if (output.Open(MakeRequest(), &resolved, &error)) {
      _exit(5);
    }
  }
  _exit(0);
}

} // namespace

int main() {
  const int failed_status = RunChild(ChildEndAccessFailure);
  CHECK(WIFSIGNALED(failed_status));
  CHECK(WTERMSIG(failed_status) == SIGABRT);

  const int still_status = RunChild(ChildStillEndAccessFailure);
  CHECK(WIFSIGNALED(still_status));
  CHECK(WTERMSIG(still_status) == SIGABRT);

  const int combined_status = RunChild(ChildGetBytesEndAccessFailure);
  CHECK(WIFSIGNALED(combined_status));
  CHECK(WTERMSIG(combined_status) == SIGABRT);

  const int rolled_back_status = RunChild(ChildRollbackSucceeds);
  CHECK(WIFEXITED(rolled_back_status));
  CHECK(WEXITSTATUS(rolled_back_status) == 0);

  std::printf("decklink output death test: OK\n");
  return 0;
}
