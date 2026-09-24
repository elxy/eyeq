#pragma once

// Process-wide fake DeckLink COM objects used by the backend fault-injection tests.
//
// The test executables that use this file do NOT link the SDK dispatch: they provide
// the two factory functions the catalog calls, so the tests exercise the real
// catalog code with deterministic query results and failures. No device, driver or
// GPU is touched, and every non-query IDeckLinkOutput method aborts.

namespace eyeq_decklink_test {

enum class FakeFault {
  None,
  IteratorUnavailable,       // factory returns nullptr
  DeviceNextFails,           // first device Next -> E_FAIL
  DeviceNextFailsAfterFirst, // one device, then E_FAIL
  ModeNextFails,             // first mode Next -> E_FAIL
  ModeNextFailsAfterFirst,   // one mode, then E_FAIL
  ModeEndOkNull,             // normal end as S_OK with a null mode (SDK 16.0 documents this
                             // alongside S_FALSE)
  SupportFails,              // DoesSupportVideoMode -> E_FAIL
  SupportFailsAfterFirst,    // one mode queried, then DoesSupportVideoMode -> E_FAIL
  SupportFalse,              // DoesSupportVideoMode -> S_OK with supported=false
  FrameRateFails,            // GetFrameRate -> E_FAIL
  FrameRateInvalid,          // GetFrameRate -> S_OK with a zero time base
  AttributeReadFails,        // VideoIOSupport/Duplex GetInt -> E_FAIL (persistent id still works)
};

// Configures the fake used by the next catalog call.
void SetFault(FakeFault fault);

// Number of live fake COM objects. The test asserts it returns to zero, which proves
// every reference the catalog acquired was released on both success and failure.
int LiveObjectCount();

} // namespace eyeq_decklink_test
