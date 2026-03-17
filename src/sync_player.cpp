#include "sync_player.hpp"

#include "utils.hpp"

using namespace EYEQ;

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
  Logger->info("\tstream {}: {}, {:.3f} fps, start time {:.3f}s, duration {:.3f}s", source->StreamIndex(),
               source->CodecString(), source->FrameRate(), source->StartTime(), source->Duration());

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

int SyncPlayer::GetCurrentFrameSerial() const { return sources_.at(main_id_)->GetCurrentFrameSerial(); }

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

void SyncPlayer::StartSources(float seek_to, size_t seek_frames) {
  for (auto &[id, source] : sources_) {
    if (seek_to > 0) {
      source->SeekTo(seek_to, 0);
    }
    source->Start(seek_frames);
  }
  if (seek_to > 0 || seek_frames > 0) {
    Logger->info("Seek ready");
  }

  for (auto &[id, source] : sources_) {
    source->WaitForFrameAvailable();
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
    Logger->warn("SyncPlayer::Play() called when not playing");
  }
}

void SyncPlayer::Pause() {
  if (PlayState::kPlaying == state_) {
    state_.store(PlayState::kPaused);
  } else {
    Logger->warn("SyncPlayer::Pause() called when not paused");
  }
}

void SyncPlayer::InvertPause() {
  if (PlayState::kPlaying == state_) {
    Pause();
  } else if (PlayState::kPaused == state_) {
    Play();
  } else {
    Logger->warn("SyncPlayer::InvertPause() called when not playing or paused");
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
      Logger->info("Seeking to {:.3f}s, which is before the start time", time_s);
    }
  }
  float end_time = EndTime();
  if (!std::isnan(end_time)) {
    if (end_time < time_s) {
      time_s = end_time;
      Logger->info("Seeking to {:.3f}s, which is after the end time", time_s);
    }
  }

  SPDLOG_LOGGER_TRACE(Logger, "Seeking to {:.3f}s", time_s);
  // Pause first, then seek. Prevents newly fetched frames in PlayerLoop() from affecting display
  // 1. Seek main video by time; returns the frame serial of the keyframe seeked to
  int main_serial = sources_[main_id_]->SeekTo(time_s);
  Logger->debug("Sync seek: main(id={}) frame_serial={}", main_id_, main_serial);

  // 2. Seek other videos to the same frame serial
  for (auto &[id, source] : sources_) {
    if (id == main_id_)
      continue;
    source->SeekToFrameSerial(main_serial);
    Logger->debug("Sync seek: video {} seek to frame_serial={}", id, main_serial);
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
        Logger->debug("timeout when getting frame from source {}", id);
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
        Logger->info("Cannot get frames right now, wait for a moment");
        continue;
      } catch (const std::out_of_range &e) {
        Logger->info("{}: {}", step_forward_ ? "Reached EOF" : "Reached start frame", e.what());
        if (PlayState::kPlaying == state_) {
          Logger->info("change state to paused");
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
        frame_callback_(frames);

        if (last_width_ != Width() || last_height_ != Height()) {
          if (last_width_ > 0 && last_height_ > 0) {
            Logger->info("Resolution changed from {}x{} to {}x{}", last_width_, last_height_, Width(), Height());
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
