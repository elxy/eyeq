// 2C-1 player tests: frame snapshot publication and serialized cursor control.
//
// These tests run the production SyncPlayer/VideoSource over locally synthesized
// YUV4MPEG2 files (rawvideo, one keyframe per frame), so the real decode, seek,
// step and publication paths are exercised without a GPU, DeckLink SDK or
// window. Assertions use bounded deadlines with an observable signal; no test
// relies on a fixed sleep to "probably" pass.
//
// The adversarial cases mirror the R1 review probes: strict FIFO command
// completion, relative seek accumulation, metadata bound to the fetched frame,
// consistent state after a multi-source EOF, bounded accepted work, explicit
// stop outcomes and seek-failure propagation.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "sync_player.hpp"
#include "utils.hpp"
#include "video_source.hpp"

using namespace EYEQ;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (condition) {
    std::fprintf(stdout, "  ok   %s\n", what.c_str());
  } else {
    std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

using Clock = std::chrono::steady_clock;

Clock::time_point Deadline(int timeout_ms) { return Clock::now() + std::chrono::milliseconds(timeout_ms); }

constexpr int kWidth = 64;
constexpr int kHeight = 48;

// Y plane content: byte at plane offset p of frame i. Includes a per-file shift
// so the two sources are distinguishable.
uint8_t ExpectedY(int frame_index, size_t offset, int shift) {
  return static_cast<uint8_t>((frame_index * 7 + static_cast<int>(offset) + shift) & 0xFF);
}

void WriteY4M(const std::filesystem::path &path, int frames, int shift) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("cannot create " + path.string());
  }
  out << "YUV4MPEG2 W" << kWidth << " H" << kHeight << " F25:1 Ip A1:1 C420mpeg2\n";
  std::vector<uint8_t> y(kWidth * kHeight);
  std::vector<uint8_t> u((kWidth / 2) * (kHeight / 2), 128);
  std::vector<uint8_t> v((kWidth / 2) * (kHeight / 2), 128);
  for (int i = 0; i < frames; ++i) {
    out << "FRAME\n";
    for (size_t p = 0; p < y.size(); ++p) {
      y[p] = ExpectedY(i, p, shift);
    }
    out.write(reinterpret_cast<const char *>(y.data()), static_cast<std::streamsize>(y.size()));
    out.write(reinterpret_cast<const char *>(u.data()), static_cast<std::streamsize>(u.size()));
    out.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(v.size()));
  }
}

bool FrameMatchesSerial(const AVFrame *frame, int frame_index, int shift) {
  if (!frame || frame->width != kWidth || frame->height != kHeight || frame->data[0] == nullptr) {
    return false;
  }
  const int linesize = frame->linesize[0];
  // Sample a sparse grid; enough to detect a wrong frame without scanning all pixels.
  for (int row = 0; row < kHeight; row += 7) {
    for (int col = 0; col < kWidth; col += 9) {
      const size_t offset = static_cast<size_t>(row) * kWidth + col;
      if (frame->data[0][row * linesize + col] != ExpectedY(frame_index, offset, shift)) {
        return false;
      }
    }
  }
  return true;
}

std::filesystem::path MakeTempDir() {
  const auto stamp = Clock::now().time_since_epoch().count();
  auto dir = std::filesystem::temp_directory_path() / ("eyeq_player_test_" + std::to_string(stamp));
  std::filesystem::create_directories(dir);
  return dir;
}

