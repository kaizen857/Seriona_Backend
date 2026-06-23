#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint16_t kChannels = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct LifecycleRecorder {
  std::vector<std::string> calls{};
  int initializeCalls{0};
  int startCalls{0};
  int stopCalls{0};
  int uninitializeCalls{0};
  bool queueAttachedOnInitialize{false};
  bool callbackUserDataAttachedOnInitialize{false};
  bool stopBeforeUninitialize{false};
  bool backendDestroyed{false};
  bool backendDestroyedAfterUninitialize{false};
};

void writeU16(std::ofstream& stream, std::uint16_t value) {
  const auto bytes = std::array<unsigned char, 2>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ofstream& stream, std::uint32_t value) {
  const auto bytes = std::array<unsigned char, 4>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
      static_cast<unsigned char>((value >> 16U) & 0xFFU),
      static_cast<unsigned char>((value >> 24U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeTag(std::ofstream& stream, const char tag[4]) { stream.write(tag, 4); }

std::vector<std::int16_t> makeSine(std::uint32_t frames) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    samples.push_back(static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0)));
  }

  return samples;
}

void writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples) {
  std::filesystem::create_directories(path.parent_path());
  const auto dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());

  writeTag(output, "RIFF");
  writeU32(output, 36U + dataSize);
  writeTag(output, "WAVE");
  writeTag(output, "fmt ");
  writeU32(output, 16U);
  writeU16(output, 1U);
  writeU16(output, kChannels);
  writeU32(output, kSampleRate);
  writeU32(output, kSampleRate * kChannels * (kBitsPerSample / 8U));
  writeU16(output, static_cast<std::uint16_t>(kChannels * (kBitsPerSample / 8U)));
  writeU16(output, kBitsPerSample);
  writeTag(output, "data");
  writeU32(output, dataSize);

  for (const auto sample : samples) {
    writeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
}

std::filesystem::path sineFixture(std::string name, std::uint32_t frames) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  writeWav(path, makeSine(frames));
  return path;
}

AudioOutputConfig outputConfig() {
  AudioOutputConfig config{};
  config.outputMode = AudioOutputMode::Mixed;
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 20ms;
  return config;
}

TrackPlaybackRequest request(const std::filesystem::path& path, std::string trackId) {
  return TrackPlaybackRequest{.trackId = std::move(trackId),
                              .filePath = path,
                              .title = "Generated Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

std::size_t bytesPerSample(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2U;
  case AudioSampleFormat::Int24:
    return 3U;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4U;
  case AudioSampleFormat::Unknown:
    return 0U;
  }

  return 0U;
}

class LifecycleAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  explicit LifecycleAudioOutputDeviceBackend(std::shared_ptr<LifecycleRecorder> recorder) : recorder_(std::move(recorder)) {}

  ~LifecycleAudioOutputDeviceBackend() override {
    recorder_->calls.push_back("backend-destroy");
    recorder_->backendDestroyed = true;
    recorder_->backendDestroyedAfterUninitialize = recorder_->uninitializeCalls > 0;
  }

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    recorder_->calls.push_back("initialize");
    ++recorder_->initializeCalls;
    recorder_->queueAttachedOnInitialize = request.pcmQueue != nullptr;
    recorder_->callbackUserDataAttachedOnInitialize = request.callbackUserData != nullptr;
    queue = request.pcmQueue;
    userData = request.callbackUserData;
    format.deviceId = request.config.preferredDeviceId.empty() ? "fake-device" : request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    format.actualMode = request.config.outputMode;
    initialized = true;
    return true;
  }

  [[nodiscard]] bool start() override {
    recorder_->calls.push_back("start");
    ++recorder_->startCalls;
    started = initialized;
    return initialized;
  }

  [[nodiscard]] bool stop() override {
    recorder_->calls.push_back("stop");
    ++recorder_->stopCalls;
    started = false;
    return true;
  }

  void uninitialize() noexcept override {
    recorder_->calls.push_back("uninitialize");
    ++recorder_->uninitializeCalls;
    recorder_->stopBeforeUninitialize = recorder_->stopCalls > 0;
    initialized = false;
    started = false;
    queue = nullptr;
    userData = nullptr;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  void consumeFrames(std::uint32_t frames) {
    REQUIRE(userData != nullptr);
    const auto bytesPerFrame = static_cast<std::size_t>(format.channelCount) * bytesPerSample(format.sampleFormat);
    callbackBuffer.assign(static_cast<std::size_t>(frames) * bytesPerFrame, 0U);
    AudioOutputDevice::renderCallback(userData, callbackBuffer.data(), frames);
  }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = kSampleRate,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 2,
                           .bufferFrames = 512,
                           .actualMode = AudioOutputMode::Mixed};
  PcmBufferQueue* queue{nullptr};
  AudioOutputDevice* userData{nullptr};
  std::vector<std::uint8_t> callbackBuffer{};
  bool initialized{false};
  bool started{false};

