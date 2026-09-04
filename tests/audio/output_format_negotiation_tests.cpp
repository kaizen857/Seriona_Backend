#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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

struct SupportedFormat {
  AudioOutputMode mode{AudioOutputMode::Mixed};
  std::uint32_t sampleRate{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::uint16_t channelCount{0};
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
  const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));

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

std::filesystem::path sineFixture(std::string name) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  writeWav(path, makeSine(kSampleRate / 10U));
  return path;
}

class MatrixAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  explicit MatrixAudioOutputDeviceBackend(std::vector<SupportedFormat> supportedFormats)
      : supportedFormats_(std::move(supportedFormats)) {}

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {currentFormat_}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    requests.push_back(request);
    const auto supported = std::find_if(supportedFormats_.begin(),
                                        supportedFormats_.end(),
                                        [&request](const SupportedFormat& format) {
                                          return format.mode == request.config.outputMode &&
                                                 format.sampleRate == request.sampleRate &&
                                                 format.sampleFormat == request.sampleFormat &&
                                                 format.channelCount == request.channelCount;
                                        });
    if (supported == supportedFormats_.end()) {
      return false;
    }

    currentFormat_ = AudioDeviceFormat{.deviceId = request.config.preferredDeviceId.empty() ? "matrix-device" : request.config.preferredDeviceId,
                                       .deviceName = "Matrix Device",
                                       .backendName = "fake-matrix",
                                       .sampleRate = request.sampleRate,
                                       .sampleFormat = request.sampleFormat,
                                       .channelCount = request.channelCount,
                                       .bufferFrames = request.bufferFrames,
                                       .actualMode = request.config.outputMode,
                                       .fallbackApplied = false};
    initialized = true;
    return true;
  }

  [[nodiscard]] bool start() override { return initialized; }
  [[nodiscard]] bool stop() override { return true; }

  void uninitialize() noexcept override { initialized = false; }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return currentFormat_; }

  std::vector<AudioOutputDeviceOpenRequest> requests{};
  bool initialized{false};

private:
  std::vector<SupportedFormat> supportedFormats_;
  AudioDeviceFormat currentFormat_{.deviceId = "matrix-device",
                                   .deviceName = "Matrix Device",
                                   .backendName = "fake-matrix",
                                   .actualMode = AudioOutputMode::Mixed};
};

struct LoadResult {
  std::vector<BackendEvent> events{};
  std::vector<AudioOutputDeviceOpenRequest> requests{};
};

AudioOutputConfig config(AudioOutputMode mode,
                         std::uint32_t sampleRate,
                         AudioSampleFormat sampleFormat,
                         std::uint16_t channelCount) {
  AudioOutputConfig outputConfig{};
  outputConfig.outputMode = mode;
  outputConfig.targetSampleRate = sampleRate;
  outputConfig.targetSampleFormat = sampleFormat;
  outputConfig.targetChannelCount = channelCount;
  outputConfig.bufferDuration = 40ms;
  outputConfig.preferredDeviceId = "matrix-device";
  return outputConfig;
}