std::shared_ptr<const FrameSnapshot> WaitSnapshot(SyncPlayer &player, uint64_t min_generation, int timeout_ms = 10000) {
  const auto deadline = Deadline(timeout_ms);
  while (Clock::now() < deadline) {
    if (auto snapshot = player.TakeSnapshot(); snapshot && snapshot->generation >= min_generation) {
      return snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return nullptr;
}

std::shared_ptr<const FrameSnapshot> WaitSerial(SyncPlayer &player, int id, int serial, int timeout_ms = 10000) {
  const auto deadline = Deadline(timeout_ms);
  while (Clock::now() < deadline) {
    auto snapshot = player.TakeSnapshot();
    if (snapshot) {
      auto it = snapshot->metadata.find(id);
      if (it != snapshot->metadata.end() && it->second.frame_serial == serial) {
        return snapshot;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return nullptr;
}

bool WaitState(SyncPlayer &player, SyncPlayer::PlayState state, int timeout_ms = 10000) {
  const auto deadline = Deadline(timeout_ms);
  while (Clock::now() < deadline) {
    if (player.State() == state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

std::vector<ControlOutcome> WaitOutcomes(SyncPlayer &player, size_t count, int timeout_ms = 10000) {
  std::vector<ControlOutcome> collected;
  const auto deadline = Deadline(timeout_ms);
  while (collected.size() < count && Clock::now() < deadline) {
    for (auto &outcome : player.TakeControlOutcomes()) {
      collected.push_back(std::move(outcome));
    }
    if (collected.size() < count) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return collected;
}

ControlOutcome SubmitAndWait(SyncPlayer &player, const std::function<uint64_t()> &submit, int timeout_ms = 10000) {
  const uint64_t request_id = submit();
  const auto deadline = Deadline(timeout_ms);
  while (Clock::now() < deadline) {
    for (auto &outcome : player.TakeControlOutcomes()) {
      if (outcome.request_id == request_id) {
        return outcome;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ControlOutcome timeout;
  timeout.request_id = request_id;
  timeout.status = ControlStatus::Failed;
  timeout.message = "no control outcome within deadline";
  return timeout;
}

class TestPlayer : public SyncPlayer {
public:
  // Enqueue an atomic batch using the existing protected queue so the owner
  // cannot race between UI submissions. Execution uses the real PlayerLoop.
  void Batch(std::initializer_list<ControlCommand> commands) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    for (auto cmd : commands) {
      cmd.request_id = next_request_id_.fetch_add(1) + 1;
      control_commands_.push_back(std::move(cmd));
    }
    control_cv_.notify_all();
  }
  void StartWithFirstControl(ControlCommand cmd) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    Start(true);
    cmd.request_id = next_request_id_.fetch_add(1) + 1;
    control_commands_.push_back(std::move(cmd));
    control_cv_.notify_all();
  }
  size_t PendingCommandCount() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return control_commands_.size();
  }
};

ControlCommand Cmd(ControlKind kind, int id = -1, float offset = 0) {
  ControlCommand c;
  c.kind = kind;
  c.video_id = id;
  c.offset_s = offset;
  return c;
}

template <typename PlayerT>
std::unique_ptr<PlayerT> MakePlayerImpl(const std::filesystem::path &dir, int a_count = 32, int b_count = 32) {
  auto a = dir / "a.y4m";
  auto b = dir / "b.y4m";
  WriteY4M(a, a_count, 0);
  WriteY4M(b, b_count, 10);
  auto player = std::make_unique<PlayerT>();
  player->AddVideoSource(std::make_unique<VideoSource>(a.string(), "", 1, HardwareDecoder::None), 0);
  player->AddVideoSource(std::make_unique<VideoSource>(b.string(), "", 1, HardwareDecoder::None), 1);
  player->SetMainId(0);
  player->SetFlickerInterval(0);
  player->StartSources(-1.0f, 0);
  player->Start(true);
  if (!WaitState(*player, SyncPlayer::PlayState::kPaused)) {
    throw std::runtime_error("player did not reach the first paused frame");
  }
  return player;
}

// Plays and waits until the player loop has attempted at least one forward fetch.
// Returns true when a new snapshot was published (fetch succeeded) and false when
// the fetch hit a stream boundary (state returned to paused). Waiting for the
// Play outcome first guarantees the state transition to paused can only come
// from the fetch, making this a deterministic signal rather than a sleep.
bool PlayUntilFetchAttempt(SyncPlayer &player, uint64_t last_generation, int timeout_ms = 10000) {
  const ControlOutcome outcome = SubmitAndWait(player, [&player]() { return player.Play(); }, timeout_ms);
  if (outcome.status != ControlStatus::Ok) {
    std::fprintf(stderr, "  Play() did not succeed: %s\n", outcome.message.c_str());
    return true;
  }
  const auto deadline = Deadline(timeout_ms);
  while (Clock::now() < deadline) {
    if (auto snapshot = player.TakeSnapshot(); snapshot && snapshot->generation > last_generation) {
      return true;
    }
    if (player.State() == SyncPlayer::PlayState::kPaused) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void test_first_paused_publication_and_snapshot_lifetime() {
  std::fprintf(stdout, "[first paused publication]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir);
  auto first = WaitSnapshot(*player, 1);
  Check(first != nullptr, "a first snapshot is published");
  if (!first) {
    player->Stop();
    return;
  }
  Check(first->frames.size() == 2, "snapshot carries both frames");
  Check(first->metadata.size() == 2, "snapshot carries metadata for both sources");
  Check(first->main_id == 0, "snapshot records the main id");
  Check(first->metadata.at(0).frame_serial == 0 && first->metadata.at(1).frame_serial == 0,
        "first publication serials are 0/0");
  Check(FrameMatchesSerial(first->frames.at(0).get(), 0, 0), "source 0 frame matches its serial");
  Check(FrameMatchesSerial(first->frames.at(1).get(), 0, 10), "source 1 frame matches its serial");

  const uint64_t generation = first->generation;
  auto step = SubmitAndWait(*player, [&player]() { return player->StepNextFrame(); });
  Check(step.status == ControlStatus::Ok, "global step command completes with ok");
  Check(step.snapshot_generation > generation, "step outcome names the snapshot it published");
  auto second = WaitSnapshot(*player, generation + 1);
  Check(second != nullptr, "stepping publishes a new snapshot");
  Check(first->frames.size() == 2 && FrameMatchesSerial(first->frames.at(0).get(), 0, 0),
        "old snapshot keeps its AVFrame alive after the player advanced");
  player->Stop();
}

void test_global_step_order() {
  std::fprintf(stdout, "[global step order]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir);

  for (int expected = 1; expected <= 2; ++expected) {
    auto outcome = SubmitAndWait(*player, [&player]() { return player->StepNextFrame(); });
    Check(outcome.status == ControlStatus::Ok, "step-forward outcome is ok");
    auto next = WaitSerial(*player, 0, expected);
    Check(next != nullptr, "step-forward reaches serial " + std::to_string(expected));
    if (next) {
      Check(next->metadata.at(1).frame_serial == expected, "both sources advance together");
      Check(FrameMatchesSerial(next->frames.at(0).get(), expected, 0), "frame content matches the stepped serial");
    }
  }

  auto prev = SubmitAndWait(*player, [&player]() { return player->StepPrevFrame(); });
  Check(prev.status == ControlStatus::Ok, "step-backward outcome is ok");
  auto back = WaitSerial(*player, 0, 1);
  Check(back != nullptr, "step-backward reaches serial 1");
  if (back) {
    Check(back->metadata.at(1).frame_serial == 1, "both sources step back together");
    Check(FrameMatchesSerial(back->frames.at(0).get(), 1, 0), "stepped-back frame content is correct");
  }
  player->Stop();
}

void test_single_video_step_isolation() {
  std::fprintf(stdout, "[single-video step isolation]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir);
  auto snapshot = WaitSnapshot(*player, 1);
  if (!snapshot) {
    Check(false, "initial snapshot available");
    player->Stop();
    return;
  }
  const uint64_t generation = snapshot->generation;

  auto start_boundary = SubmitAndWait(*player, [&player]() { return player->StepPrevFrameSingle(1); });
  Check(start_boundary.status == ControlStatus::Boundary,
        "single-video back-step at the first frame reports a boundary");

  auto invalid = SubmitAndWait(*player, [&player]() { return player->StepNextFrameSingle(9); });
  Check(invalid.status == ControlStatus::Failed && !invalid.message.empty(),
        "single-video command for an unknown video id fails with a message");

  auto outcome = SubmitAndWait(*player, [&player]() { return player->StepNextFrameSingle(1); });
  Check(outcome.status == ControlStatus::Ok, "single-video step command completes");
  auto after = WaitSnapshot(*player, generation + 1);
  Check(after != nullptr, "single-video step publishes a snapshot");
  if (after) {
    Check(after->metadata.at(0).frame_serial == 0, "non-target video cursor is unchanged");
    Check(after->metadata.at(1).frame_serial == 1, "target video advanced by one");
    Check(FrameMatchesSerial(after->frames.at(1).get(), 1, 10), "target frame content matches");
  }

  auto back = SubmitAndWait(*player, [&player]() { return player->StepPrevFrameSingle(1); });
  Check(back.status == ControlStatus::Ok, "single-video back-step completes");
  auto restored = WaitSerial(*player, 1, 0);
  Check(restored != nullptr && restored->metadata.at(0).frame_serial == 0,
        "single-video back-step restores the target without moving the other source");
  player->Stop();
}

void test_single_video_seek_offset_and_reset() {
  std::fprintf(stdout, "[single-video seek offset and reset]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir, 16, 16);
  auto snapshot = WaitSnapshot(*player, 1);
  if (!snapshot) {
    Check(false, "initial snapshot available");
    player->Stop();
    return;
  }
  const uint64_t generation = snapshot->generation;

  auto outcome = SubmitAndWait(*player, [&player]() { return player->SeekOffsetSingle(1, 0.2f); });
  Check(outcome.status == ControlStatus::Ok, "single-video offset seek completes");
  auto shifted = WaitSnapshot(*player, generation + 1);
  Check(shifted != nullptr, "offset seek publishes a snapshot");
  if (shifted) {
    const int serial = shifted->metadata.at(1).frame_serial;
    Check(serial > 0, "offset seek moved the target video forward");
    Check(shifted->metadata.at(0).frame_serial == 0, "offset seek left the main video untouched");
    Check(FrameMatchesSerial(shifted->frames.at(1).get(), serial, 10), "offset seek frame content matches its serial");
    Check(shifted->metadata.at(1).total_frames == 16, "seek builds and publishes the total frame count");
  }

  auto reset = SubmitAndWait(*player, [&player]() { return player->ResetVideoOffset(1); });
  Check(reset.status == ControlStatus::Ok, "offset reset completes");
  auto realigned = WaitSerial(*player, 1, 0);
  Check(realigned != nullptr, "reset re-aligns the target to the main serial");
  if (realigned) {
    Check(realigned->metadata.at(1).frame_serial == realigned->metadata.at(0).frame_serial,
          "target and main serials match after reset");
  }
  player->Stop();
}

void test_quick_consecutive_commands() {
  std::fprintf(stdout, "[quick consecutive commands]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir, 24, 24);

  constexpr int kSteps = 10;
  for (int i = 0; i < kSteps; ++i) {
    player->StepNextFrame();
  }
  auto outcomes = WaitOutcomes(*player, kSteps);
  Check(outcomes.size() == kSteps, "every queued command produced an outcome");
  bool all_ok = true;
  for (const auto &outcome : outcomes) {
    all_ok = all_ok && outcome.status == ControlStatus::Ok;
  }
  Check(all_ok, "all queued commands executed successfully");

  auto final = WaitSerial(*player, 0, kSteps);
  Check(final != nullptr, "final state reflects all queued steps");
  if (final) {
    Check(final->metadata.at(1).frame_serial == kSteps, "both sources end at the same serial");
  }
  player->Stop();
}

void test_step_then_reset_is_fifo() {
  std::fprintf(stdout, "[step then reset is FIFO]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<TestPlayer>(dir);

  player->Batch({Cmd(ControlKind::StepNext), Cmd(ControlKind::ResetVideoOffset, 0)});
  auto outcomes = WaitOutcomes(*player, 2);
  Check(outcomes.size() == 2, "both commands produced an outcome");
  Check(outcomes.size() == 2 && outcomes[0].status == ControlStatus::Ok && outcomes[1].status == ControlStatus::Ok,
        "the queued step and reset both complete");
  auto final = WaitSerial(*player, 0, 1);
  Check(final != nullptr && final->metadata.at(1).frame_serial == 1,
        "the step completed before the reset; no acknowledged step is stranded");
  Check(player->PendingCommandCount() == 0, "no accepted command remains pending after completion");
  player->Stop();
}

void test_relative_seeks_accumulate() {
  std::fprintf(stdout, "[relative seeks accumulate]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<TestPlayer>(dir);

  player->Batch({Cmd(ControlKind::SeekOffset, -1, 0.24f), Cmd(ControlKind::SeekOffset, -1, 0.24f)});
  auto outcomes = WaitOutcomes(*player, 2);
  Check(outcomes.size() == 2 && outcomes[0].status == ControlStatus::Ok && outcomes[1].status == ControlStatus::Ok,
        "both relative seeks complete");
  auto snapshot = WaitSnapshot(*player, 3);
  Check(snapshot != nullptr, "relative seeks publish a snapshot");
  if (snapshot) {
    const int serial = snapshot->metadata.at(0).frame_serial;
    std::fprintf(stdout, "  relative seeks reached serial %d\n", serial);
    Check(serial >= 11, "two +0.24s seeks accumulate (allow one frame rounding)");
    Check(FrameMatchesSerial(snapshot->frames.at(0).get(), serial, 0), "accumulated seek frame content matches");
  }
  player->Stop();
}

void test_seek_then_reset_binds_metadata() {
  std::fprintf(stdout, "[seek then reset binds metadata to the frame]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<TestPlayer>(dir);

  player->Batch({Cmd(ControlKind::SeekTo, -1, 0.4f), Cmd(ControlKind::ResetVideoOffset, 0)});
  auto outcomes = WaitOutcomes(*player, 2);
  Check(outcomes.size() == 2 && outcomes[0].status == ControlStatus::Ok && outcomes[1].status == ControlStatus::Ok,
        "seek and reset both complete");
  auto snapshot = WaitSnapshot(*player, 3);
  Check(snapshot != nullptr, "seek then reset publishes a snapshot");
  if (snapshot) {
    const int serial = snapshot->metadata.at(0).frame_serial;
    std::fprintf(stdout, "  seek 0.4 then reset reached serial %d\n", serial);
    Check(serial == 10, "seek to 0.4s lands on serial 10");
    Check(FrameMatchesSerial(snapshot->frames.at(0).get(), serial, 0),
          "published serial is bound to the frame, not a later cursor");
  }
  player->Stop();
}

void test_short_source_eof_keeps_both_sources_aligned() {
  std::fprintf(stdout, "[short-source EOF keeps both sources aligned]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir, 16, 8);
  uint64_t generation = 1;

  for (int expected = 1; expected <= 7; ++expected) {
    auto outcome = SubmitAndWait(*player, [&player]() { return player->StepNextFrame(); });
    if (outcome.status != ControlStatus::Ok) {
      Check(false, "step to serial " + std::to_string(expected) + " completes");
      player->Stop();
      return;
    }
    auto next = WaitSerial(*player, 0, expected);
    if (!next || next->metadata.at(1).frame_serial != expected) {
      Check(false, "both sources reach serial " + std::to_string(expected));
      player->Stop();
      return;
    }
    generation = next->generation;
  }

  // The short source hits EOF. The committed state must stay at the last common
  // frame (7/7); the failed attempt must not silently misalign the comparison.
  Check(!PlayUntilFetchAttempt(*player, generation), "short-source EOF pauses without publishing");
  auto outcome = SubmitAndWait(*player, [&player]() { return player->ResetVideoOffset(0); });
  Check(outcome.status == ControlStatus::Ok, "reset after EOF completes");
  auto snapshot = WaitSnapshot(*player, generation + 1);
  Check(snapshot != nullptr, "reset publishes after the EOF pause");
  if (snapshot) {
    const int main_serial = snapshot->metadata.at(0).frame_serial;
    const int short_serial = snapshot->metadata.at(1).frame_serial;
    std::fprintf(stdout, "  after EOF+reset serials are %d/%d\n", main_serial, short_serial);
    Check(main_serial == 7 && short_serial == 7, "both sources stay at the last common frame (7/7), not 8/7");
    Check(FrameMatchesSerial(snapshot->frames.at(0).get(), main_serial, 0), "main frame content matches its serial");
    Check(FrameMatchesSerial(snapshot->frames.at(1).get(), short_serial, 10), "short frame content matches its serial");
  }
  player->Stop();
}

void test_first_control_cannot_publish_incomplete_set() {
  std::fprintf(stdout, "[first control cannot publish an incomplete set]\n");
  const auto dir = MakeTempDir();
  const auto a = dir / "a.y4m";
  const auto b = dir / "b.y4m";
  WriteY4M(a, 32, 0);
  WriteY4M(b, 32, 10);

  TestPlayer player;
  player.AddVideoSource(std::make_unique<VideoSource>(a.string(), "", 1, HardwareDecoder::None), 0);
  player.AddVideoSource(std::make_unique<VideoSource>(b.string(), "", 1, HardwareDecoder::None), 1);
  player.SetMainId(0);
  player.SetFlickerInterval(0);
  player.StartSources(-1.0f, 0);
  player.StartWithFirstControl(Cmd(ControlKind::StepNextSingle, 1));

  auto outcomes = WaitOutcomes(player, 1);
  Check(outcomes.size() == 1 && outcomes[0].status == ControlStatus::Ok, "first control completes");
  auto snapshot = WaitSnapshot(player, 1);
  Check(snapshot != nullptr, "first control publishes a snapshot");
  if (snapshot) {
    Check(snapshot->frames.size() == 2, "first published set contains every source");
    Check(snapshot->frames.count(0) == 1, "the main source is present in the first published set");
  }
  player.Stop();
}

void test_step_budget_is_bounded() {
  std::fprintf(stdout, "[step budget is bounded]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<TestPlayer>(dir, 1024, 1024);

  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 64; ++i) {
      player->StepNextFrame();
    }
    auto outcomes = WaitOutcomes(*player, 64);
    if (outcomes.size() != 64) {
      Check(false, "burst outcomes are all reported");
      player->Stop();
      return;
    }
  }
  const size_t pending = player->PendingCommandCount();
  std::fprintf(stdout, "  pending commands after 10x64 steps: %zu\n", pending);
  Check(pending <= 64, "accepted work stays within the single bounded queue");
  player->Stop();
}

void test_stop_resolves_every_request() {
  std::fprintf(stdout, "[stop resolves every request]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir);

  player->Stop();
  player->StepNextFrame();
  player->SeekTo(0.2f);
  player->Stop();
  auto outcomes = player->TakeControlOutcomes();
  Check(outcomes.size() == 2, "both post-stop requests are answered");
  if (outcomes.size() == 2) {
    Check(outcomes[0].status == ControlStatus::Cancelled && outcomes[1].status == ControlStatus::Cancelled,
          "post-stop requests are cancelled, not silently dropped");
  }
}

void test_video_source_stop_and_cancellable_waits() {
  std::fprintf(stdout, "[VideoSource stop and cancellable waits]\n");
  const auto dir = MakeTempDir();
  const auto a = dir / "a.y4m";
  WriteY4M(a, 12, 0);

  const auto race_deadline = Deadline(20000);
  int completed = 0;
  for (int i = 0; i < 40 && Clock::now() < race_deadline; ++i) {
    VideoSource source(a.string(), "", 1, HardwareDecoder::None);
    source.Start(0);
    source.Stop(); // must not hang on a thread that set its own active flag
    ++completed;
  }
  Check(completed == 40, "Start() immediately followed by Stop() completes 40/40 times");

  VideoSource source(a.string(), "", 1, HardwareDecoder::None);
  source.Start(0);
  source.WaitForFrameAvailable();
  source.GetNextFrame(2000);
  source.GetNextFrame(2000);
  source.Stop();

  bool prev_stopped = false;
  try {
    auto frame = source.GetPrevFrame(30000);
    std::fprintf(stderr, "  GetPrevFrame returned pts %lld after Stop()\n", (long long)frame->pts);
  } catch (const stopped_error &) {
    prev_stopped = true;
  } catch (const std::exception &) {
  }
  Check(prev_stopped, "GetPrevFrame observes Stop even on a cache hit");

  auto start = Clock::now();
  bool seek_stopped = false;
  try {
    source.SeekToFrameSerial(5, -1);
  } catch (const stopped_error &) {
    seek_stopped = true;
  } catch (const std::exception &) {
  }
  Check(seek_stopped, "seek after Stop() throws stopped_error");
  Check(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() < 2000,
        "seek after Stop() returns promptly");
}

void test_player_stop_is_bounded() {
  std::fprintf(stdout, "[player stop is bounded]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<SyncPlayer>(dir, 24, 24);

  for (int i = 0; i < 8; ++i) {
    player->StepNextFrame();
  }
  const auto start = Clock::now();
  player->Stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  Check(elapsed < 5000, "Stop() returns within its deadline after a command burst");
}

// R2-1: a slow UI consumer must never lose an accepted command's result. The
// submission backlog is rejected by the total budget instead of dropping the
// oldest outcome (the previous design silently lost 64 results).
void test_control_outcomes_are_never_dropped_on_overflow() {
  std::fprintf(stdout, "[control outcomes are never dropped on overflow]\n");
  const auto dir = MakeTempDir();
  auto player = MakePlayerImpl<TestPlayer>(dir, 4096, 4096);

  constexpr int kWaves = 10;
  constexpr int kPerWave = 32;
  std::vector<uint64_t> submitted;
  submitted.reserve(kWaves * kPerWave);
  for (int wave = 0; wave < kWaves; ++wave) {
    for (int i = 0; i < kPerWave; ++i) {
      submitted.push_back(player->StepNextFrame());
    }
    const auto deadline = Deadline(30000);
    while (player->PendingCommandCount() != 0 && Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Wait for every request id to be delivered exactly once; a missing id is a
  // hard timeout, not a sleep-based pass.
  std::map<uint64_t, int> counts;
  std::map<uint64_t, ControlStatus> statuses;
  const auto deadline = Deadline(30000);
  while (counts.size() < submitted.size() && Clock::now() < deadline) {
    for (auto &outcome : player->TakeControlOutcomes()) {
      counts[outcome.request_id]++;
      statuses[outcome.request_id] = outcome.status;
    }
    if (counts.size() < submitted.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  int accepted = 0;
  int rejected = 0;
  bool exactly_once = true;
  for (const uint64_t id : submitted) {
    if (counts[id] != 1) {
      exactly_once = false;
    }
    if (statuses[id] == ControlStatus::Rejected) {
      ++rejected;
    } else {
      ++accepted;
    }
  }
  std::fprintf(stdout, "  submitted=%zu resolved=%zu accepted=%d rejected=%d\n", submitted.size(), counts.size(),
               accepted, rejected);
  Check(exactly_once, "every submission is resolved exactly once across a slow UI consumer");
  Check(counts.size() == submitted.size(), "no request id is lost when the outcome backlog exceeds the budget");
  Check(rejected > 0, "the total budget rejects new work instead of dropping accepted results");
  Check(accepted > 0, "commands accepted before the budget filled still execute");
  player->Stop();
}

// R2-2: after a short source hits EOF mid multi-source fetch, the rolled-back
// source must keep a cursor that supports both a backward and a forward step.
void test_eof_rollback_keeps_bidirectional_cursor() {
  std::fprintf(stdout, "[EOF rollback keeps a bidirectional cursor]\n");
  const auto dir = MakeTempDir();
  const auto a = dir / "a.y4m";
  const auto b = dir / "b.y4m";
  WriteY4M(a, 250, 0);
  WriteY4M(b, 45, 10);

  TestPlayer player;
  player.AddVideoSource(std::make_unique<VideoSource>(a.string(), "", 1, HardwareDecoder::None), 0);
  player.AddVideoSource(std::make_unique<VideoSource>(b.string(), "", 1, HardwareDecoder::None), 1);
  player.SetMainId(0);
  player.SetFlickerInterval(0);
  player.StartSources(-1.0f, 0);
  player.Start(true);
  Check(WaitState(player, SyncPlayer::PlayState::kPaused) && WaitSnapshot(player, 1) != nullptr,
        "player reaches the first paused frame");

  auto seek = SubmitAndWait(player, [&player]() { return player.SeekTo(43 * 0.04f); });
  Check(seek.status == ControlStatus::Ok, "positioning seek completes");

  auto to44 = SubmitAndWait(player, [&player]() { return player.StepNextFrame(); });
  Check(to44.status == ControlStatus::Ok, "step to the last common frame (44/44) completes");
  auto at44 = WaitSerial(player, 0, 44);
  Check(at44 != nullptr && at44->metadata.at(1).frame_serial == 44, "both sources sit on the last common frame");
  if (at44) {
    Check(FrameMatchesSerial(at44->frames.at(0).get(), 44, 0) && FrameMatchesSerial(at44->frames.at(1).get(), 44, 10),
          "contents match the serials at the last common frame");
  }

  auto beyond = SubmitAndWait(player, [&player]() { return player.StepNextFrame(); });
  Check(beyond.status == ControlStatus::Boundary, "stepping past the short source reports a boundary");

  // The rollback used to leave the main source at "no frame consumed", which
  // made this backward step fail deterministically.
  auto back = SubmitAndWait(player, [&player]() { return player.StepPrevFrame(); });
  Check(back.status == ControlStatus::Ok, "step back after the EOF boundary completes");
  auto at43 = WaitSerial(player, 0, 43);
  Check(at43 != nullptr && at43->metadata.at(1).frame_serial == 43, "both sources return to serial 43");
  if (at43) {
    Check(FrameMatchesSerial(at43->frames.at(0).get(), 43, 0) && FrameMatchesSerial(at43->frames.at(1).get(), 43, 10),
          "contents are consistent after the EOF rollback");
  }

  auto forward = SubmitAndWait(player, [&player]() { return player.StepNextFrame(); });
  Check(forward.status == ControlStatus::Ok, "step forward after the rollback completes");
  auto again44 = WaitSerial(player, 0, 44);
  Check(again44 != nullptr && again44->metadata.at(1).frame_serial == 44,
        "both sources return to serial 44 after the round trip");
  if (again44) {
    Check(FrameMatchesSerial(again44->frames.at(0).get(), 44, 0),
          "forward content matches serial 44 after the round trip");
  }
  player.Stop();
}

// R2-3: Stop() marks every source dead; a later Start() must preserve terminal
// results the UI has not consumed and bring the decode pipeline back up instead
// of spinning silently on a stopped source.
void test_start_preserves_results_and_restarts_pipeline() {
  std::fprintf(stdout, "[Start preserves unconsumed results and restarts the pipeline]\n");
  const auto dir = MakeTempDir();
  const auto a = dir / "a.y4m";
  const auto b = dir / "b.y4m";
  WriteY4M(a, 64, 0);
  WriteY4M(b, 64, 10);

  TestPlayer player;
  player.AddVideoSource(std::make_unique<VideoSource>(a.string(), "", 1, HardwareDecoder::None), 0);
  player.AddVideoSource(std::make_unique<VideoSource>(b.string(), "", 1, HardwareDecoder::None), 1);
  player.SetMainId(0);
  player.SetFlickerInterval(0);
  player.StartSources(-1.0f, 0);
  player.Start(true);
  auto first = WaitSnapshot(player, 1);
  Check(first != nullptr && WaitState(player, SyncPlayer::PlayState::kPaused),
        "first session reaches the first paused frame");
  const uint64_t first_generation = first ? first->generation : 0;

  player.Stop();
  // The stopped player answers synchronously with a terminal Cancelled result,
  // which the UI deliberately leaves unconsumed across the restart.
  const uint64_t late_id = player.StepNextFrame();
  player.Start(false);
  auto outcomes = player.TakeControlOutcomes();
  bool late_found = false;
  for (const auto &outcome : outcomes) {
    if (outcome.request_id == late_id) {
      late_found = true;
      Check(outcome.status == ControlStatus::Cancelled, "the preserved result is the terminal Cancelled outcome");
    }
  }
  Check(late_found, "a terminal result delivered before Start() survives the restart");

  auto restarted = WaitSnapshot(player, first_generation + 1);
  Check(restarted != nullptr, "the restarted pipeline publishes a frame");
  if (restarted) {
    const int serial = restarted->metadata.at(0).frame_serial;
    Check(FrameMatchesSerial(restarted->frames.at(0).get(), serial, 0), "restarted frame content matches its serial");
  }
  auto step = SubmitAndWait(player, [&player]() { return player.StepNextFrame(); });
  Check(step.status == ControlStatus::Ok, "a control command completes after the restart");
  player.Stop();
}

} // namespace

// Interposed so a test can make exactly one av_seek_frame fail with AVERROR(EIO)
// and verify that the failure is propagated as a failed seek acknowledgement.
// All other calls are forwarded to the real FFmpeg implementation.
static std::atomic<bool> g_fail_next_seek{false};

extern "C" int av_seek_frame(AVFormatContext *ctx, int stream_index, int64_t timestamp, int flags) {
  if (g_fail_next_seek.exchange(false)) {
    return AVERROR(EIO);
  }
  using Fn = int (*)(AVFormatContext *, int, int64_t, int);
  static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_seek_frame"));
  if (!real) {
    std::abort();
  }
  return real(ctx, stream_index, timestamp, flags);
}

namespace {

void test_seek_failure_propagates() {
  std::fprintf(stdout, "[seek failure propagates]\n");
  const auto dir = MakeTempDir();
  const auto a = dir / "a.y4m";
  WriteY4M(a, 32, 0);

  VideoSource source(a.string(), "", 1, HardwareDecoder::None);
  source.Start(0);
  source.WaitForFrameAvailable();
  source.GetNextFrame(2000);

  g_fail_next_seek = true;
  bool failed = false;
  try {
    source.SeekToFrameSerial(10, 2000);
  } catch (const std::exception &e) {
    std::fprintf(stdout, "  seek failure surfaced: %s\n", e.what());
    failed = true;
  }
  Check(failed, "av_seek_frame failure propagates instead of a successful acknowledgement");
  source.Stop();
}

void test_start_seek_positions_at_startup() {
  std::fprintf(stdout, "[startup --seek-to]\n");
  const auto dir = MakeTempDir();
  const auto a = dir / "a.y4m";
  WriteY4M(a, 32, 0);

  SyncPlayer player;
  player.AddVideoSource(std::make_unique<VideoSource>(a.string(), "", 1, HardwareDecoder::None), 0);
  player.SetFlickerInterval(0);
  bool started = false;
  try {
    player.StartSources(0.2f, 0);
    started = true;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "  StartSources(0.2) threw: %s\n", e.what());
  }
  Check(started, "--seek-to startup does not time out");
  if (started) {
    player.Start(true);
    auto snapshot = WaitSnapshot(player, 1);
    if (snapshot) {
      std::fprintf(stdout, "  startup seek reached serial %d\n", snapshot->metadata.at(0).frame_serial);
    }
    Check(snapshot != nullptr && snapshot->metadata.at(0).frame_serial == 5,
          "--seek-to startup reaches the requested frame (serial 5)");
  }
  player.Stop();
}

} // namespace

int main() {
  set_log_level(LoggingLevel::WARN);
  try {
    test_first_paused_publication_and_snapshot_lifetime();
    test_global_step_order();
    test_single_video_step_isolation();
    test_single_video_seek_offset_and_reset();
    test_quick_consecutive_commands();
    test_step_then_reset_is_fifo();
    test_relative_seeks_accumulate();
    test_seek_then_reset_binds_metadata();
    test_short_source_eof_keeps_both_sources_aligned();
    test_first_control_cannot_publish_incomplete_set();
    test_step_budget_is_bounded();
    test_stop_resolves_every_request();
    test_video_source_stop_and_cancellable_waits();
    test_player_stop_is_bounded();
    test_control_outcomes_are_never_dropped_on_overflow();
    test_eof_rollback_keeps_bidirectional_cursor();
    test_start_preserves_results_and_restarts_pipeline();
    test_seek_failure_propagates();
    test_start_seek_positions_at_startup();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "UNCAUGHT EXCEPTION: %s\n", e.what());
    return 1;
  }

  std::fprintf(stdout, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