private:
  std::shared_ptr<LifecycleRecorder> recorder_;
};

}

TEST_CASE("audio_shutdown_lifecycle stops device before service destroys queue") {
  const auto recorder = std::make_shared<LifecycleRecorder>();
  {
    auto backend = std::make_unique<LifecycleAudioOutputDeviceBackend>(recorder);
    AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
    player.configureOutput(outputConfig());
    player.loadTrack(request(sineFixture("audio_shutdown_lifecycle_stop_before_destroy.wav", kSampleRate), "shutdown"));
    player.play();

    CHECK(recorder->initializeCalls == 1);
    CHECK(recorder->startCalls == 1);
    CHECK(recorder->queueAttachedOnInitialize);
    CHECK(recorder->callbackUserDataAttachedOnInitialize);
  }

  CHECK(recorder->stopCalls == 1);
  CHECK(recorder->uninitializeCalls == 1);
  CHECK(recorder->stopBeforeUninitialize);
  CHECK(recorder->backendDestroyed);
  CHECK(recorder->backendDestroyedAfterUninitialize);
  CHECK(recorder->calls == std::vector<std::string>{"initialize", "start", "stop", "uninitialize", "backend-destroy"});
}

TEST_CASE("audio_shutdown_lifecycle clear sink blocks events through stop and destruction") {
  const auto recorder = std::make_shared<LifecycleRecorder>();
  auto backend = std::make_unique<LifecycleAudioOutputDeviceBackend>(recorder);
  auto* fake = backend.get();
  std::vector<BackendEvent> events;

  {
    AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
    player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
    player.configureOutput(outputConfig());
    player.loadTrack(request(sineFixture("audio_shutdown_lifecycle_sink_clear.wav", 480U), "sink-clear"));
    const auto deliveredBeforeClear = events.size();
    REQUIRE(deliveredBeforeClear > 0U);

    player.setEventSink(BackendEventSink{});
    player.play();
    fake->consumeFrames(960U);
    static_cast<void>(player.queryPlaybackClock());
    player.stop();

    CHECK(events.size() == deliveredBeforeClear);
  }

  CHECK(recorder->stopCalls >= 1);
  CHECK(recorder->uninitializeCalls == 1);
  CHECK(recorder->backendDestroyed);
}

TEST_CASE("audio_shutdown_lifecycle waits for progress worker before clearing sinks") {
  const auto recorder = std::make_shared<LifecycleRecorder>();
  auto backend = std::make_unique<LifecycleAudioOutputDeviceBackend>(recorder);
  auto* fake = backend.get();

  std::mutex mutex;
  std::condition_variable sinkEntered;
  std::condition_variable releaseSink;
  bool progressSinkEntered = false;
  bool allowSinkReturn = false;
  bool destructorReturned = false;
  std::atomic<bool> armProgressSink{false};

  auto player = std::make_unique<AudioPlayer>(makeAudioPlaybackService(std::move(backend)));
  player->setEventSink([&](BackendEvent event) {
    if (!armProgressSink.load(std::memory_order_acquire) || event.type != BackendEventType::PlaybackPositionUpdated) {
      return;
    }

    std::unique_lock lock{mutex};
    progressSinkEntered = true;
    sinkEntered.notify_one();
    releaseSink.wait(lock, [&] { return allowSinkReturn; });
  });
  player->configureOutput(outputConfig());
  player->loadTrack(request(sineFixture("audio_shutdown_lifecycle_progress_before_sink_clear.wav", kSampleRate), "progress-clear"));
  player->play();
  armProgressSink.store(true, std::memory_order_release);
  fake->consumeFrames(1024U);

  {
    std::unique_lock lock{mutex};
    REQUIRE(sinkEntered.wait_for(lock, 2s, [&] { return progressSinkEntered; }));
  }

  std::thread destroyer{[&] {
    player.reset();
    std::lock_guard lock{mutex};
    destructorReturned = true;
  }};

  {
    std::unique_lock lock{mutex};
    CHECK_FALSE(releaseSink.wait_for(lock, 25ms, [&] { return destructorReturned; }));
    allowSinkReturn = true;
  }
  releaseSink.notify_one();
  destroyer.join();

  CHECK(destructorReturned);
  CHECK(recorder->stopCalls == 1);
  CHECK(recorder->uninitializeCalls == 1);
  CHECK(recorder->backendDestroyed);
}

}