TrackPlaybackRequest request(const std::filesystem::path& path, std::string trackId) {
  return TrackPlaybackRequest{.trackId = std::move(trackId),
                              .filePath = path,
                              .title = "Matrix Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

LoadResult loadWith(std::vector<SupportedFormat> supportedFormats,
                    const AudioOutputConfig& outputConfig,
                    const std::filesystem::path& path,
                    std::string trackId) {
  auto backend = std::make_unique<MatrixAudioOutputDeviceBackend>(std::move(supportedFormats));
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig);
  player.loadTrack(request(path, std::move(trackId)));
  static_cast<void>(player.queryPlaybackClock());
  return LoadResult{std::move(events), fake->requests};
}

std::vector<BackendEvent> eventsOf(const std::vector<BackendEvent>& events, BackendEventType type) {
  std::vector<BackendEvent> filtered;
  for (const auto& event : events) {
    if (event.type == type) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

const BackendEvent& firstEventOf(const std::vector<BackendEvent>& events, BackendEventType type) {
  const auto iterator = std::find_if(events.begin(), events.end(), [type](const BackendEvent& event) { return event.type == type; });
  REQUIRE(iterator != events.end());
  return *iterator;
}

// —— T7 免重开（mixed_output_noreopen）测试设施 ——
// 接受一切格式、仅计数与记录打开请求的后端：initialize/uninitialize 计数用于断言
// "同参数切歌设备 0 次重开"，requests 记录每次协商尝试的设备目标。渲染回调经
// renderFrames 驱动，供"原地换队列后播的是新曲内容"的字节级验证。
class CountingAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {currentFormat_}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    requests.push_back(request);
    userData = request.callbackUserData;
    currentFormat_ = AudioDeviceFormat{.deviceId = request.config.preferredDeviceId.empty() ? "counting-device"
                                                                                            : request.config.preferredDeviceId,
                                       .deviceName = "Counting Device",
                                       .backendName = "fake-counting",
                                       .sampleRate = request.sampleRate,
                                       .sampleFormat = request.sampleFormat,
                                       .channelCount = request.channelCount,
                                       .bufferFrames = request.bufferFrames,
                                       .actualMode = request.config.outputMode,
                                       .fallbackApplied = false};
    return true;
  }

  [[nodiscard]] bool start() override { return true; }
  [[nodiscard]] bool stop() override { return true; }

  void uninitialize() noexcept override { ++uninitializeCalls; }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return currentFormat_; }

  // 以设备当前格式驱动一次渲染回调（音量 1.0、未静音），返回该块渲染字节。
  std::vector<std::uint8_t> renderFrames(std::uint32_t frames) {
    REQUIRE(userData != nullptr);
    const auto bytesPerSample = currentFormat_.sampleFormat == AudioSampleFormat::Int16   ? 2U
                                : currentFormat_.sampleFormat == AudioSampleFormat::Int24 ? 3U
                                                                                          : 4U;
    std::vector<std::uint8_t> rendered(static_cast<std::size_t>(frames) * currentFormat_.channelCount * bytesPerSample,
                                       0U);
    AudioOutputDevice::renderCallback(userData, rendered.data(), frames);
    return rendered;
  }

  AudioOutputDevice* userData{nullptr};
  std::vector<AudioOutputDeviceOpenRequest> requests{};
  int initializeCalls{0};
  int uninitializeCalls{0};

private:
  AudioDeviceFormat currentFormat_{.deviceId = "counting-device",
                                   .deviceName = "Counting Device",
                                   .backendName = "fake-counting",
                                   .actualMode = AudioOutputMode::Mixed};
};

// 事件日志：sink 在 audio worker 线程执行，测试线程经 mutex 读快照（快照的互斥
// 同步同时为后端计数器的跨线程可见性提供 happens-before）。
class MutexEventLog {
public:
  BackendEventSink sink() {
    return [this](BackendEvent event) {
      std::lock_guard lock{mutex_};
      events_.push_back(std::move(event));
    };
  }

  [[nodiscard]] std::vector<BackendEvent> snapshot() const {
    std::lock_guard lock{mutex_};
    return events_;
  }

private:
  mutable std::mutex mutex_{};
  std::vector<BackendEvent> events_{};
};

bool hasEvent(const std::vector<BackendEvent>& events, BackendEventType type) {
  return std::any_of(events.begin(), events.end(), [type](const BackendEvent& event) { return event.type == type; });
}

// 目标曲目的 TrackChanged 出现后没有更新的其它曲目 TrackChanged，且其后已到达
// state（Ready/Playing 等）——用于在连续切歌序列中精确等待某一曲的加载/播放完成。
bool trackReachedState(const std::vector<BackendEvent>& events, const std::string& trackId, PlaybackState state) {
  std::optional<std::size_t> lastTrackIndex;
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (events[index].type != BackendEventType::TrackChanged) {
      continue;
    }
    const auto& changed = std::get<TrackChanged>(events[index].payload);
    lastTrackIndex = changed.request.trackId == trackId ? std::optional<std::size_t>{index} : std::nullopt;
  }
  if (!lastTrackIndex.has_value()) {
    return false;
  }
  for (std::size_t index = *lastTrackIndex; index < events.size(); ++index) {
    if (events[index].type == BackendEventType::PlaybackStateChanged &&
        std::get<PlaybackStateChanged>(events[index].payload).state == state) {
      return true;
    }
  }
  return false;
}

bool waitUntil(const MutexEventLog& log,
               std::function<bool(const std::vector<BackendEvent>&)> predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate(log.snapshot())) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate(log.snapshot());
}

}

