#include "sync_player.hpp"

#include "utils.hpp"

using namespace EYEQ;

namespace {

FetchedFrame MakeFetchedFrame(VideoSource *source, std::shared_ptr<AVFrame> frame) {
  FetchedFrame fetched;
  fetched.frame = std::move(frame);
  // Bind the metadata at fetch time: the source cursor still points at this
  // frame, so a later seek/reset cannot relabel it.
  fetched.serial = source->GetCurrentFrameSerial();
  fetched.total_frames = source->GetTotalFrames();
  fetched.pict_type = av_get_picture_type_char(fetched.frame->pict_type);
  fetched.pts = fetched.frame->pts;
  fetched.pts_s = fetched.frame->pts * source->TimeBase();
  return fetched;
}

} // namespace

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

std::vector<int> SyncPlayer::CheckUnusedHardwareDecoders() const {
  std::vector<int> unused_ids;
  for (auto &[id, source] : sources_) {
    if (source->IsHardwareDecoderRequested() && !source->IsHardwareDecoderUsed()) {
      unused_ids.push_back(id);
    }
  }
  return unused_ids;
}

void SyncPlayer::StartSources(float seek_to, size_t seek_to_frame) {
  // Start the decode threads first: a running seek needs the decode thread to
  // acknowledge it. Seeking before Start() used to time out immediately.
  for (auto &[id, source] : sources_) {
    source->Start(seek_to_frame);
  }

  if (seek_to > 0) {
    Logger->debug("Seeking to {:.3f}s during startup", seek_to);
    for (auto &[id, source] : sources_) {
      source->SeekTo(seek_to);
    }
  } else if (seek_to_frame > 0) {
    Logger->debug("Startup positioned at frame {}", seek_to_frame);
  }

  for (auto &[id, source] : sources_) {
    source->WaitForFrameAvailable();
  }

  if (seek_to > 0 || seek_to_frame > 0) {
    Logger->debug("Seek ready");
  }
}

uint64_t SyncPlayer::SubmitControl(ControlCommand cmd) {
  cmd.request_id = next_request_id_.fetch_add(1) + 1;
  const uint64_t request_id = cmd.request_id;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (closing_.load() || PlayState::kStopped == state_.load()) {
      // A stopped player still reports exactly one terminal result per request.
      RecordOutcome(ControlOutcome{request_id, cmd.kind, ControlStatus::Cancelled, "player is stopped", 0});
      return request_id;
    }
    // Total budget over all outstanding work. A submission that is rejected
    // still gets exactly one outcome, so no accepted request's result is ever
    // dropped to make room.
    const size_t budget = control_commands_.size() + executing_.load() + outstanding_outcomes_.load();
    if (control_commands_.size() >= kMaxPendingControls || budget >= kMaxOutstandingWork) {
      RecordOutcome(ControlOutcome{request_id, cmd.kind, ControlStatus::Rejected, "control budget is full", 0});
      Logger->error("Control budget exhausted (queue {} outstanding {}); request {} rejected", control_commands_.size(),
                    budget, request_id);
      return request_id;
    }
    control_commands_.push_back(std::move(cmd));
    control_cv_.notify_all();
  }
  return request_id;
}

void SyncPlayer::RecordOutcome(ControlOutcome outcome) {
  std::lock_guard<std::mutex> lock(outcome_mutex_);
  control_outcomes_.push_back(std::move(outcome));
  outstanding_outcomes_.fetch_add(1);
}

