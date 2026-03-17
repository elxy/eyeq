# FindLibplacebo.cmake
#
# Find the libplacebo library
#
# This module defines the following variables: LIBPLACEBO_FOUND - True if
# libplacebo was found LIBPLACEBO_INCLUDE_DIRS - libplacebo include directories
# LIBPLACEBO_LIBRARIES - libplacebo libraries LIBPLACEBO_VERSION - libplacebo
# version string
#
# Also defines import target: Libplacebo::libplacebo

# Check if libplacebo has already been found
if(LIBPLACEBO_INCLUDE_DIRS AND LIBPLACEBO_LIBRARIES)
  set(LIBPLACEBO_FOUND TRUE)
  return()
endif()

# Prefer pkg-config on Unix systems
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBPLACEBO QUIET libplacebo)
endif()

# Find header files
find_path(
  LIBPLACEBO_INCLUDE_DIR
  NAMES libplacebo/common.h
  HINTS ENV CPATH  # pkg-config ignores paths in CPATH
  PATHS ${PC_LIBPLACEBO_INCLUDE_DIRS} /usr/include /usr/local/include
        /opt/local/include /opt/homebrew/include /sw/include)

# Find library files
find_library(
  LIBPLACEBO_LIBRARY
  NAMES placebo libplacebo
  PATHS ${PC_LIBPLACEBO_LIBRARY_DIRS} /usr/lib /usr/local/lib /opt/local/lib
        /opt/homebrew/lib /sw/lib)

# Get version information
if(PC_LIBPLACEBO_VERSION)
  set(LIBPLACEBO_VERSION ${PC_LIBPLACEBO_VERSION})
else()
  if(EXISTS "${LIBPLACEBO_INCLUDE_DIR}/libplacebo/version.h")
    file(STRINGS "${LIBPLACEBO_INCLUDE_DIR}/libplacebo/version.h" version_line
         REGEX "^#define PL_API_VERSION[ \t]+[0-9]+")
    if(version_line)
      string(REGEX REPLACE "^#define PL_API_VERSION[ \t]+([0-9]+)" "\\1"
                           PL_API_VERSION "${version_line}")
      # Calculate version number (e.g., API version 123 becomes 1.23)
      math(EXPR PL_VERSION_MAJOR "${PL_API_VERSION} / 100")
      math(EXPR PL_VERSION_MINOR "${PL_API_VERSION} % 100")
      set(LIBPLACEBO_VERSION "${PL_VERSION_MAJOR}.${PL_VERSION_MINOR}")
    endif()
  endif()
endif()

# Set include directories and libraries
set(LIBPLACEBO_INCLUDE_DIRS ${LIBPLACEBO_INCLUDE_DIR})
set(LIBPLACEBO_LIBRARIES ${LIBPLACEBO_LIBRARY})

# Handle find results
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Libplacebo
  REQUIRED_VARS LIBPLACEBO_LIBRARY LIBPLACEBO_INCLUDE_DIR
  VERSION_VAR LIBPLACEBO_VERSION)

mark_as_advanced(LIBPLACEBO_INCLUDE_DIR LIBPLACEBO_LIBRARY)

# Create imported target
if(LIBPLACEBO_FOUND AND NOT TARGET Libplacebo::Libplacebo)
  add_library(Libplacebo::Libplacebo UNKNOWN IMPORTED)
  set_target_properties(
    Libplacebo::Libplacebo
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${LIBPLACEBO_INCLUDE_DIRS}"
               IMPORTED_LOCATION "${LIBPLACEBO_LIBRARY}")

  # Add compile options found by pkg-config
  if(PC_LIBPLACEBO_FOUND)
    set_property(
      TARGET Libplacebo::Libplacebo PROPERTY INTERFACE_COMPILE_OPTIONS
                                             "${PC_LIBPLACEBO_CFLAGS_OTHER}")

    # If libplacebo has dependencies, get and add them from pkg-config
    if(PC_LIBPLACEBO_LIBRARIES)
      set_property(
        TARGET Libplacebo::Libplacebo
        APPEND
        PROPERTY INTERFACE_LINK_LIBRARIES "${PC_LIBPLACEBO_LINK_LIBRARIES}")
    endif()
  endif()
endif()

# Create Libplacebo::Headers interface target
if(NOT TARGET Libplacebo::Headers AND LIBPLACEBO_INCLUDE_DIRS)
  add_library(Libplacebo::Headers INTERFACE IMPORTED)
  set_target_properties(
    Libplacebo::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${LIBPLACEBO_INCLUDE_DIRS}")
endif()