TEST_CASE("output_format_negotiation covers direct, fallback, and mix downgrade") {
  const auto path = sineFixture("output_format_negotiation.wav");

  const auto directResult = loadWith({{AudioOutputMode::Direct, kSampleRate, AudioSampleFormat::Int16, kChannels}},
                                     config(AudioOutputMode::Direct, 48000, AudioSampleFormat::Float32, 2),
                                     path,
                                     "direct-success");
  CHECK(directResult.requests.size() == 1U);
  CHECK(eventsOf(directResult.events, BackendEventType::OutputModeFallback).empty());
  const auto& directFormat = std::get<OutputFormatChanged>(firstEventOf(directResult.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(directFormat.actualMode == AudioOutputMode::Direct);
  CHECK(directFormat.sampleRate == kSampleRate);
  CHECK(directFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(directFormat.channelCount == kChannels);
  CHECK_FALSE(directFormat.fallbackApplied);

  const auto fallbackResult = loadWith({{AudioOutputMode::Mixed, 48000, AudioSampleFormat::Float32, 2}},
                                       config(AudioOutputMode::Direct, 48000, AudioSampleFormat::Float32, 2),
                                       path,
                                       "direct-fallback");
  CHECK(fallbackResult.requests.size() == 2U);
  const auto& fallback = std::get<OutputModeFallback>(firstEventOf(fallbackResult.events, BackendEventType::OutputModeFallback).payload);
  CHECK(fallback.requestedConfig.outputMode == AudioOutputMode::Direct);
  CHECK(fallback.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(fallback.effectiveFormat.actualMode == AudioOutputMode::Mixed);
  CHECK(fallback.effectiveFormat.sampleFormat == AudioSampleFormat::Float32);
  CHECK(fallback.effectiveFormat.fallbackApplied);
  CHECK(fallback.reason.find("direct") != std::string::npos);
  const auto& fallbackFormat = std::get<OutputFormatChanged>(firstEventOf(fallbackResult.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(fallbackFormat.actualMode == AudioOutputMode::Mixed);
  CHECK(fallbackFormat.fallbackApplied);

  const auto downgradeResult = loadWith({{AudioOutputMode::Mixed, 48000, AudioSampleFormat::Int16, 1}},
                                        config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2),
                                        path,
                                        "mix-downgrade");
  CHECK(downgradeResult.requests.size() == 3U);
  const auto& downgrade = std::get<OutputModeFallback>(firstEventOf(downgradeResult.events, BackendEventType::OutputModeFallback).payload);
  CHECK(downgrade.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(downgrade.effectiveFormat.sampleRate == 48000U);
  CHECK(downgrade.effectiveFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(downgrade.effectiveFormat.channelCount == 1U);
  CHECK(downgrade.reason.find("source format") != std::string::npos);
}

TEST_CASE("output_format_negotiation direct mode initializes device with track parameters") {
  const auto path = sineFixture("output_format_negotiation_direct_track_params.wav");

  const auto result = loadWith({{AudioOutputMode::Direct, kSampleRate, AudioSampleFormat::Int16, kChannels},
                                {AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2}},
                               config(AudioOutputMode::Direct, 96000, AudioSampleFormat::Float32, 2),
                               path,
                               "direct-track-params");
  CHECK(result.requests.size() == 1U);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  const auto& format = std::get<OutputFormatChanged>(firstEventOf(result.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(format.actualMode == AudioOutputMode::Direct);
  CHECK(format.sampleRate == kSampleRate);
  CHECK(format.sampleFormat == AudioSampleFormat::Int16);
  CHECK(format.channelCount == kChannels);
  CHECK_FALSE(format.fallbackApplied);
}

TEST_CASE("output_format_negotiation_failure emits playback error only after all candidates fail") {
  const auto path = sineFixture("output_format_negotiation_failure.wav");
  const auto result = loadWith({},
                               config(AudioOutputMode::Direct, 96000, AudioSampleFormat::Float32, 2),
                               path,
                               "total-failure");

  CHECK(result.requests.size() == 4U);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  CHECK(eventsOf(result.events, BackendEventType::OutputFormatChanged).empty());
  const auto& error = std::get<PlaybackError>(firstEventOf(result.events, BackendEventType::PlaybackError).payload);
  CHECK(error.code == PlaybackErrorCode::FormatNegotiationFailed);
  CHECK(error.message == "failed to negotiate an output format");
  CHECK(error.detail.find("direct") != std::string::npos);
  CHECK(error.detail.find("mixed") != std::string::npos);
}

TEST_CASE("format_fallback downgrades bit depth when device rejects requested format") {
  // 计划 happy 场景：指定 Int24，设备仅支持 Float32（同采样率/声道）→
  // 候选链 Int24 → Int16 → Float32 自动降级并通知，不报错。
  const auto path = sineFixture("format_fallback_int24_to_float32.wav");

  const auto result = loadWith({{AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2}},
                               config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Int24, 2),
                               path,
                               "int24-to-float32");
  CHECK(result.requests.size() == 3U);
  CHECK(result.requests[0].sampleFormat == AudioSampleFormat::Int24);
  CHECK(result.requests[1].sampleFormat == AudioSampleFormat::Int16);
  CHECK(result.requests[2].sampleFormat == AudioSampleFormat::Float32);

  CHECK(eventsOf(result.events, BackendEventType::PlaybackError).empty());
  const auto& fallback = std::get<OutputModeFallback>(firstEventOf(result.events, BackendEventType::OutputModeFallback).payload);
  CHECK(fallback.requestedConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(fallback.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK_FALSE(fallback.reason.empty());
  CHECK(fallback.reason.find("24 位") != std::string::npos);
  CHECK(fallback.reason.find("32 位浮点") != std::string::npos);
  CHECK(fallback.effectiveFormat.sampleFormat == AudioSampleFormat::Float32);
  CHECK(fallback.effectiveFormat.sampleRate == 96000U);
  CHECK(fallback.effectiveFormat.fallbackApplied);

  const auto& format = std::get<OutputFormatChanged>(firstEventOf(result.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(format.sampleFormat == AudioSampleFormat::Float32);
  CHECK(format.fallbackApplied);
}

TEST_CASE("format_fallback rejects unsupported sample format enum and moves to next candidate") {
  // 人为构造不支持枚举值（Unknown）：候选被拒后降级到 Int16 成功播放 + 通知。
  const auto path = sineFixture("format_fallback_unknown_enum.wav");

  const auto result = loadWith({{AudioOutputMode::Mixed, 96000, AudioSampleFormat::Int16, 2}},
                               config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Unknown, 2),
                               path,
                               "unknown-enum");
  CHECK(result.requests.size() == 1U);
  CHECK(result.requests[0].sampleFormat == AudioSampleFormat::Int16);

  CHECK(eventsOf(result.events, BackendEventType::PlaybackError).empty());
  const auto& fallback = std::get<OutputModeFallback>(firstEventOf(result.events, BackendEventType::OutputModeFallback).payload);
  CHECK_FALSE(fallback.reason.empty());
  CHECK(fallback.reason.find("16 位整数") != std::string::npos);
  CHECK(fallback.effectiveFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(fallback.effectiveFormat.fallbackApplied);
}

TEST_CASE("format_fallback respects allowFallback disabled") {
  // allowFallback=false：候选循环退化为单候选（用户指定格式），设备不支持即失败。
  const auto path = sineFixture("format_fallback_allow_fallback_off.wav");

  auto outputConfig = config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Int24, 2);
  outputConfig.allowFallback = false;
  const auto result = loadWith({{AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2}},
                               outputConfig,
                               path,
                               "allow-fallback-off");
  CHECK(result.requests.size() == 1U);
  CHECK(result.requests[0].sampleFormat == AudioSampleFormat::Int24);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  CHECK(eventsOf(result.events, BackendEventType::OutputFormatChanged).empty());
  const auto& error = std::get<PlaybackError>(firstEventOf(result.events, BackendEventType::PlaybackError).payload);
  CHECK(error.code == PlaybackErrorCode::FormatNegotiationFailed);
}

TEST_CASE("mixed_output_noreopen same-format switches keep the device open and swap the queue") {
  // T7 happy path：Mixed 同参数连切（含播放中切与自然播完后再切）——设备
  // initialize/uninitialize 恒 0 次增量；每曲解码/队列重建一次（渲染字节证明播放的
  // 是新曲内容）；TrackChanged 每曲照常，OutputFormatChanged 仅首曲实变时发一次。
  const auto sinePath = sineFixture("noreopen_sine.wav");
  const auto silentPath = [&] {
    const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
    const auto path = root / "noreopen_silent.wav";
    writeWav(path, std::vector<std::int16_t>(kSampleRate / 10U * kChannels, 0));
    return path;
  }();

  auto backend = std::make_unique<CountingAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  MutexEventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig outputConfig{};
  outputConfig.preferredDeviceId = "counting-device";
  outputConfig.bufferDuration = 20ms;
  player.configureOutput(outputConfig);

  // 1) 首曲：正常协商开设备并播放（可闻正弦）。
  player.loadTrack(request(sinePath, "noreopen-a"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-a", PlaybackState::Ready); }));
  player.play();
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-a", PlaybackState::Playing); }));
  const auto firstRender = fake->renderFrames(960U);
  CHECK(std::any_of(firstRender.begin(), firstRender.end(), [](std::uint8_t byte) { return byte != 0U; }));

  // 2) 播放中切到同格式静音曲：免重开（计数不增），队列原地换新后播放的是新曲内容。
  player.loadTrack(request(silentPath, "noreopen-b"));
  player.play();
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-b", PlaybackState::Ready); }));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-b", PlaybackState::Playing); }));
  const auto secondRender = fake->renderFrames(960U);
  CHECK(std::none_of(secondRender.begin(), secondRender.end(), [](std::uint8_t byte) { return byte != 0U; }));
  CHECK(fake->initializeCalls == 1);
  CHECK(fake->uninitializeCalls == 0);
  CHECK(fake->requests.size() == 1U);

  // 3) 再次切歌（第三次换队列仍不重开设备），渲染重新可闻。
  player.loadTrack(request(sinePath, "noreopen-c"));
  player.play();
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-c", PlaybackState::Ready); }));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-c", PlaybackState::Playing); }));
  const auto thirdRender = fake->renderFrames(960U);
  CHECK(std::any_of(thirdRender.begin(), thirdRender.end(), [](std::uint8_t byte) { return byte != 0U; }));

  // 4) 自然播完（naturalEnd 只停设备不拆设备，到达时设备已停）后再切同格式曲：
  //    "非 handoff 路径 LoadTrack 到达时设备已停"的既有事实——仍免重开。
  for (int index = 0; index < 200 && !hasEvent(eventLog.snapshot(), BackendEventType::PlaybackEnded); ++index) {
    static_cast<void>(fake->renderFrames(960U));
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(hasEvent(eventLog.snapshot(), BackendEventType::PlaybackEnded));
  player.loadTrack(request(sinePath, "noreopen-d"));
  player.play();
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-d", PlaybackState::Ready); }));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "noreopen-d", PlaybackState::Playing); }));
  const auto fourthRender = fake->renderFrames(960U);
  CHECK(std::any_of(fourthRender.begin(), fourthRender.end(), [](std::uint8_t byte) { return byte != 0U; }));

  // 5) 全程：设备只开一次、从未拆卸；每曲 TrackChanged 照常；OutputFormatChanged
  //    仅首曲实变一次；无真实错误（手工渲染可能产生 BufferUnderrun，属测试手法噪声）。
  const auto events = eventLog.snapshot();
  CHECK(fake->initializeCalls == 1);
  CHECK(fake->uninitializeCalls == 0);
  CHECK(fake->requests.size() == 1U);
  CHECK(eventsOf(events, BackendEventType::TrackChanged).size() == 4U);
  CHECK(eventsOf(events, BackendEventType::OutputFormatChanged).size() == 1U);
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code != PlaybackErrorCode::BufferUnderrun;
  }));
  const auto clock = player.queryPlaybackClock();
  CHECK(clock.trackId == "noreopen-d");
  CHECK(clock.position > 0ms);
  player.setEventSink(BackendEventSink{});
}

TEST_CASE("mixed_output_noreopen direct mode still reopens the device for every track") {
  // Direct 恒重开回归：同参数连切 3 曲，每曲 uninit+init（首曲无拆卸）；协商事件语义
  // 不变（L1 收紧同样生效：设备格式逐曲一致 → OutputFormatChanged 仅首曲实变一次）。
  auto backend = std::make_unique<CountingAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  MutexEventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig outputConfig{};
  outputConfig.outputMode = AudioOutputMode::Direct;
  outputConfig.preferredDeviceId = "counting-device";
  outputConfig.bufferDuration = 20ms;
  player.configureOutput(outputConfig);

  player.loadTrack(request(sineFixture("noreopen_direct_a.wav"), "direct-a"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "direct-a", PlaybackState::Ready); }));
  player.loadTrack(request(sineFixture("noreopen_direct_b.wav"), "direct-b"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "direct-b", PlaybackState::Ready); }));
  player.loadTrack(request(sineFixture("noreopen_direct_c.wav"), "direct-c"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "direct-c", PlaybackState::Ready); }));

  const auto events = eventLog.snapshot();
  CHECK(fake->initializeCalls == 3);
  CHECK(fake->uninitializeCalls == 2);
  CHECK(fake->requests.size() == 3U);
  CHECK(fake->requests[1].config.outputMode == AudioOutputMode::Direct);
  CHECK(eventsOf(events, BackendEventType::TrackChanged).size() == 3U);
  CHECK(eventsOf(events, BackendEventType::OutputFormatChanged).size() == 1U);
  CHECK(eventsOf(events, BackendEventType::PlaybackError).empty());
  const auto clock = player.queryPlaybackClock();
  CHECK(clock.trackId == "direct-c");
  player.setEventSink(BackendEventSink{});
}

