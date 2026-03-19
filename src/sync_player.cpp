#include "sync_player.hpp"

#include "utils.hpp"

using namespace EYEQ;

const SyncPlayer::VideoOffset SyncPlayer::kDefaultVideoOffset{};

SyncPlayer::SyncPlayer() {
  main_id_ = -1;
  state_.store(PlayState::kStopped);
}

SyncPlayer::~SyncPlayer() { Stop(); }

int SyncPlayer::AddVideoSource(std::unique_ptr<VideoSource> &&source, int id) {
  id = id < 0 ? sources_.size() : id;
  if (main_id_ < 0) {
    main_id_ = id;
  }

  Logger->info("Video #{}: {}", id, source->Filename());
  Logger->info("\t{}, {:.3f} fps, start time {:.3f}s, duration {:.3f}s", source->CodecString(), source->FrameRate(),
               source->StartTime(), source->Duration());

  sources_[id] = std::move(source);
  return id;
}

int SyncPlayer::Width(int id) { return sources_.at(ResolveId(id))->Width(); }

int SyncPlayer::Height(int id) { return sources_.at(ResolveId(id))->Height(); }

float SyncPlayer::FrameRate(int id) { return sources_.at(ResolveId(id))->FrameRate(); }

float SyncPlayer::StartTime(int id) { return sources_.at(ResolveId(id))->StartTime(); }

float SyncPlayer::EndTime(int id) { return sources_.at(ResolveId(id))->EndTime(); }

void SyncPlayer::SaveCurrentFrames(bool save_in_source, const std::string &ext) {
  std::filesystem::path path;
  int serial = 0;
  for (auto &[id, source] : sources_) {
    std::filesystem::path filename(source->Filename());
    if (save_in_source) {
      path = filename.parent_path();
    } else {
      path.clear();
    }
    serial = source->GetCurrentFrameSerial();
    path /= fmt::format("{:04d}.{}.{}.{}", serial, id, filename.filename().string(), ext);
    source->SaveCurrentFrame(path.string());
    Logger->info("{} saved", path.string());
  }
}

float SyncPlayer::CurrentTime() const { return current_position_s_.load(); }

int SyncPlayer::GetCurrentFrameSerial(int id) const { return sources_.at(ResolveId(id))->GetCurrentFrameSerial(); }

int SyncPlayer::GetTotalFrames(int id) const { return sources_.at(ResolveId(id))->GetTotalFrames(); }

std::vector<int> SyncPlayer::CheckUnusedHardwareDecoders() const {
  std::vector<int> unused_ids;
  for (auto &[id, source] : sources_) {
    if (source->IsHardwareDecoderRequested() && !source->IsHardwareDecoderUsed()) {
      unused_ids.push_back(id);
    }
  }
  return unused_ids;
}

float SyncPlayer::GetPlaybackFps() const { return playback_fps_.load(); }

void SyncPlayer::StartSources(float seek_to, size_t seek_to_frame) {
  for (auto &[id, source] : sources_) {
    if (seek_to > 0) {
      source->SeekTo(seek_to, 0);
    }
    source->Start(seek_to_frame);
  }

  if (seek_to > 0 || seek_to_frame > 0) {
    Logger->debug("Waiting for seeking");
  }

  for (auto &[id, source] : sources_) {
    source->WaitForFrameAvailable();
  }

  if (seek_to > 0 || seek_to_frame > 0) {
    Logger->debug("Seek ready");
    position_updated_ = true; // Sync playback clock to the first frame's PTS after seek
  }
}

void SyncPlayer::SetFrameUpdateCallback(std::function<void(std::map<int, std::shared_ptr<AVFrame>> &)> callback) {
  frame_callback_ = callback;
}

void SyncPlayer::SetFlickerCallback(float interval_s, std::function<void()> callback) {
  flicker_interval_s_ = interval_s;
  flicker_callback_ = callback;
}

void SyncPlayer::SetResolutionChangedCallback(std::function<void()> callback) {
  resolution_changed_callback_ = callback;
}

void SyncPlayer::Start(bool pause) {
  if (PlayState::kStopped != state_) {
    throw std::runtime_error("SyncPlayer::Start() called when not stopped");
  }

  current_position_s_.store(0);
  last_update_time_ = std::chrono::steady_clock::now();
  last_flicker_time_s_ = 0;
  fps_window_start_ = std::chrono::steady_clock::now();
  fps_window_frame_count_ = 0;
  fps_warmup_ = true;
  start_on_pause_ = pause;
  loop_thread_ = std::thread(&SyncPlayer::PlayerLoop, this);

  state_.store(PlayState::kPlaying);
}

