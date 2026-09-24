# Monitor composition-layer tests: the real MonitorOutput drives the real
# RenderContext, FrameRenderer and FramePacker, with a scripted fake FrameSink.
# No DeckLink SDK, no window and no device. Added under EYEQ_BUILD_DISPLAY_GPU_TESTS.

# Display rendering sources are compiled into each test: the player is an
# executable, not a library.
set(_monitor_display_sources
  ${CMAKE_SOURCE_DIR}/src/display/render_context.cpp
  ${CMAKE_SOURCE_DIR}/src/display/frame_renderer.cpp
  ${CMAKE_SOURCE_DIR}/src/display/fill_render.cpp
  ${CMAKE_SOURCE_DIR}/src/display/slide_render.cpp
  ${CMAKE_SOURCE_DIR}/src/display/grid_render.cpp
  ${CMAKE_SOURCE_DIR}/src/display/video_frame.cpp
  ${CMAKE_SOURCE_DIR}/src/display/osd.cpp
  ${CMAKE_SOURCE_DIR}/src/display/render_util.cpp
  ${CMAKE_SOURCE_DIR}/src/display/vulkan_exts.cpp
  ${CMAKE_SOURCE_DIR}/src/display/platform_icc_stub.cpp
  ${CMAKE_SOURCE_DIR}/src/log.cpp
  ${CMAKE_SOURCE_DIR}/src/libav_impl.c)

set(_monitor_test_includes
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests/decklink
  ${CMAKE_SOURCE_DIR}/tests/decklink/packer
  ${CMAKE_CURRENT_SOURCE_DIR})

set(_monitor_test_libraries
  eyeq_monitor_output
  spdlog::spdlog
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil
  Libplacebo::Libplacebo)

# Main composition test links the normal packer.
add_executable(monitor_output_gpu_test
  monitor_output_gpu_test.cpp
  ${CMAKE_SOURCE_DIR}/tests/decklink/fake_frame_sink.cpp
  ${_monitor_display_sources})
target_include_directories(monitor_output_gpu_test PRIVATE ${_monitor_test_includes})
target_link_libraries(monitor_output_gpu_test PRIVATE eyeq_frame_packer ${_monitor_test_libraries})
add_test(NAME monitor_output_gpu COMMAND monitor_output_gpu_test)

# Fault-injection build of the real frame_packer.cpp with the test hooks. It must
# NOT link eyeq_frame_packer, otherwise the injection would silently miss.
add_executable(monitor_output_fault_test
  monitor_output_fault_test.cpp
  ${CMAKE_SOURCE_DIR}/tests/decklink/fake_frame_sink.cpp
  ${CMAKE_SOURCE_DIR}/src/output/frame_packer.cpp
  ${_monitor_display_sources})
target_include_directories(monitor_output_fault_test PRIVATE ${_monitor_test_includes})
target_compile_definitions(monitor_output_fault_test PRIVATE EYEQ_FRAME_PACKER_TEST_HOOKS)
target_link_libraries(monitor_output_fault_test PRIVATE ${_monitor_test_libraries})
add_test(NAME monitor_output_fault COMMAND monitor_output_fault_test)

set_tests_properties(monitor_output_gpu monitor_output_fault PROPERTIES TIMEOUT 900)