TEST_CASE("mixed_output_noreopen format change reopens the device and emits once") {
  // Mixed 参数实变照旧整段协商：configureOutput 改变目标采样率后的 LoadTrack 必须
  // uninit+init 一次，且 OutputFormatChanged 实变恰发一次（事件载荷为 96kHz）。
  auto backend = std::make_unique<CountingAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  MutexEventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig outputConfig{};
  outputConfig.preferredDeviceId = "counting-device";
  outputConfig.bufferDuration = 20ms;
  player.configureOutput(outputConfig);

  const auto path = sineFixture("noreopen_reformat.wav");
  player.loadTrack(request(path, "reformat-48k"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "reformat-48k", PlaybackState::Ready); }));
  CHECK(fake->initializeCalls == 1);
  CHECK(fake->uninitializeCalls == 0);

  outputConfig.targetSampleRate = 96000;
  player.configureOutput(outputConfig);
  player.loadTrack(request(path, "reformat-96k"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "reformat-96k", PlaybackState::Ready); }));

  const auto events = eventLog.snapshot();
  CHECK(fake->initializeCalls == 2);
  CHECK(fake->uninitializeCalls == 1);
  CHECK(fake->requests.size() == 2U);
  CHECK(fake->requests[1].sampleRate == 96000U);
  const auto formats = eventsOf(events, BackendEventType::OutputFormatChanged);
  REQUIRE(formats.size() == 2U);
  CHECK(std::get<OutputFormatChanged>(formats[1].payload).deviceFormat.sampleRate == 96000U);
  CHECK(std::get<OutputFormatChanged>(formats[1].payload).deviceFormat.actualMode == AudioOutputMode::Mixed);
  CHECK(eventsOf(events, BackendEventType::TrackChanged).size() == 2U);
  CHECK(eventsOf(events, BackendEventType::PlaybackError).empty());
  const auto clock = player.queryPlaybackClock();
  CHECK(clock.trackId == "reformat-96k");
  player.setEventSink(BackendEventSink{});
}

