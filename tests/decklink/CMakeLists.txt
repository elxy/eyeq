# DeckLink CPU/fake contract tests. No SDK, no GPU, no device.
add_executable(decklink_contract_test
  contract_test.cpp
  fake_frame_sink.cpp
  include_pixel_format.cpp
  include_output_profile.cpp
  include_frame_sink.cpp
  include_frame_packer.cpp)
target_include_directories(decklink_contract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(decklink_contract_test PRIVATE eyeq_output_contract)

add_test(NAME decklink_contract COMMAND decklink_contract_test)

# Strict CLI selector parsing. No SDK, no GPU, no device.
add_executable(decklink_device_selector_test device_selector_test.cpp)
target_include_directories(decklink_device_selector_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/decklink-catalog)
target_link_libraries(decklink_device_selector_test PRIVATE eyeq_output_contract)
add_test(NAME decklink_device_selector COMMAND decklink_device_selector_test)

# Strict monitor CLI parsing/validation (device selector, integer mode, connection,
# pixel format, color target, native mode match, coherent output request). CPU only.
add_executable(decklink_monitor_cli_test monitor_cli_test.cpp)
target_include_directories(decklink_monitor_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(decklink_monitor_cli_test PRIVATE eyeq_output_contract)
add_test(NAME decklink_monitor_cli COMMAND decklink_monitor_cli_test)

# Deterministic CPU test-signal pattern generator. No SDK, no GPU, no device.
add_executable(decklink_test_signal_pattern_test
  test_signal_pattern_test.cpp
  ${CMAKE_SOURCE_DIR}/tools/decklink-test-signal/test_pattern.cpp)
target_include_directories(decklink_test_signal_pattern_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/decklink-test-signal)
target_link_libraries(decklink_test_signal_pattern_test PRIVATE eyeq_output_contract)
add_test(NAME decklink_test_signal_pattern COMMAND decklink_test_signal_pattern_test)

# CPU-only known-answer tests for the independent r210/v210 packing reference and the
# byte-level comparator. No GPU, no SDK, no packer link.
add_executable(decklink_frame_packer_oracle_test
  packer/frame_packer_oracle_test.cpp)
target_include_directories(decklink_frame_packer_oracle_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/packer)
target_link_libraries(decklink_frame_packer_oracle_test PRIVATE eyeq_output_contract)
add_test(NAME decklink_frame_packer_oracle COMMAND decklink_frame_packer_oracle_test)
