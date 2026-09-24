# A-owned DeckLink backend tests. These require the SDK headers but no device and
# no GPU. They are added only when EYEQ_ENABLE_DECKLINK and
# EYEQ_BUILD_DECKLINK_TESTS are both ON.
add_executable(decklink_monitor_mode_test monitor_mode_test.cpp)
target_link_libraries(decklink_monitor_mode_test PRIVATE eyeq_decklink_backend)
add_test(NAME decklink_monitor_mode COMMAND decklink_monitor_mode_test)

add_executable(decklink_catalog_test catalog_test.cpp)
target_link_libraries(decklink_catalog_test PRIVATE eyeq_decklink_backend)
add_test(NAME decklink_catalog COMMAND decklink_catalog_test)
