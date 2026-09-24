# A-owned DeckLink backend tests. These require the SDK headers but no device and
# no GPU. They are added only when EYEQ_ENABLE_DECKLINK and
# EYEQ_BUILD_DECKLINK_TESTS are both ON.
set(_decklink_include "${DECKLINK_SDK_ROOT}/Mac/include")

add_executable(decklink_monitor_mode_test monitor_mode_test.cpp)
target_link_libraries(decklink_monitor_mode_test PRIVATE eyeq_decklink_backend)
add_test(NAME decklink_monitor_mode COMMAND decklink_monitor_mode_test)

add_executable(decklink_catalog_test catalog_test.cpp)
target_link_libraries(decklink_catalog_test PRIVATE eyeq_decklink_backend)
add_test(NAME decklink_catalog COMMAND decklink_catalog_test)

# Deterministic fault-injection tests. The fake dispatch in fake_decklink.cpp replaces
# the SDK dispatch, so these do not link DeckLinkAPIDispatch.cpp, load a driver or touch
# a device. They cover the catalog error paths that the environment smoke test cannot.
add_executable(decklink_catalog_fault_test
  catalog_fault_test.cpp
  fake_decklink.cpp
  ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_catalog.cpp
  ${CMAKE_SOURCE_DIR}/src/output/decklink/monitor_mode.cpp)
target_include_directories(decklink_catalog_fault_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  "${_decklink_include}")
target_link_libraries(decklink_catalog_fault_test PRIVATE
  eyeq_output_contract
  "-framework CoreFoundation")
add_test(NAME decklink_catalog_fault COMMAND decklink_catalog_fault_test)

# End-to-end CLI regression with the same fake dispatch: selector wiring and exit codes.
add_executable(decklink_catalog_cli_test
  ${CMAKE_SOURCE_DIR}/tools/decklink-catalog/main.cpp
  fake_decklink.cpp
  ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_catalog.cpp
  ${CMAKE_SOURCE_DIR}/src/output/decklink/monitor_mode.cpp)
target_include_directories(decklink_catalog_cli_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tools/decklink-catalog
  "${_decklink_include}")
target_link_libraries(decklink_catalog_cli_test PRIVATE
  eyeq_output_contract
  "-framework CoreFoundation")

add_test(NAME decklink_catalog_cli_list COMMAND decklink_catalog_cli_test)
add_test(NAME decklink_catalog_cli_help COMMAND decklink_catalog_cli_test --help)
add_test(NAME decklink_catalog_cli_mode_by_id
  COMMAND decklink_catalog_cli_test --device id:2697367084 --connection sdi --pixel-format v210)
add_test(NAME decklink_catalog_cli_mode_by_index COMMAND decklink_catalog_cli_test --device index:1)
add_test(NAME decklink_catalog_cli_rejects_bare_number COMMAND decklink_catalog_cli_test --device 0)
set_tests_properties(decklink_catalog_cli_rejects_bare_number PROPERTIES WILL_FAIL TRUE)
add_test(NAME decklink_catalog_cli_rejects_overlarge_index
  COMMAND decklink_catalog_cli_test --device index:4294967296)
set_tests_properties(decklink_catalog_cli_rejects_overlarge_index PROPERTIES WILL_FAIL TRUE)

# Output backend tests: they compile the real decklink_output.cpp state machine and
# the read-only catalog helpers, and replace only the two SDK dispatch factories with
# the fake. DeckLinkAPIDispatch.cpp is deliberately not linked.
find_package(Threads REQUIRED)
set(_decklink_output_sources
  ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_output.cpp
  ${CMAKE_SOURCE_DIR}/src/output/decklink/decklink_catalog.cpp
  ${CMAKE_SOURCE_DIR}/src/output/decklink/monitor_mode.cpp
  fake_decklink_output.cpp)

add_executable(decklink_output_test decklink_output_test.cpp ${_decklink_output_sources})
target_include_directories(decklink_output_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  "${_decklink_include}")
target_link_libraries(decklink_output_test PRIVATE
  eyeq_output_contract
  Threads::Threads
  "-framework CoreFoundation")
add_test(NAME decklink_output COMMAND decklink_output_test)

add_executable(decklink_output_concurrency_test
  decklink_output_concurrency_test.cpp ${_decklink_output_sources})
target_include_directories(decklink_output_concurrency_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  "${_decklink_include}")
target_link_libraries(decklink_output_concurrency_test PRIVATE
  eyeq_output_contract
  Threads::Threads
  "-framework CoreFoundation")
add_test(NAME decklink_output_concurrency COMMAND decklink_output_concurrency_test)

add_executable(decklink_output_death_test decklink_output_death_test.cpp ${_decklink_output_sources})
target_include_directories(decklink_output_death_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  "${_decklink_include}")
target_link_libraries(decklink_output_death_test PRIVATE
  eyeq_output_contract
  Threads::Threads
  "-framework CoreFoundation")
add_test(NAME decklink_output_death COMMAND decklink_output_death_test)

# Test-signal session test: the real backend state machine plus the real session and
# pattern sources, driven against the fake SDK.
set(_decklink_test_signal_sources
  ${CMAKE_SOURCE_DIR}/tools/decklink-test-signal/test_pattern.cpp
  ${CMAKE_SOURCE_DIR}/tools/decklink-test-signal/test_signal_session.cpp)

add_executable(decklink_test_signal_test
  decklink_test_signal_test.cpp
  ${_decklink_test_signal_sources}
  ${_decklink_output_sources})
target_include_directories(decklink_test_signal_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tools/decklink-test-signal
  "${_decklink_include}")
target_link_libraries(decklink_test_signal_test PRIVATE
  eyeq_output_contract
  Threads::Threads
  "-framework CoreFoundation")
add_test(NAME decklink_test_signal COMMAND decklink_test_signal_test)

# CLI regression: main.cpp is compiled into the test (renamed) and the fake SDK
# provides the device/mode list. No output dispatch is linked.
add_executable(decklink_test_signal_cli_test
  decklink_test_signal_cli_test.cpp
  ${_decklink_test_signal_sources}
  ${_decklink_output_sources})
target_include_directories(decklink_test_signal_cli_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tools/decklink-catalog
  ${CMAKE_SOURCE_DIR}/tools/decklink-test-signal
  "${_decklink_include}")
target_link_libraries(decklink_test_signal_cli_test PRIVATE
  eyeq_output_contract
  Threads::Threads
  "-framework CoreFoundation")
add_test(NAME decklink_test_signal_cli COMMAND decklink_test_signal_cli_test)
