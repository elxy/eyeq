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
option(EYEQ_ENABLE_DECKLINK "Enable the DeckLink SDK output backend (not implemented in phase 0.5)" OFF)
option(EYEQ_BUILD_DECKLINK_TESTS "Build the DeckLink CPU/fake contract tests (no SDK, no GPU)" OFF)
option(EYEQ_BUILD_DECKLINK_GPU_TESTS "Build the DeckLink GPU packing tests (not implemented in phase 0.5)" OFF)
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

# Phase 0.5 guard: do not pretend an unavailable backend is built.
if(EYEQ_ENABLE_DECKLINK)
  message(FATAL_ERROR
    "EYEQ_ENABLE_DECKLINK=ON is not available in phase 0.5: the SDK output backend is not implemented yet. "
    "Only the CPU contract library and CPU/fake tests exist. Leave EYEQ_ENABLE_DECKLINK=OFF.")
endif()
if(EYEQ_BUILD_DECKLINK_GPU_TESTS)
  message(FATAL_ERROR
    "EYEQ_BUILD_DECKLINK_GPU_TESTS=ON is not available in phase 0.5: the GPU packer is declaration-only. "
    "Leave EYEQ_BUILD_DECKLINK_GPU_TESTS=OFF.")
endif()

# CPU-only output contract library: public headers plus pure helpers. It has no SDK
# dependency and does not use the GPU; libplacebo is linked PUBLIC only because
# frame_packer.hpp declares pl_tex/pl_gpu parameters.
add_library(eyeq_output_contract STATIC
  ${CMAKE_SOURCE_DIR}/src/output/pixel_format.cpp
  ${CMAKE_SOURCE_DIR}/src/output/output_profile.cpp)
target_include_directories(eyeq_output_contract PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(eyeq_output_contract PUBLIC Libplacebo::Libplacebo)

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
endif()