void SyncPlayer::Stop() {
  state_.store(PlayState::kStopped);

  for (auto &[id, source] : sources_) {
    source->Stop();
  }

  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
}

void SyncPlayer::Play() {
  if (PlayState::kPaused == state_) {
    last_update_time_ = std::chrono::steady_clock::now();
    state_.store(PlayState::kPlaying);
  } else {
    Logger->error("SyncPlayer::Play() called when not playing");
  }
}

void SyncPlayer::Pause() {
  if (PlayState::kPlaying == state_) {
    state_.store(PlayState::kPaused);
  } else {
    Logger->error("SyncPlayer::Pause() called when not paused");
  }
}

void SyncPlayer::InvertPause() {
  if (PlayState::kPlaying == state_) {
    Pause();
  } else if (PlayState::kPaused == state_) {
    Play();
  } else {
    Logger->error("SyncPlayer::InvertPause() called when not playing or paused");
  }
}

void SyncPlayer::SeekFrames(int nb_frames) {
  bool forward = (nb_frames > 0);

  Logger->debug("seeking {} frames", nb_frames);
  nb_frames = std::abs(nb_frames);
  if (0 == nb_frames) {
    return;
  } else {
    // frames_outdated_ causes an extra frame seek
    nb_frames -= 1;
  }
  while (nb_frames > 0) {
    GetFrames(forward);
    nb_frames -= 1;
  }

  frames_outdated_ = true;
  position_updated_ = true;
}

void SyncPlayer::SeekTo(float time_s) {
  PlayState old_state = state_.exchange(PlayState::kPaused);

  Logger->debug("seeking to {:.3f}s", time_s);
  float start_time = StartTime();
  if (!std::isnan(start_time)) {
    if (time_s < start_time) {
      time_s = start_time;
      Logger->debug("Seeking to {:.3f}s, which is before the start time", time_s);
    }
  }
  float end_time = EndTime();
  if (!std::isnan(end_time)) {
    if (end_time < time_s) {
      time_s = end_time;
      Logger->debug("Seeking to {:.3f}s, which is after the end time", time_s);
    }
  }

  SPDLOG_LOGGER_TRACE(Logger, "Seeking to {:.3f}s", time_s);
  // 1. Convert time to base frame serial (no actual seek)
  int base_serial = sources_[main_id_]->TimeToFrameSerial(time_s);
  Logger->debug("Sync seek: time={:.3f}s base_serial={}", time_s, base_serial);

  if (base_serial < 0) {
    // Fallback: use SeekTo for all videos when frame index is unavailable
    for (auto &[id, source] : sources_) {
      source->SeekTo(time_s);
    }
  } else {
    // 2. Seek all videos uniformly via SeekToFrameSerial(base_serial + offset)
    for (auto &[id, source] : sources_) {
      int frame_offset = video_offsets_.count(id) ? video_offsets_[id].frame_offset : 0;
      int target_serial = std::max(0, base_serial + frame_offset);
      source->SeekToFrameSerial(target_serial);
      SPDLOG_LOGGER_TRACE(Logger, "Sync seek: video {} -> frame_serial={} (base={} + offset={})", id, target_serial,
                          base_serial, frame_offset);
    }
  }

  fps_window_frame_count_ = 0; // Reset FPS calculation after seek
  fps_window_start_ = std::chrono::steady_clock::now();
  fps_warmup_ = true;
  playback_fps_.store(0.0f);
  frames_outdated_ = true;
  position_updated_ = true;
  state_.store(old_state);
}

void SyncPlayer::SeekOffset(float offset_s) {
  float new_position = current_position_s_.load() + offset_s;
  SeekTo(new_position);
}

void SyncPlayer::StepNextFrame() {
  state_ = PlayState::kPaused;

  step_forward_ = true;

  frames_outdated_ = true;
  position_updated_ = true;
}

void SyncPlayer::StepPrevFrame() {
  state_ = PlayState::kPaused;

  step_forward_ = false;

  frames_outdated_ = true;
  position_updated_ = true;
}

const SyncPlayer::VideoOffset &SyncPlayer::GetVideoOffset(int video_id) const {
  auto it = video_offsets_.find(video_id);
  if (it != video_offsets_.end()) {
    return it->second;
  }
  return kDefaultVideoOffset;
}

