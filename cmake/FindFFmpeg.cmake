# FindFFmpeg.cmake
#
# Find the FFmpeg libraries
#
# This module defines the following variables: FFMPEG_FOUND - True if FFmpeg was
# found FFMPEG_INCLUDE_DIRS - FFmpeg include directories FFMPEG_LIBRARIES -
# FFmpeg libraries
#
# Also defines import targets: FFmpeg::avcodec FFmpeg::avdevice FFmpeg::avfilter
# FFmpeg::avformat FFmpeg::avutil FFmpeg::postproc FFmpeg::swresample
# FFmpeg::swscale

# Check if FFmpeg has already been found
if(FFMPEG_INCLUDE_DIRS AND FFMPEG_LIBRARIES)
  set(FFMPEG_FOUND TRUE)
  return()
endif()

# Define FFmpeg components to find
set(FFMPEG_COMPONENTS
    avcodec
    avdevice
    avfilter
    avformat
    avutil
    swscale)

# Use pkg-config on Unix systems
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  foreach(component ${FFMPEG_COMPONENTS})
    pkg_check_modules(PC_FFMPEG_${component} IMPORTED_TARGET lib${component})
  endforeach()
endif()

# Find headers and libraries
foreach(component ${FFMPEG_COMPONENTS})
  # Find header files
  find_path(
    FFMPEG_${component}_INCLUDE_DIR
    NAMES lib${component}/version.h
    HINTS ENV CPATH  # pkg-config ignores paths in CPATH
    PATHS ${PC_FFMPEG_${component}_INCLUDE_DIRS} /usr/include /usr/local/include
          /opt/local/include /opt/homebrew/include /sw/include
    PATH_SUFFIXES ffmpeg)

  # Find library files
  find_library(
    FFMPEG_${component}_LIBRARY
    NAMES ${component} lib${component}
    PATHS ${PC_FFMPEG_${component}_LIBRARY_DIRS} /usr/lib /usr/local/lib
          /opt/local/lib /opt/homebrew/lib /sw/lib)

  # Add found headers and libraries to the lists
  if(FFMPEG_${component}_INCLUDE_DIR AND FFMPEG_${component}_LIBRARY)
    list(APPEND FFMPEG_INCLUDE_DIRS ${FFMPEG_${component}_INCLUDE_DIR})
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${component}_LIBRARY})

    # Create imported target
    if(NOT TARGET FFmpeg::${component})
      add_library(FFmpeg::${component} UNKNOWN IMPORTED)
      set_target_properties(
        FFmpeg::${component}
        PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                   "${FFMPEG_${component}_INCLUDE_DIR}"
                   IMPORTED_LOCATION "${FFMPEG_${component}_LIBRARY}")
      if(PC_FFMPEG_${component}_FOUND)
        set_property(
          TARGET FFmpeg::${component}
          PROPERTY INTERFACE_COMPILE_OPTIONS
                   "${PC_FFMPEG_${component}_CFLAGS_OTHER}")

        set_property(
          TARGET FFmpeg::${component}
          APPEND
          PROPERTY INTERFACE_LINK_LIBRARIES
                   "${PC_FFMPEG_${component}_LINK_LIBRARIES}")
      endif()
    endif()
  endif()
endforeach()

# Remove duplicate include directories
if(FFMPEG_INCLUDE_DIRS)
  list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
endif()

# Set _FOUND variable for each found component
foreach(component ${FFMPEG_COMPONENTS})
  if(FFMPEG_${component}_INCLUDE_DIR AND FFMPEG_${component}_LIBRARY)
    set(FFmpeg_${component}_FOUND TRUE)
  else()
    set(FFmpeg_${component}_FOUND FALSE)
  endif()
endforeach()

# Handle FFMPEG_FOUND variable
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  FFmpeg
  REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS
  HANDLE_COMPONENTS)

mark_as_advanced(FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES)

foreach(component ${FFMPEG_COMPONENTS})
  mark_as_advanced(FFMPEG_${component}_INCLUDE_DIR FFMPEG_${component}_LIBRARY)
endforeach()

# Create FFmpeg::Headers interface target
if(NOT TARGET FFmpeg::Headers AND FFMPEG_INCLUDE_DIRS)
  add_library(FFmpeg::Headers INTERFACE IMPORTED)
  set_target_properties(FFmpeg::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                   "${FFMPEG_INCLUDE_DIRS}")
endif()
