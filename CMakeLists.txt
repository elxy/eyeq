cmake_minimum_required(VERSION 3.24)

project(eyeq LANGUAGES C CXX)

# Get version number from Git
execute_process(
  COMMAND git describe --tags --always --dirty
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT GIT_VERSION)
  set(GIT_VERSION "unknown")
endif()
message(STATUS "EyeQ version: ${GIT_VERSION}")

# Optional DeckLink output backend (not part of the default player build).
option(EYEQ_ENABLE_DECKLINK "Enable the DeckLink SDK backend (read-only catalog plus scheduled output) and its tools" OFF)
option(EYEQ_BUILD_DECKLINK_TESTS "Build the DeckLink CPU/fake contract and backend tests (no GPU, no device)" OFF)
option(EYEQ_BUILD_DECKLINK_GPU_TESTS "Build the GPU packing tests (needs libplacebo and a real Vulkan device at runtime; no SDK)" OFF)
set(DECKLINK_SDK_ROOT "" CACHE PATH "Path to the Blackmagic DeckLink SDK root (read-only; used from phase 1-A)")

# macOS requires Objective-C++ support (ICC profile auto-detection)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS "YES")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
      "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DSPDLOG_ACTIVE_LEVEL=0")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DSPDLOG_ACTIVE_LEVEL=0")
endif()

# Add custom CMake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find dependencies
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  message(STATUS "spdlog not found, building from source via FetchContent")
  include(BuildSpdlog)
endif()
find_package(SDL3 REQUIRED)
find_package(Libplacebo REQUIRED)
find_package(SDL3_ttf REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avfilter avformat avutil swscale)

# CPU-only output contract library: public headers plus pure helpers. It has no SDK
# dependency and does not use the GPU; libplacebo is linked PUBLIC only because
# frame_packer.hpp declares pl_tex/pl_gpu parameters.
add_library(eyeq_output_contract STATIC
  ${CMAKE_SOURCE_DIR}/src/output/pixel_format.cpp
  ${CMAKE_SOURCE_DIR}/src/output/output_profile.cpp)