void SyncPlayer::StepNextFrameSingle(int video_id) {
  state_ = PlayState::kPaused;
  try {
    auto frame = sources_.at(video_id)->GetNextFrame(-1);
    video_offsets_[video_id].frame_offset += 1;
    last_frames_[video_id] = frame;
    Logger->debug("StepNextFrameSingle: video {} frame_offset={}", video_id, video_offsets_[video_id].frame_offset);
    if (frame_callback_) {
      frame_callback_(last_frames_);
    }
  } catch (const std::out_of_range &e) {
    Logger->debug("Single step forward: video {} reached boundary: {}", video_id, e.what());
  }
}

void SyncPlayer::StepPrevFrameSingle(int video_id) {
  state_ = PlayState::kPaused;
  try {
    auto frame = sources_.at(video_id)->GetPrevFrame(-1);
    video_offsets_[video_id].frame_offset -= 1;
    last_frames_[video_id] = frame;
    Logger->debug("StepPrevFrameSingle: video {} frame_offset={}", video_id, video_offsets_[video_id].frame_offset);
    if (frame_callback_) {
      frame_callback_(last_frames_);
    }
  } catch (const std::out_of_range &e) {
    Logger->debug("Single step backward: video {} reached boundary: {}", video_id, e.what());
  }
}

void SyncPlayer::SeekOffsetSingle(int video_id, float offset_s) {
  state_ = PlayState::kPaused;

  if (last_frames_.find(video_id) == last_frames_.end()) {
    Logger->warn("SeekOffsetSingle: no cached frame for video {}", video_id);
    return;
  }

  // 1. Record serial before seek
  int serial_before = sources_.at(video_id)->GetCurrentFrameSerial();

  // 2. Seek by time (efficient, uses keyframe index)
  float current_time = last_frames_[video_id]->pts * sources_[video_id]->TimeBase();
  float target_time = current_time + offset_s;
  float start_time = sources_[video_id]->StartTime();
  float end_time = sources_[video_id]->EndTime();
  if (!std::isnan(start_time)) {
    target_time = std::max(target_time, start_time);
  }
  if (!std::isnan(end_time)) {
    target_time = std::min(target_time, end_time);
  }

  try {
    sources_[video_id]->SeekTo(target_time);
    sources_[video_id]->WaitForFrameAvailable();
    auto frame = sources_[video_id]->GetNextFrame(-1);

    // 3. Calculate actual frame offset from serial difference
    int serial_after = sources_[video_id]->GetCurrentFrameSerial();
    int actual_delta = serial_after - serial_before;
    video_offsets_[video_id].frame_offset += actual_delta;

    Logger->debug("SeekOffsetSingle: video {} offset_s={:.1f} serial {} -> {} delta={} frame_offset={}", video_id,
                  offset_s, serial_before, serial_after, actual_delta, video_offsets_[video_id].frame_offset);

    last_frames_[video_id] = frame;
    if (frame_callback_) {
      frame_callback_(last_frames_);
    }
  } catch (const std::exception &e) {
    Logger->debug("SeekOffsetSingle: video {} seek failed: {}", video_id, e.what());
  }
}

void SyncPlayer::ResetVideoOffset(int video_id) {
  state_ = PlayState::kPaused;
  Logger->info("Resetting offset for video {}", video_id);
  video_offsets_[video_id] = VideoOffset{};

  if (video_id != main_id_ && !last_frames_.empty()) {
    // Re-align to main video's current frame serial
    int main_serial = sources_[main_id_]->GetCurrentFrameSerial();
    Logger->debug("ResetVideoOffset: video {} re-aligning to main serial {}", video_id, main_serial);
    sources_[video_id]->SeekToFrameSerial(main_serial);
    sources_[video_id]->WaitForFrameAvailable();
    auto frame = sources_[video_id]->GetNextFrame(-1);
    last_frames_[video_id] = frame;
  }

  if (frame_callback_ && !last_frames_.empty()) {
    frame_callback_(last_frames_);
  }
}

void SyncPlayer::ResetAllVideoOffsets() {
  state_ = PlayState::kPaused;
  Logger->info("Resetting all video offsets");

  int main_serial = sources_[main_id_]->GetCurrentFrameSerial();

  for (auto &[id, source] : sources_) {
    video_offsets_[id] = VideoOffset{};
    if (id != main_id_ && !last_frames_.empty()) {
      Logger->debug("ResetAllVideoOffsets: video {} re-aligning to main serial {}", id, main_serial);
      source->SeekToFrameSerial(main_serial);
      source->WaitForFrameAvailable();
      auto frame = source->GetNextFrame(-1);
      last_frames_[id] = frame;
    }
  }

  if (frame_callback_ && !last_frames_.empty()) {
    frame_callback_(last_frames_);
  }
}

