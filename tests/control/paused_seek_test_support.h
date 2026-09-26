#pragma once

#include "control_test_harness.h"
#include "seriona/audio/audio_playback_service.h"
#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace seriona::control::test {
using namespace std::chrono_literals;

class SeekGate {
public:
  void arm() { std::lock_guard lock(mutex_); armed_ = true; entered_ = false; }
  void enter() {
    std::unique_lock lock(mutex_);
    if (!armed_) return;
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return !armed_; });
  }
  bool wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 30s, [&] { return entered_; });
  }
  void release() { std::lock_guard lock(mutex_); armed_ = false; changed_.notify_all(); }
private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_{false};
  bool entered_{false};
};

class ManualSeekDevice final : public audio::AudioOutputDeviceBackend {
public:
  ~ManualSeekDevice() override { if (onDestroy) onDestroy(); }
  std::function<void()> onDestroy;
  std::vector<audio::AudioDeviceFormat> enumeratePlaybackDevices() override { return {format_}; }
  bool initialize(const audio::AudioOutputDeviceOpenRequest& request) override {
    std::lock_guard lock(mutex_);
    data_ = request.callbackUserData;
    format_.sampleRate = request.sampleRate;
    format_.sampleFormat = request.sampleFormat;
    format_.channelCount = request.channelCount;
    format_.bufferFrames = request.bufferFrames;
    format_.actualMode = request.config.outputMode;
    return true;
  }
  bool start() override { std::lock_guard lock(mutex_); active_ = true; return true; }
  bool stop() override { std::lock_guard lock(mutex_); active_ = false; return true; }
  void uninitialize() noexcept override { std::lock_guard lock(mutex_); active_ = false; data_ = nullptr; }
  audio::AudioDeviceFormat currentFormat() const override { std::lock_guard lock(mutex_); return format_; }
  bool active() const { std::lock_guard lock(mutex_); return active_; }
  std::array<float, 480> consume() {
    std::lock_guard lock(mutex_);
    if (!active_ || !data_) throw std::runtime_error("manual device is not playing");
    std::array<float, 480> samples{};
    audio::AudioOutputDevice::renderCallback(data_, samples.data(), samples.size());
    return samples;
  }
private:
  mutable std::mutex mutex_;
  audio::AudioOutputDevice* data_{};
  audio::AudioDeviceFormat format_{};
  bool active_{false};
};

class ObservedSeekAudio final : public audio::AudioPlaybackService {
public:
  explicit ObservedSeekAudio(std::shared_ptr<audio::AudioPlaybackService> service) : inner(std::move(service)) {}
  ~ObservedSeekAudio() override { shutdown(); }
  void shutdown() {
    gate.release();
    if (inner) {
      inner->setEventSink({});
      inner.reset();
    }
  }
  void setEventSink(audio::BackendEventSink sink) override {
    if (!inner) return;
    if (!sink) {
      inner->setEventSink({});
      return;
    }
    inner->setEventSink([this, sink](audio::BackendEvent event) {
      if (const auto* value = std::get_if<audio::PlaybackStateChanged>(&event.payload)) {
        state = value->state;
        if (value->state == audio::PlaybackState::Loading) gate.enter();
      }
      if (std::holds_alternative<audio::PlaybackError>(event.payload)) ++errors;
      if (sink) sink(std::move(event));
    });
  }
  void configureOutput(const audio::AudioOutputConfig& value) override { inner->configureOutput(value); }
  void configureTransition(const audio::TransitionConfig& value) override { inner->configureTransition(value); }
  void loadTrack(const audio::TrackPlaybackRequest& value) override { ++loads; inner->loadTrack(value); }
  void prepareNext(const audio::TrackPlaybackRequest& value) override { inner->prepareNext(value); }
  void play() override { ++plays; inner->play(); }
  void pause() override { inner->pause(); }
  void resume() override { ++resumes; inner->resume(); }
  void stop() override { inner->stop(); }
  void seek(std::chrono::milliseconds value) override { inner->seek(value); }
  void setVolume(float value) override { inner->setVolume(value); }
  void setMuted(bool value) override { inner->setMuted(value); }
  void selectOutputDevice(const std::string& value) override { inner->selectOutputDevice(value); }
  audio::PlaybackClockSnapshot queryPlaybackClock() const override { return inner->queryPlaybackClock(); }
  std::shared_ptr<audio::AudioPlaybackService> inner;
  SeekGate gate;
  std::atomic<int> loads{0}, plays{0}, resumes{0}, errors{0};
  std::atomic<audio::PlaybackState> state{audio::PlaybackState::Idle};
};

