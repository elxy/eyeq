# Library targets for src/output/. The top-level CMakeLists.txt excludes src/output/
# from the player's source glob, because this directory is shared by the player, the
# tools/ executables and the tests; the player is an executable, so the shared code
# has to live in libraries. Defined here, next to the sources they build.

# CPU-only output contract library: public headers plus pure helpers. It has no SDK
# dependency and does not use the GPU; libplacebo is linked PUBLIC only because
# frame_packer.hpp declares pl_tex/pl_gpu parameters. It also carries the pure
# monitor mode helpers and the strict monitor CLI parsing so the player and the
# tools share exactly one implementation (no duplicated selector parser).
add_library(eyeq_output_contract STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/pixel_format.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/output_profile.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/monitor_cli.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/decklink/monitor_mode.cpp)
target_include_directories(eyeq_output_contract PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(eyeq_output_contract PUBLIC Libplacebo::Libplacebo)

# GPU frame packer: nonlinear full-range float RGB -> r210/v210 Video-range bytes,
# with a fixed staging ring and asynchronous pl_tex_download + pl_buf_poll readback.
# It only needs libplacebo and a caller-provided pl_gpu; the player is not wired to
# it yet and the DeckLink SDK is not a dependency. EXCLUDE_FROM_ALL keeps the default
# player build unaffected; the GPU tests pull it in when they are enabled.
add_library(eyeq_frame_packer STATIC EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_packer.cpp)
target_include_directories(eyeq_frame_packer PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(eyeq_frame_packer PUBLIC eyeq_output_contract Libplacebo::Libplacebo)

# Still-only monitor composition layer: FrameRenderer -> FramePacker ->
# FrameSink::DisplayStill. EXCLUDE_FROM_ALL keeps it out of the default player
# build. It intentionally does NOT link eyeq_frame_packer so a test can substitute
# the fault-injection build of frame_packer.cpp; the final linkable must provide the
# FramePacker symbols.
add_library(eyeq_monitor_output STATIC EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_SOURCE_DIR}/monitor_output.cpp)
target_include_directories(eyeq_monitor_output PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(eyeq_monitor_output PUBLIC
  eyeq_output_contract
  Libplacebo::Libplacebo
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avutil
  spdlog::spdlog)
