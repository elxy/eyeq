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

# Collect source files
file(GLOB_RECURSE C_SOURCES "${CMAKE_SOURCE_DIR}/src/*.c")
file(GLOB_RECURSE CXX_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")

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
