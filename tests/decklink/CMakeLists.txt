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