class SeekPcmFixture {
public:
  SeekPcmFixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      root_ = std::filesystem::temp_directory_path() / ("seriona-paused-seek-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(root_)) break;
      if (attempt == 99) throw std::runtime_error("could not reserve PCM fixture directory");
    }
    std::ofstream out(path(), std::ios::binary);
    auto word = [&](std::uint32_t value, unsigned bytes) {
      for (unsigned index = 0; index < bytes; ++index) out.put(static_cast<char>((value >> (8 * index)) & 255));
    };
    constexpr std::uint32_t frames = 12 * 48000;
    out.write("RIFF", 4); word(36 + frames * 2, 4); out.write("WAVEfmt ", 8);
    word(16, 4); word(1, 2); word(1, 2); word(48000, 4); word(96000, 4);
    word(2, 2); word(16, 2); out.write("data", 4); word(frames * 2, 4);
    for (std::uint32_t frame = 0; frame < frames; ++frame) word(frame < 3 * 48000 ? 4096 : 16384, 2);
    out.close();
    if (!out) {
      std::filesystem::remove_all(root_);
      throw std::runtime_error("could not write PCM fixture");
    }
  }
  ~SeekPcmFixture() { std::error_code error; std::filesystem::remove_all(root_, error); }
  std::filesystem::path path() const { return root_ / "controlled.wav"; }
private:
  std::filesystem::path root_;
};

class PausedSeekHarness {
public:
  explicit PausedSeekHarness(std::chrono::milliseconds offset = 0ms,
                             std::function<void(PausedSeekHarness&)> beforeReady = {}) {
    try {
      initialize(offset, beforeReady);
    } catch (...) {
      shutdown();
      throw;
    }
  }
private:
  void initialize(std::chrono::milliseconds offset,
                  const std::function<void(PausedSeekHarness&)>& beforeReady) {
    auto backend = std::make_unique<ManualSeekDevice>();
    device = backend.get();
    audio = std::make_shared<ObservedSeekAudio>(audio::makeAudioPlaybackService(std::move(backend)));
    MediaControllerDependencies dependencies;
    dependencies.audio = audio;
    dependencies.scanner = std::make_shared<FakeFileScannerService>();
    dependencies.metadata = std::make_unique<FakeMetadataSharingService>();
    controller = std::make_unique<MediaController>(std::move(dependencies));
    controller->start();
    output.targetSampleRate = 48000;
    output.targetSampleFormat = audio::AudioSampleFormat::Float32;
    output.targetChannelCount = 1;
    output.bufferDuration = 150ms;
    audio->configureOutput(output);
    scanner::SongMetadata song;
    song.trackId = "probe"; song.filePath = fixture.path(); song.duration = 12s;
    if (offset != 0ms) {
      song.sourceFilePath = song.filePath;
      song.filePath = fixture.path().parent_path() / "virtual.cue";
      song.offset = offset; song.duration = 8s;
    }
    scanner::PlaylistNode root;
    root.nodeId = "root"; root.kind = scanner::PlaylistNodeKind::Root; root.childNodeIds = {"song"};
    scanner::PlaylistNode node;
    node.nodeId = "song"; node.parentNodeId = "root"; node.kind = scanner::PlaylistNodeKind::Track; node.song = song;
    scanner::PlaylistTreeSnapshot tree;
    tree.version = 1; tree.rootNodeId = "root"; tree.nodes = {root, node};
    scanner::ScannerEvent event;
    event.type = scanner::ScannerEventType::PlaylistSnapshotUpdated;
    event.monotonicVersion = 1; event.payload = tree;
    controller->scannerEventSink()(std::move(event));
    command(MediaControlCommandKind::Play);
    settle();
    device->consume();
    command(MediaControlCommandKind::Pause);
    settle();
    if (beforeReady) beforeReady(*this);
  }
public:
  ~PausedSeekHarness() { shutdown(); }
  void shutdown() {
    if (audio) audio->gate.release();
    if (controller) controller->shutdown();
    if (audio) audio->shutdown();
    controller.reset();
    audio.reset();
  }
  void command(MediaControlCommandKind kind, std::optional<std::chrono::milliseconds> position = {}) {
    MediaControlCommand value;
    value.kind = kind; value.position = position;
    REQUIRE(controller->submitCommand(value).accepted);
  }
  audio::PlaybackClockSnapshot settle() {
    const auto issued = std::chrono::steady_clock::now();
    auto clock = audio->queryPlaybackClock();
    const auto deadline = issued + 30s;
    while (clock.sampledAt < issued && std::chrono::steady_clock::now() < deadline) clock = audio->queryPlaybackClock();
    REQUIRE(clock.sampledAt >= issued);
    MediaControlCommand fence;
    fence.kind = MediaControlCommandKind::SetMuted; fence.muted = false;
    REQUIRE(controller->submitCommand(fence).accepted);
    return clock;
  }
  void expectPcm(float expected) {
    REQUIRE(device->active());
    const auto samples = device->consume();
    CHECK(samples.front() == doctest::Approx(expected));
    CHECK(std::all_of(samples.begin(), samples.end(), [expected](float value) { return value == doctest::Approx(expected); }));
  }
  SeekPcmFixture fixture;
  ManualSeekDevice* device{};
  std::shared_ptr<ObservedSeekAudio> audio;
  std::unique_ptr<MediaController> controller;
  audio::AudioOutputConfig output;
};

}
