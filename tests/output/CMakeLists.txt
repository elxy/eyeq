# Monitor composition-layer tests: the real MonitorOutput drives the real
# RenderContext, FrameRenderer and FramePacker, with a scripted fake FrameSink.
# No DeckLink SDK, no window and no device. Added under EYEQ_BUILD_DISPLAY_GPU_TESTS.

# Display rendering sources are compiled into each test: the player is an
# executable, not a library.
set(_monitor_display_sources
  ${CMAKE_SOURCE_DIR}/src/display/render_context.cpp
  ${CMAKE_SOURCE_DIR}/src/display/display_coordinator.cpp
  ${CMAKE_SOURCE_DIR}/src/display/canvas_geometry.cpp
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

# Queue-order invariant sentinel: the encode dispatched by Submit must be
# submitted before an immediate recomposition overwrites the borrowed common
# result texture (the premise for dropping the borrow protocol).
add_executable(queue_order_gpu_test
  queue_order_gpu_test.cpp
  ${CMAKE_SOURCE_DIR}/tests/decklink/fake_frame_sink.cpp
  ${_monitor_display_sources})
target_include_directories(queue_order_gpu_test PRIVATE ${_monitor_test_includes})
target_link_libraries(queue_order_gpu_test PRIVATE eyeq_frame_packer ${_monitor_test_libraries})
add_test(NAME queue_order_gpu COMMAND queue_order_gpu_test)

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

# MonitorController integration: the real coordinator + MonitorOutput + packer with
# the scripted fake sink, proving the borrow pairing, the shared desktop/monitor
# result, resize/OSD byte stability, resubmission and the Busy/cancel/failure/
# drain/close paths on the interface main actually uses.
add_executable(monitor_controller_gpu_test
  monitor_controller_gpu_test.cpp
  ${CMAKE_SOURCE_DIR}/tests/decklink/fake_frame_sink.cpp
  ${CMAKE_SOURCE_DIR}/src/output/monitor_controller.cpp
  ${_monitor_display_sources})
target_include_directories(monitor_controller_gpu_test PRIVATE ${_monitor_test_includes})
target_link_libraries(monitor_controller_gpu_test PRIVATE eyeq_frame_packer ${_monitor_test_libraries})
add_test(NAME monitor_controller_gpu COMMAND monitor_controller_gpu_test)

set_tests_properties(monitor_output_gpu monitor_output_fault monitor_controller_gpu queue_order_gpu PROPERTIES TIMEOUT 900)
