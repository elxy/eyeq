# GPU packing tests. They need libplacebo and a real Vulkan device at runtime, but
# never the DeckLink SDK. Added when EYEQ_BUILD_DECKLINK_GPU_TESTS is ON.
#
# The CPU-only oracle (reference + comparator known answers and mutation tests) is
# registered with the CPU tests instead, so it runs without a GPU.

# Real-GPU packing/readback and slot ownership tests.
add_executable(frame_packer_gpu_test frame_packer_gpu_test.cpp)
target_include_directories(frame_packer_gpu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frame_packer_gpu_test PRIVATE eyeq_frame_packer eyeq_output_contract)
add_test(NAME frame_packer_gpu COMMAND frame_packer_gpu_test)

# Deterministic fault injection: the real frame_packer.cpp is compiled into this
# target with the test hooks enabled instead of linking eyeq_frame_packer.
add_executable(frame_packer_fault_test
  frame_packer_fault_test.cpp
  ${CMAKE_SOURCE_DIR}/src/output/frame_packer.cpp)
target_include_directories(frame_packer_fault_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(frame_packer_fault_test PRIVATE EYEQ_FRAME_PACKER_TEST_HOOKS)
target_link_libraries(frame_packer_fault_test PRIVATE eyeq_output_contract)
add_test(NAME frame_packer_fault COMMAND frame_packer_fault_test)

# A final wall-time guard; the tests also bound their own waits.
set_tests_properties(frame_packer_gpu frame_packer_fault PROPERTIES TIMEOUT 600)
