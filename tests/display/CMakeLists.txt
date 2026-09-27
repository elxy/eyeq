# Offscreen display/rendering tests. They run the real RenderContext and
# FrameRenderer against a headless Vulkan device and never touch the DeckLink
# SDK or a window.
#
# The player is an executable, not a library, so the display rendering sources
# are compiled directly into the test. platform_icc_stub.cpp is used so no
# system display query is required; ICC isolation is exercised with an explicit
# profile file. Added when EYEQ_BUILD_DISPLAY_GPU_TESTS is ON.

set(_display_test_sources
  ${CMAKE_SOURCE_DIR}/src/display/render_context.cpp
  ${CMAKE_SOURCE_DIR}/src/display/frame_renderer.cpp
  ${CMAKE_SOURCE_DIR}/src/display/fill_render.cpp
  ${CMAKE_SOURCE_DIR}/src/display/slide_render.cpp
  ${CMAKE_SOURCE_DIR}/src/display/grid_render.cpp
  ${CMAKE_SOURCE_DIR}/src/display/video_frame.cpp
  ${CMAKE_SOURCE_DIR}/src/display/osd.cpp
  ${CMAKE_SOURCE_DIR}/src/display/render_util.cpp
  ${CMAKE_SOURCE_DIR}/src/display/mapping_target.cpp
  ${CMAKE_SOURCE_DIR}/src/display/output_encode.cpp
  ${CMAKE_SOURCE_DIR}/src/display/hdrvivid_curve.cpp
  ${CMAKE_SOURCE_DIR}/src/display/hdrvivid_meta.cpp
  ${CMAKE_SOURCE_DIR}/src/display/hdrvivid_shader.cpp
  ${CMAKE_SOURCE_DIR}/src/display/vulkan_exts.cpp
  ${CMAKE_SOURCE_DIR}/src/display/platform_icc_stub.cpp
  ${CMAKE_SOURCE_DIR}/src/log.cpp
  ${CMAKE_SOURCE_DIR}/src/libav_impl.c)

add_executable(display_renderer_gpu_test
  display_renderer_gpu_test.cpp
  ${_display_test_sources})
target_include_directories(display_renderer_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(display_renderer_gpu_test PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
add_test(NAME display_renderer_gpu COMMAND display_renderer_gpu_test)

set_tests_properties(display_renderer_gpu PROPERTIES TIMEOUT 600)

# Present-target contract tests. The desktop presenter never hands the renderer a
# plain offscreen texture: the wrapped swapchain image is not sampleable, and
# libplacebo only *logs* a validation failure without failing the dispatch, so a
# pass that samples the target corrupts the frame silently (the pink-screen OSD
# bug). These tests drive every present path against a swapchain-like target and
# assert both that no libplacebo error is logged and that the result matches an
# equivalent sampleable target. Headless Vulkan device; no window, no SDK.
add_executable(present_target_gpu_test
  present_target_gpu_test.cpp
  ${_display_test_sources})
target_include_directories(present_target_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(present_target_gpu_test PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
add_test(NAME present_target_gpu COMMAND present_target_gpu_test)
set_tests_properties(present_target_gpu PROPERTIES TIMEOUT 600)

# Display-linear contract and wire-encode tests. CPU oracles first, then the real
# OutputEncoder on a headless Vulkan device. Only the context, the mapping target
# and the encoder are compiled in: no window, no OSD, no DeckLink SDK.
set(_linear_test_sources
  ${CMAKE_SOURCE_DIR}/src/display/render_context.cpp
  ${CMAKE_SOURCE_DIR}/src/display/mapping_target.cpp
  ${CMAKE_SOURCE_DIR}/src/display/output_encode.cpp
  ${CMAKE_SOURCE_DIR}/src/display/vulkan_exts.cpp
  ${CMAKE_SOURCE_DIR}/src/display/platform_icc_stub.cpp
  ${CMAKE_SOURCE_DIR}/src/log.cpp
  ${CMAKE_SOURCE_DIR}/src/libav_impl.c)
add_executable(display_linear_gpu_test
  display_linear_gpu_test.cpp
  ${_linear_test_sources})
target_include_directories(display_linear_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(display_linear_gpu_test PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
add_test(NAME display_linear_gpu COMMAND display_linear_gpu_test)

set_tests_properties(display_linear_gpu PROPERTIES TIMEOUT 600)

# Coordination-layer tests: the DisplayCoordinator owns the single FrameRenderer,
# the fixed mapping target, the canvas and the ICC/precision/NaN-Inf policies.
# Headless Vulkan device; no window, no SDK.
add_executable(display_coordinator_gpu_test
  display_coordinator_gpu_test.cpp
  ${CMAKE_SOURCE_DIR}/src/display/display_coordinator.cpp
  ${CMAKE_SOURCE_DIR}/src/display/canvas_geometry.cpp
  ${_display_test_sources})
target_include_directories(display_coordinator_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(display_coordinator_gpu_test PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
add_test(NAME display_coordinator_gpu COMMAND display_coordinator_gpu_test)
set_tests_properties(display_coordinator_gpu PROPERTIES TIMEOUT 600)

# The canvas/preview coordinate transform is pure CPU: it must pass without a GPU,
# a window or libplacebo.
add_executable(canvas_geometry_test
  canvas_geometry_test.cpp
  ${CMAKE_SOURCE_DIR}/src/display/canvas_geometry.cpp)
target_include_directories(canvas_geometry_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME canvas_geometry COMMAND canvas_geometry_test)
set_tests_properties(canvas_geometry PROPERTIES TIMEOUT 60)

# HDR Vivid / Dolby Vision CPU building blocks: the CUVA curve solver, the
# metadata parser and the PQ transfer. Pure CPU, no GPU, no window, no SDK.
add_executable(hdrvivid_cpu_test
  hdrvivid_cpu_test.cpp
  ${CMAKE_SOURCE_DIR}/src/display/hdrvivid_curve.cpp
  ${CMAKE_SOURCE_DIR}/src/display/hdrvivid_meta.cpp)
target_include_directories(hdrvivid_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hdrvivid_cpu_test PRIVATE
  spdlog::spdlog
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
add_test(NAME hdrvivid_cpu COMMAND hdrvivid_cpu_test)
set_tests_properties(hdrvivid_cpu PROPERTIES TIMEOUT 120)

# HDR Vivid / Dolby Vision integration on the real comparison renderer: the single
# FrameRenderer maps both dynamic-HDR formats into the same common display-linear
# target. Headless Vulkan device; no window, no SDK.
add_executable(hdrvivid_dv_gpu_test
  hdrvivid_dv_gpu_test.cpp
  ${_display_test_sources})
target_include_directories(hdrvivid_dv_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hdrvivid_dv_gpu_test PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
add_test(NAME hdrvivid_dv_gpu COMMAND hdrvivid_dv_gpu_test)
set_tests_properties(hdrvivid_dv_gpu PROPERTIES TIMEOUT 600)

# Real-material probe (built, not registered as a test): documents what a real
# file actually carries and what the common mapping produces. Run manually.
add_executable(hdrvivid_real_probe
  hdrvivid_real_probe.cpp
  ${_display_test_sources})
target_include_directories(hdrvivid_real_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hdrvivid_real_probe PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil)