std::map<int, std::shared_ptr<AVFrame>> SyncPlayer::GetFrames(bool forward) {
  std::map<int, std::shared_ptr<AVFrame>> frames;

  SPDLOG_LOGGER_TRACE(Logger, "getting frames...");
  for (auto &[id, source] : sources_) {
    int try_count = 0;
    while (try_count <= 3) {
      try_count += 1;
      try {
        if (forward) {
          frames[id] = source->GetNextFrame(-1);
        } else {
          frames[id] = source->GetPrevFrame(-1);
        }
        Logger->trace("Get frame {} from source {}, pts {}, duration {}, type {}", fmt::ptr(frames[id].get()), id,
                      frames[id]->pts, frames[id]->duration, av_get_picture_type_char(frames[id]->pict_type));
        break;
      } catch (const timeout_error &e) {
        SPDLOG_LOGGER_TRACE(Logger, "timeout when getting frame from source {}", id);
      }
    }
    // TODO: Show prompt when frame retrieval times out
  }
  return frames;
}

void SyncPlayer::PlayerLoop() {
  // Set thread name for debugging
#ifdef __APPLE__
  pthread_setname_np("SyncPlayer::PlayerLoop()");
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), "SyncPlayer::Player()");
#endif

  std::map<int, std::shared_ptr<AVFrame>> frames;
  while (state_.load() != PlayState::kStopped) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kUpdateIntervalMS));

    if (frames_outdated_) {
      try {
        frames = GetFrames(step_forward_);
        step_forward_ = true; // Default to forward playback at all times
      } catch (const timeout_error &e) {
        Logger->debug("Cannot get frames right now, wait for a moment");
        continue;
      } catch (const std::out_of_range &e) {
        Logger->debug("{}: {}", step_forward_ ? "Reached EOF" : "Reached start frame", e.what());
        if (PlayState::kPlaying == state_) {
          Logger->debug("change state to paused");
          Pause();
        }
        frames_outdated_ = false;
        step_forward_ = true;
        continue;
      }

      // Calculate playback FPS (fixed time window; skip first window after start/seek)
      {
        auto now = std::chrono::steady_clock::now();
        fps_window_frame_count_++;
        auto elapsed_s =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_window_start_).count() / 1000.0f;
        if (elapsed_s >= kFpsWindowSeconds) {
          if (fps_warmup_) {
            fps_warmup_ = false;
          } else {
            playback_fps_.store(static_cast<float>(fps_window_frame_count_) / elapsed_s);
          }
          fps_window_start_ = now;
          fps_window_frame_count_ = 0;
        }
      }

      // Update position first to ensure CurrentTime() returns the correct value in frame callback
      last_frame_time_s_ = (frames[main_id_]->pts + frames[main_id_]->duration) * sources_[main_id_]->TimeBase();
      if (position_updated_) {
        current_position_s_ = frames[main_id_]->pts * sources_[main_id_]->TimeBase();
        last_update_time_ = std::chrono::steady_clock::now();
      }

      if (frame_callback_) {
        last_frames_ = frames; // Cache frames for single-video operations
        frame_callback_(frames);

        if (last_width_ != Width() || last_height_ != Height()) {
          if (last_width_ > 0 && last_height_ > 0) {
            Logger->debug("Resolution changed from {}x{} to {}x{}", last_width_, last_height_, Width(), Height());
            resolution_changed_callback_();
          }
          last_width_ = Width();
          last_height_ = Height();
        }
      }
      SPDLOG_LOGGER_TRACE(Logger, "last_frame_time_s_ changed to {:.3f}s, callback to feed frames", last_frame_time_s_);

      frames_outdated_ = false;
      position_updated_ = false;
      if (start_on_pause_) {
        state_ = PlayState::kPaused;
        start_on_pause_ = false;
      }
    }

    if (state_.load() == PlayState::kPaused) {
      continue;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_update_time_).count();
    last_update_time_ = now;

    float new_position = current_position_s_.load() + (elapsed_us / 1000000.);
    current_position_s_.store(new_position);
    // Check if current time exceeds frame's pts + dur; if so, fetch new frame for display
    if (last_frame_time_s_ < new_position) {
      SPDLOG_LOGGER_TRACE(Logger, "new_position {:.3f}s > last_frame_time_s_ {:.3f}s, need to update frames",
                          new_position, last_frame_time_s_);
      frames_outdated_ = true;
    }

    // Check if flicker interval has elapsed
    if (flicker_interval_s_ > 0) {
      if (current_position_s_ > last_flicker_time_s_ + flicker_interval_s_) {
        last_flicker_time_s_ = current_position_s_;
        flicker_callback_();
      }
    }
  }
}
