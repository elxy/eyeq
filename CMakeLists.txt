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

# src/output/ is built by its own library targets, defined next to the sources in
# src/output/CMakeLists.txt. They are production code shared by the player, the
# tools/ executables and the tests, so they must stay available with the test
# switches off. This must run before the DeckLink backend block below, which links
# eyeq_output_contract.
add_subdirectory(src/output)

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
    ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_output.cpp)
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
# src/output/ is built by its own library targets (src/output/CMakeLists.txt); never
# compile it into the player.
list(FILTER CXX_SOURCES EXCLUDE REGEX "/src/output/")

# Platform-specific source files
if(APPLE)
  # macOS: use .mm implementations, exclude stubs
  list(REMOVE_ITEM CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/display/platform_icc_stub.cpp")
  list(REMOVE_ITEM CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/display/platform_activate_stub.cpp")
  list(REMOVE_ITEM CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/display/platform_display_stub.cpp")
  set(PLATFORM_SOURCES
    "${CMAKE_SOURCE_DIR}/src/display/platform_icc_mac.mm"
    "${CMAKE_SOURCE_DIR}/src/display/platform_activate_mac.mm"
    "${CMAKE_SOURCE_DIR}/src/display/platform_display_mac.mm")
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

# SDK-enabled player wiring. The still-only MonitorOutput consumes the common result
# produced by the DisplayCoordinator; the DeckLink backend is opened only when the
# user passes --monitor-device. With EYEQ_ENABLE_DECKLINK=OFF none of these sources
# or symbols are compiled or linked into the default player.
if(EYEQ_ENABLE_DECKLINK)
  target_compile_definitions(${PROJECT_NAME} PRIVATE EYEQ_ENABLE_DECKLINK=1)
  target_link_libraries(${PROJECT_NAME} PRIVATE
    eyeq_monitor_output
    eyeq_frame_packer
    eyeq_decklink_backend)
endif()

# Configure install rules
include(GNUInstallDirs)
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Optional test targets. The switches, the target wiring and the documentation all
# live in tests/ (see tests/README.md); every test target defaults to OFF, so the
# normal player build is unaffected. enable_testing() is called here because ctest
# expects a test file in the build root, and it generates the top-level test file
# that delegates to the subdirectories.
enable_testing()
add_subdirectory(tests)
