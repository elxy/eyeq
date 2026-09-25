# CPU tests for the 2C-1 frame publication/control model. They drive the real
# SyncPlayer and VideoSource over locally synthesized YUV4MPEG2 files. No GPU,
# DeckLink SDK or window is involved.
#
# libplacebo is linked only because log.cpp and the shared headers reference it;
# the player sources under test do not use the GPU.

add_executable(player_sync_test
  player_sync_test.cpp
  ${CMAKE_SOURCE_DIR}/src/sync_player.cpp
  ${CMAKE_SOURCE_DIR}/src/video_source.cpp
  ${CMAKE_SOURCE_DIR}/src/save_frame.cpp
  ${CMAKE_SOURCE_DIR}/src/log.cpp)

target_include_directories(player_sync_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(player_sync_test PRIVATE
  spdlog::spdlog
  Libplacebo::Libplacebo
  FFmpeg::avcodec
  FFmpeg::avfilter
  FFmpeg::avformat
  FFmpeg::avutil
  FFmpeg::swscale)

add_test(NAME player_sync COMMAND player_sync_test)
set_tests_properties(player_sync PROPERTIES TIMEOUT 300)