target_include_directories(eyeq_output_contract PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(eyeq_output_contract PUBLIC Libplacebo::Libplacebo)

# GPU frame packer: nonlinear full-range float RGB -> r210/v210 Video-range bytes,
# with a fixed staging ring and asynchronous pl_tex_download + pl_buf_poll readback.
# It only needs libplacebo and a caller-provided pl_gpu; the player is not wired to
# it yet and the DeckLink SDK is not a dependency. EXCLUDE_FROM_ALL keeps the default
# player build unaffected; the GPU tests pull it in when they are enabled.
add_library(eyeq_frame_packer STATIC EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/src/output/frame_packer.cpp)
target_include_directories(eyeq_frame_packer PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(eyeq_frame_packer PUBLIC eyeq_output_contract Libplacebo::Libplacebo)

# Optional DeckLink SDK backend. Phase 1A-1 builds only the read-only catalog and
# mode query; it never enables video output. The SDK is an external, read-only tree
# referenced by DECKLINK_SDK_ROOT; nothing is downloaded or modified.
if(EYEQ_ENABLE_DECKLINK)
  if(NOT APPLE)
    message(FATAL_ERROR "EYEQ_ENABLE_DECKLINK currently requires macOS")
  endif()
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "EYEQ_ENABLE_DECKLINK currently supports Apple Silicon (arm64) only")
  endif()
  if(NOT DECKLINK_SDK_ROOT)
    message(FATAL_ERROR "EYEQ_ENABLE_DECKLINK=ON requires -DDECKLINK_SDK_ROOT=<path-to-SDK>")
  endif()
  set(_decklink_include "${DECKLINK_SDK_ROOT}/Mac/include")
  set(_decklink_dispatch "${_decklink_include}/DeckLinkAPIDispatch.cpp")
  set(_decklink_api "${_decklink_include}/DeckLinkAPI.h")
  if(NOT EXISTS "${_decklink_api}" OR NOT EXISTS "${_decklink_dispatch}")
    message(FATAL_ERROR
      "DECKLINK_SDK_ROOT='${DECKLINK_SDK_ROOT}' does not contain "
      "Mac/include/DeckLinkAPI.h and Mac/include/DeckLinkAPIDispatch.cpp")
  endif()

  add_library(eyeq_decklink_backend STATIC
    "${_decklink_dispatch}"
    ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_output.cpp
    ${CMAKE_SOURCE_DIR}/src/output/decklink/monitor_mode.cpp)
  target_include_directories(eyeq_decklink_backend
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PRIVATE "${_decklink_include}")
  target_link_libraries(eyeq_decklink_backend
    PUBLIC eyeq_output_contract
    PRIVATE "-framework CoreFoundation")
  # The vendored SDK predates some modern warnings; keep the suppression scoped to the
  # SDK dispatch source instead of relaxing diagnostics for our own backend code.
  set_source_files_properties("${_decklink_dispatch}" PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")

  add_executable(decklink-catalog tools/decklink-catalog/main.cpp)
  target_link_libraries(decklink-catalog PRIVATE eyeq_decklink_backend)

  # Standalone deterministic test-signal generator. It drives the backend through the
  # FrameSink seam and does not include the player or the GPU packer.
  add_executable(decklink-test-signal
    tools/decklink-test-signal/main.cpp
    tools/decklink-test-signal/test_pattern.cpp
    tools/decklink-test-signal/test_signal_session.cpp)
  target_include_directories(decklink-test-signal PRIVATE tools/decklink-catalog)
  target_link_libraries(decklink-test-signal PRIVATE eyeq_decklink_backend)
endif()

# Collect source files
file(GLOB_RECURSE C_SOURCES "${CMAKE_SOURCE_DIR}/src/*.c")
file(GLOB_RECURSE CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
# src/output/ is built by its own library targets; never compile it into the player.
list(FILTER CXX_SOURCES EXCLUDE REGEX "/src/output/")

# Platform-specific source files
if(APPLE)
  # macOS: use .mm implementations, exclude stubs
  list(REMOVE_ITEM CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/display/platform_icc_stub.cpp")
  list(REMOVE_ITEM CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/display/platform_activate_stub.cpp")
  set(PLATFORM_SOURCES
    "${CMAKE_SOURCE_DIR}/src/display/platform_icc_mac.mm"
    "${CMAKE_SOURCE_DIR}/src/display/platform_activate_mac.mm")
else()
  # Other platforms: stubs already collected by GLOB_RECURSE
  set(PLATFORM_SOURCES "")
endif()

# Generate version.h
configure_file(
  ${CMAKE_SOURCE_DIR}/src/version.h.in
  ${CMAKE_BINARY_DIR}/generated/version.h)

# Create executable
add_executable(${PROJECT_NAME} ${C_SOURCES} ${CXX_SOURCES} ${PLATFORM_SOURCES})

# Include directories (generated/ takes priority, overrides old version.h in source)
target_include_directories(
  ${PROJECT_NAME}
  PRIVATE ${CMAKE_BINARY_DIR}/generated
          ${CMAKE_SOURCE_DIR}/src)

# Link libraries
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE spdlog::spdlog
          SDL3::SDL3
          SDL3_ttf::SDL3_ttf
          Libplacebo::Libplacebo
          FFmpeg::avcodec
          FFmpeg::avfilter
          FFmpeg::avformat
          FFmpeg::avutil
          FFmpeg::swscale)

# macOS: link AppKit framework (required for ICC profile auto-detection)
if(APPLE)
  target_link_libraries(${PROJECT_NAME} PRIVATE "-framework AppKit")
endif()

# Configure install rules
include(GNUInstallDirs)
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# DeckLink CPU/fake contract tests. No SDK, no GPU, no device.
if(EYEQ_BUILD_DECKLINK_TESTS)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tests/decklink/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_DECKLINK_TESTS=ON but tests/decklink/CMakeLists.txt is missing")
  endif()
  enable_testing()
  add_subdirectory(tests/decklink)

  # A-owned backend tests additionally need the SDK build.
  if(EYEQ_ENABLE_DECKLINK)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tests/decklink/backend/CMakeLists.txt")
      message(FATAL_ERROR "EYEQ_ENABLE_DECKLINK and EYEQ_BUILD_DECKLINK_TESTS are ON but tests/decklink/backend/CMakeLists.txt is missing")
    endif()
    add_subdirectory(tests/decklink/backend)
  endif()
endif()

# GPU packing tests plus their CPU oracle. They need libplacebo and a real Vulkan
# device at runtime, but never the DeckLink SDK, so they can build and run with the
# SDK disabled and DECKLINK_SDK_ROOT unset. A missing/unsuitable GPU is reported as
# a test failure, not silently skipped.
if(EYEQ_BUILD_DECKLINK_GPU_TESTS)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tests/decklink/packer/CMakeLists.txt")
    message(FATAL_ERROR "EYEQ_BUILD_DECKLINK_GPU_TESTS=ON but tests/decklink/packer/CMakeLists.txt is missing")
  endif()
  enable_testing()
  add_subdirectory(tests/decklink/packer)
endif()