uint64_t SyncPlayer::Play() {
  ControlCommand cmd;
  cmd.kind = ControlKind::Play;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::Pause() {
  ControlCommand cmd;
  cmd.kind = ControlKind::Pause;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::InvertPause() {
  ControlCommand cmd;
  cmd.kind = ControlKind::TogglePause;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::SeekTo(float time_s) {
  ControlCommand cmd;
  cmd.kind = ControlKind::SeekTo;
  cmd.offset_s = time_s;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::SeekOffset(float offset_s) {
  ControlCommand cmd;
  cmd.kind = ControlKind::SeekOffset;
  cmd.offset_s = offset_s;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::StepNextFrame() {
  ControlCommand cmd;
  cmd.kind = ControlKind::StepNext;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::StepPrevFrame() {
  ControlCommand cmd;
  cmd.kind = ControlKind::StepPrev;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::StepNextFrameSingle(int video_id) {
  ControlCommand cmd;
  cmd.kind = ControlKind::StepNextSingle;
  cmd.video_id = video_id;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::StepPrevFrameSingle(int video_id) {
  ControlCommand cmd;
  cmd.kind = ControlKind::StepPrevSingle;
  cmd.video_id = video_id;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::SeekOffsetSingle(int video_id, float offset_s) {
  ControlCommand cmd;
  cmd.kind = ControlKind::SeekOffsetSingle;
  cmd.video_id = video_id;
  cmd.offset_s = offset_s;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::ResetVideoOffset(int video_id) {
  ControlCommand cmd;
  cmd.kind = ControlKind::ResetVideoOffset;
  cmd.video_id = video_id;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::ResetAllVideoOffsets() {
  ControlCommand cmd;
  cmd.kind = ControlKind::ResetAllVideoOffsets;
  return SubmitControl(std::move(cmd));
}

uint64_t SyncPlayer::RequestFormatFallback(const std::vector<int> &video_ids) {
  ControlCommand cmd;
  cmd.kind = ControlKind::RequestFormatFallback;
  cmd.video_ids = video_ids;
  return SubmitControl(std::move(cmd));
}

std::shared_ptr<const FrameSnapshot> SyncPlayer::TakeSnapshot() {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  if (!latest_snapshot_ || latest_snapshot_->generation == taken_generation_) {
    return nullptr;
  }
  taken_generation_ = latest_snapshot_->generation;
  return latest_snapshot_;
}

std::vector<ControlOutcome> SyncPlayer::TakeControlOutcomes() {
  std::lock_guard<std::mutex> lock(outcome_mutex_);
  std::vector<ControlOutcome> outcomes;
  outcomes.reserve(control_outcomes_.size());
  while (!control_outcomes_.empty()) {
    outcomes.push_back(std::move(control_outcomes_.front()));
    control_outcomes_.pop_front();
  }
  outstanding_outcomes_.store(0);
  return outcomes;
}

bool SyncPlayer::ConsumeFatalError(std::string *message) {
  if (!fatal_error_.exchange(false)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(fatal_mutex_);
  if (message) {
    *message = fatal_message_;
  }
  return true;
}

void SyncPlayer::SetFatalError(const std::string &message) {
  {
    std::lock_guard<std::mutex> lock(fatal_mutex_);
    fatal_message_ = message;
  }
  fatal_error_.store(true);
  Logger->critical("SyncPlayer fatal error: {}", message);
}

void SyncPlayer::Start(bool pause) {
  if (PlayState::kStopped != state_.load()) {
    throw std::runtime_error("SyncPlayer::Start() called when not stopped");
  }

  // Stop() marks every source dead; a later Start() is a restart and must bring
  // the decode pipeline back up. Undelivered outcomes are intentionally kept:
  // they are terminal results the UI has not consumed yet.
  const bool restart = closing_.load();

  current_position_s_.store(0);
  last_update_time_ = std::chrono::steady_clock::now();
  last_frame_time_s_ = -1.;
  last_flicker_time_s_ = 0;
  fps_window_start_ = std::chrono::steady_clock::now();
  fps_window_frame_count_ = 0;
  fps_warmup_ = true;
  start_on_pause_ = pause;
  current_frames_.clear();
  last_width_ = -1;
  last_height_ = -1;

  if (restart) {
    Logger->info("Restarting playback after Stop()");
    for (auto &[id, source] : sources_) {
      source->Restart();
    }
  }
  // Only clear the stop marker once every source came back up; if a restart
  // throws, a later Start() must still know it has to retry the restart.
  closing_.store(false);

  // Arm the loop state before the thread exists. Setting it afterwards used to
  // leave a window where PlayerLoop observed kStopped and exited immediately
  // while Start() then marked the player as playing.
  state_.store(PlayState::kPlaying);
  loop_thread_ = std::thread(&SyncPlayer::PlayerLoop, this);
}

void SyncPlayer::Stop() {
  closing_.store(true);
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    // Every accepted-but-unfinished request gets exactly one Cancelled result.
    for (auto &cmd : control_commands_) {
      RecordOutcome(ControlOutcome{cmd.request_id, cmd.kind, ControlStatus::Cancelled, "player stopped", 0});
    }
    control_commands_.clear();
  }
  state_.store(PlayState::kStopped);
  control_cv_.notify_all();

  // Cancel every blocking wait before joining, so PlayerLoop (possibly inside a
  // seek or a frame fetch) can return instead of blocking the join.
  for (auto &[id, source] : sources_) {
    source->Stop();
  }

  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    latest_snapshot_.reset();
  }
  current_frames_.clear();
}

void SyncPlayer::FetchCompleteSet(bool forward, FrameSet &out) {
  out.clear();
  std::vector<int> fetched;
  try {
    for (auto &[id, source] : sources_) {
      auto frame = forward ? source->GetNextFrame(kTooLongIntervalMS) : source->GetPrevFrame(kTooLongIntervalMS);
      out.emplace(id, MakeFetchedFrame(source.get(), std::move(frame)));
      fetched.push_back(id);
    }
  } catch (...) {
    RollbackFetched(fetched, forward);
    throw;
  }
}

void SyncPlayer::RollbackFetched(const std::vector<int> &fetched_ids, bool forward) {
  for (int id : fetched_ids) {
    auto it = sources_.find(id);
    if (it == sources_.end()) {
      continue;
    }
    VideoSource *source = it->second.get();
    try {
      if (forward) {
        // Restore the equivalent cursor: the source must look as if it had just
        // consumed the committed frame. Seeking to the committed serial and
        // consuming that frame makes the next forward fetch return
        // committed_serial + 1 while keeping a valid cursor for a backward step.
        // (Seeking to committed + 1 left the cursor at "nothing consumed", so a
        // following GetPrevFrame threw and back-stepping was stuck.)
        auto committed = current_frames_.find(id);
        if (committed != current_frames_.end()) {
          source->SeekToFrameSerial(committed->second.serial);
          source->GetNextFrame(-1);
        } else {
          // Nothing committed yet: rewind to the first frame without consuming
          // it, so a retry starts from serial 0.
          source->SeekToFrameSerial(0);
        }
      } else {
        // Undo one backward step, so the cursor is back on the committed frame
        // (next forward fetch returns committed + 1, next backward step can
        // decode earlier frames).
        source->GetNextFrame(kTooLongIntervalMS);
      }
    } catch (const stopped_error &) {
      return; // stop requested; nothing to restore
    } catch (const std::exception &e) {
      SetFatalError(fmt::format("could not roll back source #{} after an incomplete fetch: {}", id, e.what()));
      return;
    }
  }
}

void SyncPlayer::EnsureCompleteFrameSet() {
  if (current_frames_.size() == sources_.size()) {
    return;
  }
  FrameSet set;
  FetchCompleteSet(true, set);
  current_frames_ = std::move(set);
}

uint64_t SyncPlayer::PublishFrameSet(const FrameSet &set) {
  auto snapshot = std::make_shared<FrameSnapshot>();
  const uint64_t generation = snapshot_generation_.fetch_add(1) + 1;
  snapshot->generation = generation;
  snapshot->main_id = main_id_;
  for (const auto &[id, fetched] : set) {
    snapshot->frames.emplace(id, fetched.frame);
    FrameMetadata info;
    info.video_id = id;
    info.pict_type = fetched.pict_type;
    info.frame_serial = fetched.serial;
    info.total_frames = fetched.total_frames;
    info.pts_s = fetched.pts_s;
    snapshot->metadata.emplace(id, info);
  }
  snapshot->current_time_s = current_position_s_.load();
  snapshot->start_time_s = StartTime();
  snapshot->end_time_s = EndTime();
  snapshot->playback_fps = playback_fps_.load();
  snapshot->main_fps = FrameRate();

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    // Single slot: a newer publication replaces an unconsumed one (merge policy).
    latest_snapshot_ = snapshot;
  }
  last_published_generation_ = generation;

  const int width = Width();
  const int height = Height();
  if (last_width_ != width || last_height_ != height) {
    if (last_width_ > 0 && last_height_ > 0) {
      Logger->debug("Resolution changed from {}x{} to {}x{}", last_width_, last_height_, width, height);
      resolution_changed_.store(true);
    }
    last_width_ = width;
    last_height_ = height;
  }
  return generation;
}

void SyncPlayer::CommitFrameSet(FrameSet &&set, bool update_position) {
  current_frames_ = std::move(set);
  auto main_it = current_frames_.find(main_id_);
  if (main_it != current_frames_.end()) {
    int64_t duration = main_it->second.frame->duration;
    if (duration <= 0) {
      const double fps = sources_[main_id_]->FrameRate();
      duration = (fps > 0) ? static_cast<int64_t>(1.0 / (sources_[main_id_]->TimeBase() * fps)) : 0;
    }
    last_frame_time_s_ = (main_it->second.pts + duration) * sources_[main_id_]->TimeBase();
    if (update_position) {
      current_position_s_.store(main_it->second.pts_s);
      last_update_time_ = std::chrono::steady_clock::now();
    }
  }
  PublishFrameSet(current_frames_);
}

void SyncPlayer::DoSeek(float time_s) {
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

  // 3. Fetch the positioned frames and publish them as one consistent snapshot.
  FrameSet set;
  FetchCompleteSet(true, set);
  CommitFrameSet(std::move(set), true);
}

void SyncPlayer::ResetVideoOffsetImpl(int video_id) {
  Logger->info("Resetting offset for video {}", video_id);
  EnsureCompleteFrameSet();
  video_offsets_[video_id] = VideoOffset{};

  FrameSet set = current_frames_;
  if (video_id != main_id_) {
    // Re-align to main video's current frame serial
    int main_serial = sources_.at(main_id_)->GetCurrentFrameSerial();
    Logger->debug("ResetVideoOffset: video {} re-aligning to main serial {}", video_id, main_serial);
    sources_.at(video_id)->SeekToFrameSerial(main_serial);
    sources_.at(video_id)->WaitForFrameAvailable();
    auto frame = sources_.at(video_id)->GetNextFrame(kTooLongIntervalMS);
    set[video_id] = MakeFetchedFrame(sources_.at(video_id).get(), std::move(frame));
  }

  CommitFrameSet(std::move(set), false);
}

void SyncPlayer::ResetAllVideoOffsetsImpl() {
  Logger->info("Resetting all video offsets");
  EnsureCompleteFrameSet();

  int main_serial = sources_.at(main_id_)->GetCurrentFrameSerial();
  FrameSet set = current_frames_;
  for (auto &[id, source] : sources_) {
    video_offsets_[id] = VideoOffset{};
    if (id != main_id_) {
      Logger->debug("ResetAllVideoOffsets: video {} re-aligning to main serial {}", id, main_serial);
      source->SeekToFrameSerial(main_serial);
      source->WaitForFrameAvailable();
      auto frame = source->GetNextFrame(kTooLongIntervalMS);
      set[id] = MakeFetchedFrame(source.get(), std::move(frame));
    }
  }

  CommitFrameSet(std::move(set), false);
}

void SyncPlayer::SeekOffsetSingleImpl(int video_id, float offset_s) {
  EnsureCompleteFrameSet();
  if (current_frames_.find(video_id) == current_frames_.end()) {
    throw std::runtime_error(fmt::format("SeekOffsetSingle: no cached frame for video {}", video_id));
  }

  VideoSource *source = sources_.at(video_id).get();
  // 1. Record serial before seek
  int serial_before = source->GetCurrentFrameSerial();

  // 2. Seek by time (efficient, uses keyframe index)
  float current_time = current_frames_.at(video_id).pts_s;
  float target_time = current_time + offset_s;
  float start_time = source->StartTime();
  float end_time = source->EndTime();
  if (!std::isnan(start_time)) {
    target_time = std::max(target_time, start_time);
  }
  if (!std::isnan(end_time)) {
    target_time = std::min(target_time, end_time);
  }

  source->SeekTo(target_time);
  source->WaitForFrameAvailable();
  auto frame = source->GetNextFrame(kTooLongIntervalMS);
  FetchedFrame fetched = MakeFetchedFrame(source, std::move(frame));

  // 3. Calculate actual frame offset from serial difference
  int serial_after = fetched.serial;
  int actual_delta = serial_after - serial_before;
  video_offsets_[video_id].frame_offset += actual_delta;

  Logger->debug("SeekOffsetSingle: video {} offset_s={:.1f} serial {} -> {} delta={} frame_offset={}", video_id,
                offset_s, serial_before, serial_after, actual_delta, video_offsets_[video_id].frame_offset);

  FrameSet set = current_frames_;
  set[video_id] = std::move(fetched);
  CommitFrameSet(std::move(set), false);
}

ControlOutcome SyncPlayer::ExecuteControl(const ControlCommand &cmd) {
  ControlOutcome outcome;
  outcome.request_id = cmd.request_id;
  outcome.kind = cmd.kind;
  const uint64_t generation_before = last_published_generation_;

  try {
    switch (cmd.kind) {
    case ControlKind::Play:
      if (PlayState::kPaused == state_.load()) {
        last_update_time_ = std::chrono::steady_clock::now();
        state_.store(PlayState::kPlaying);
      } else {
        outcome.status = ControlStatus::Failed;
        outcome.message = "Play() called when not paused";
      }
      break;
    case ControlKind::Pause:
      if (PlayState::kPlaying == state_.load()) {
        state_.store(PlayState::kPaused);
      } else {
        outcome.status = ControlStatus::Failed;
        outcome.message = "Pause() called when not playing";
      }
      break;
    case ControlKind::TogglePause:
      if (PlayState::kPlaying == state_.load()) {
        state_.store(PlayState::kPaused);
      } else if (PlayState::kPaused == state_.load()) {
        last_update_time_ = std::chrono::steady_clock::now();
        state_.store(PlayState::kPlaying);
      } else {
        outcome.status = ControlStatus::Failed;
        outcome.message = "InvertPause() called when not playing or paused";
      }
      break;
    case ControlKind::SeekTo:
      DoSeek(cmd.offset_s);
      break;
    case ControlKind::SeekOffset:
      DoSeek(current_position_s_.load() + cmd.offset_s);
      break;
    case ControlKind::StepNext:
    case ControlKind::StepPrev: {
      const bool forward = (cmd.kind == ControlKind::StepNext);
      FrameSet set;
      FetchCompleteSet(forward, set);
      CommitFrameSet(std::move(set), true);
      break;
    }
    case ControlKind::StepNextSingle:
    case ControlKind::StepPrevSingle: {
      if (!sources_.count(cmd.video_id)) {
        outcome.status = ControlStatus::Failed;
        outcome.message = fmt::format("no video source #{}", cmd.video_id);
        break;
      }
      EnsureCompleteFrameSet();
      const bool forward = (cmd.kind == ControlKind::StepNextSingle);
      VideoSource *source = sources_.at(cmd.video_id).get();
      auto frame = forward ? source->GetNextFrame(kTooLongIntervalMS) : source->GetPrevFrame(kTooLongIntervalMS);
      video_offsets_[cmd.video_id].frame_offset += forward ? 1 : -1;
      Logger->debug("Step{}Single: video {} frame_offset={}", forward ? "Next" : "Prev", cmd.video_id,
                    video_offsets_[cmd.video_id].frame_offset);
      FrameSet set = current_frames_;
      set[cmd.video_id] = MakeFetchedFrame(source, std::move(frame));
      CommitFrameSet(std::move(set), false);
      break;
    }
    case ControlKind::SeekOffsetSingle:
      if (!sources_.count(cmd.video_id)) {
        outcome.status = ControlStatus::Failed;
        outcome.message = fmt::format("no video source #{}", cmd.video_id);
        break;
      }
      SeekOffsetSingleImpl(cmd.video_id, cmd.offset_s);
      break;
    case ControlKind::ResetVideoOffset:
      if (!sources_.count(cmd.video_id)) {
        outcome.status = ControlStatus::Failed;
        outcome.message = fmt::format("no video source #{}", cmd.video_id);
        break;
      }
      ResetVideoOffsetImpl(cmd.video_id);
      break;
    case ControlKind::ResetAllVideoOffsets:
      ResetAllVideoOffsetsImpl();
      break;
    case ControlKind::RequestFormatFallback: {
      bool all_accepted = true;
      for (int id : cmd.video_ids) {
        auto it = sources_.find(id);
        if (it == sources_.end()) {
          all_accepted = false;
          outcome.message = fmt::format("no video source #{} for format fallback", id);
          continue;
        }
        if (!it->second->RequestFormatFallback()) {
          all_accepted = false;
          outcome.message = fmt::format("video #{} rejected format fallback", id);
        }
      }
      if (all_accepted) {
        // Re-seek so the rebuilt filter graph produces frames at the current
        // position; this is the explicit owner-controlled fallback path.
        DoSeek(current_position_s_.load());
      } else {
        outcome.status = ControlStatus::Failed;
      }
      break;
    }
    }
  } catch (const stopped_error &e) {
    outcome.status = ControlStatus::Cancelled;
    outcome.message = e.what();
  } catch (const std::out_of_range &e) {
    outcome.status = ControlStatus::Boundary;
    outcome.message = e.what();
  } catch (const timeout_error &e) {
    outcome.status = ControlStatus::Failed;
    outcome.message = e.what();
  } catch (const std::exception &e) {
    outcome.status = ControlStatus::Failed;
    outcome.message = e.what();
  }
  outcome.snapshot_generation = (last_published_generation_ != generation_before) ? last_published_generation_ : 0;
  return outcome;
}

void SyncPlayer::PlaybackStep() {
  auto now = std::chrono::steady_clock::now();
  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_update_time_).count();
  last_update_time_ = now;

  const float new_position = current_position_s_.load() + (elapsed_us / 1000000.);
  current_position_s_.store(new_position);

  if (last_frame_time_s_ < new_position) {
    try {
      FrameSet set;
      FetchCompleteSet(true, set);
      CommitFrameSet(std::move(set), false);

      // Calculate playback FPS (fixed time window; skip first window after start/seek)
      auto fps_now = std::chrono::steady_clock::now();
      fps_window_frame_count_++;
      auto elapsed_s =
          std::chrono::duration_cast<std::chrono::milliseconds>(fps_now - fps_window_start_).count() / 1000.0f;
      if (elapsed_s >= kFpsWindowSeconds) {
        if (fps_warmup_) {
          fps_warmup_ = false;
        } else {
          playback_fps_.store(static_cast<float>(fps_window_frame_count_) / elapsed_s);
        }
        fps_window_start_ = fps_now;
        fps_window_frame_count_ = 0;
      }

      if (start_on_pause_) {
        state_.store(PlayState::kPaused);
        start_on_pause_ = false;
      }
    } catch (const stopped_error &) {
      // Stop() sets kStopped before cancelling the sources; anything else means
      // a decoder died under a running loop. Enter an observable terminal state
      // instead of spinning silently at the control update rate forever.
      if (PlayState::kStopped != state_.load()) {
        SetFatalError("frame fetch stopped unexpectedly");
        state_.store(PlayState::kStopped);
      }
    } catch (const timeout_error &e) {
      Logger->debug("Cannot get frames right now, wait for a moment");
    } catch (const std::out_of_range &e) {
      Logger->debug("Reached stream boundary: {}", e.what());
      if (PlayState::kPlaying == state_.load()) {
        state_.store(PlayState::kPaused);
      }
    } catch (const std::exception &e) {
      SetFatalError(fmt::format("frame fetch failed: {}", e.what()));
      state_.store(PlayState::kStopped);
    }
  }

  // Flicker on playback-time intervals
  if (flicker_interval_s_ > 0 && current_position_s_.load() > last_flicker_time_s_ + flicker_interval_s_) {
    last_flicker_time_s_ = current_position_s_.load();
    flicker_events_.fetch_add(1);
  }
}

void SyncPlayer::PlayerLoop() {
  // Set thread name for debugging
#ifdef __APPLE__
  pthread_setname_np("SyncPlayer::PlayerLoop()");
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), "SyncPlayer::Player()");
#endif

  while (state_.load() != PlayState::kStopped) {
    // Execute at most one accepted command to completion before doing anything
    // else; the next command only starts after this one's outcome is recorded.
    ControlCommand cmd;
    bool have_cmd = false;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      if (!closing_.load() && !control_commands_.empty()) {
        cmd = std::move(control_commands_.front());
        control_commands_.pop_front();
        have_cmd = true;
      }
    }
    if (have_cmd) {
      executing_.fetch_add(1);
      ControlOutcome outcome = ExecuteControl(cmd);
      executing_.fetch_sub(1);
      RecordOutcome(std::move(outcome));
      continue;
    }

    if (PlayState::kPaused == state_.load()) {
      std::unique_lock<std::mutex> lock(control_mutex_);
      if (!closing_.load() && control_commands_.empty() && PlayState::kPaused == state_.load()) {
        control_cv_.wait_for(lock, std::chrono::milliseconds(kUpdateIntervalMS));
      }
      continue;
    }
    if (PlayState::kStopped == state_.load()) {
      break;
    }

    PlaybackStep();
    std::this_thread::sleep_for(std::chrono::milliseconds(kUpdateIntervalMS));
  }
}
