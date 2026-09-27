# Test wiring for EyeQ. All test targets are optional and default to OFF, so the
# normal player build is unaffected. Human-readable documentation: tests/README.md.
#
# The switches are declared here, next to the targets they control, so every
# test-related definition stays in one place. enable_testing() is called by the
# top-level CMakeLists.txt because ctest expects a test file in the build root.

option(EYEQ_BUILD_DECKLINK_TESTS "Build the DeckLink CPU/fake contract and backend tests (no GPU, no device)" OFF)
option(EYEQ_BUILD_DECKLINK_GPU_TESTS "Build the GPU packing tests (needs libplacebo and a real Vulkan device at runtime; no SDK)" OFF)
option(EYEQ_BUILD_DISPLAY_GPU_TESTS "Build the offscreen display/rendering tests (needs libplacebo and a real Vulkan device at runtime; no SDK, no window)" OFF)
option(EYEQ_BUILD_PLAYER_TESTS "Build the SyncPlayer frame publication/control tests (CPU only, no GPU/SDK/window)" OFF)

# DeckLink CPU/fake contract tests. No SDK, no GPU, no device.
if(EYEQ_BUILD_DECKLINK_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/decklink/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_DECKLINK_TESTS=ON but tests/decklink/CMakeLists.txt is missing")
  endif()
  add_subdirectory(decklink)

  # A-owned backend tests additionally need the SDK build.
  if(EYEQ_ENABLE_DECKLINK)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/decklink/backend/CMakeLists.txt")
      message(FATAL_ERROR "EYEQ_ENABLE_DECKLINK and EYEQ_BUILD_DECKLINK_TESTS are ON but tests/decklink/backend/CMakeLists.txt is missing")
    endif()
    add_subdirectory(decklink/backend)
  endif()
endif()

# GPU packing tests plus their CPU oracle. They need libplacebo and a real Vulkan
# device at runtime, but never the DeckLink SDK, so they can build and run with the
# SDK disabled and DECKLINK_SDK_ROOT unset. A missing/unsuitable GPU is reported as
# a test failure, not silently skipped.
if(EYEQ_BUILD_DECKLINK_GPU_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/decklink/packer/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_DECKLINK_GPU_TESTS=ON but tests/decklink/packer/CMakeLists.txt is missing")
  endif()
  add_subdirectory(decklink/packer)
endif()

# Offscreen display/rendering tests and the monitor composition-layer tests. They
# need libplacebo and a real Vulkan device at runtime but never the DeckLink SDK or
# a window, so they can build and run with the SDK disabled and DECKLINK_SDK_ROOT
# unset. A missing/unsuitable GPU is reported as a test failure, not silently skipped.
if(EYEQ_BUILD_DISPLAY_GPU_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/display/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_DISPLAY_GPU_TESTS=ON but tests/display/CMakeLists.txt is missing")
  endif()
  add_subdirectory(display)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/output/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_DISPLAY_GPU_TESTS=ON but tests/output/CMakeLists.txt is missing")
  endif()
  add_subdirectory(output)
endif()

# Player frame publication/control tests. They run the real SyncPlayer and
# VideoSource over locally synthesized YUV4MPEG2 files; no GPU, SDK, or window.
if(EYEQ_BUILD_PLAYER_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/player/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_PLAYER_TESTS=ON but tests/player/CMakeLists.txt is missing")
  endif()
  add_subdirectory(player)
endif()