TEST_CASE("mixed_output_noreopen output device target change reopens even with identical format") {
  // "设备目标未变"判据：仅 preferredDeviceId（selectOutputDevice）变化的 LoadTrack
  // 保持同格式，但必须整段重新协商到新设备，不得误短路。
  auto backend = std::make_unique<CountingAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  MutexEventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig outputConfig{};
  outputConfig.preferredDeviceId = "counting-device";
  player.configureOutput(outputConfig);

  const auto path = sineFixture("noreopen_retarget.wav");
  player.loadTrack(request(path, "retarget-first"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "retarget-first", PlaybackState::Ready); }));
  CHECK(fake->initializeCalls == 1);

  player.selectOutputDevice("second-device");
  player.loadTrack(request(path, "retarget-second"));
  REQUIRE(waitUntil(eventLog, [](const auto& events) { return trackReachedState(events, "retarget-second", PlaybackState::Ready); }));

  const auto events = eventLog.snapshot();
  CHECK(fake->initializeCalls == 2);
  CHECK(fake->uninitializeCalls == 1);
  REQUIRE(fake->requests.size() == 2U);
  CHECK(fake->requests[1].config.preferredDeviceId == "second-device");
  CHECK(eventsOf(events, BackendEventType::TrackChanged).size() == 2U);
  CHECK(eventsOf(events, BackendEventType::PlaybackError).empty());
  const auto clock = player.queryPlaybackClock();
  CHECK(clock.trackId == "retarget-second");
  player.setEventSink(BackendEventSink{});
}

}
