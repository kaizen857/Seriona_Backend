#include "transition_equivalence_capture.h"

#include "seriona/audio/audio_playback_service.h"
#include "seriona/audio/device/audio_output_device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

constexpr std::uint32_t kCapSampleRate = 48'000;
constexpr std::uint16_t kCapChannels = 2;
constexpr double kPi = 3.141592653589793238462643383279502884;

std::uint32_t bytesPerSampleOf(AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::Int16:
      return 2U;
    case AudioSampleFormat::Int24:
      return 3U;
    case AudioSampleFormat::Int32:
    case AudioSampleFormat::Float32:
      return 4U;
    default:
      throw std::runtime_error("unsupported capture sample format");
  }
}

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

std::vector<std::int16_t> makeSine(std::uint32_t frames) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kCapSampleRate);
    samples.push_back(static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0)));
  }
  return samples;
}

// 16-bit PCM 单声道 48k WAV（与单曲目测试 fixture 同构；喂 ffmpeg 解码路径）。
void writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples) {
  std::filesystem::create_directories(path.parent_path());
  const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.good()) {
    throw std::runtime_error("failed to create fixture: " + path.string());
  }
  writeU32(output, 0x46464952U);  // "RIFF"
  writeU32(output, 36U + dataSize);
  output.write("WAVEfmt ", 8U);
  writeU32(output, 16U);
  writeU16(output, 1U);
  writeU16(output, 1U);
  writeU32(output, kCapSampleRate);
  writeU32(output, kCapSampleRate * 2U);
  writeU16(output, 2U);
  writeU16(output, 16U);
  output.write("data", 4U);
  writeU32(output, dataSize);
  for (const auto sample : samples) {
    writeU16(output, static_cast<std::uint16_t>(sample));
  }
  if (!output.good()) {
    throw std::runtime_error("failed to write fixture: " + path.string());
  }
}

class EventLog {
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

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock{mutex_};
    return events_.size();
  }

private:
  mutable std::mutex mutex_{};
  std::vector<BackendEvent> events_{};
};

// 捕获期渲染驱动：enumeratePlaybackDevices 恒返回受支持声明（空能力列表 = 全支持），
// initialize 全接受并按请求记录当前协商格式；consumeFrames 按真实位宽分配回调缓冲后
// 直调 AudioOutputDevice::renderCallback。当前请求格式 = fake 记录的 negotiate 结果
// （Mixed 目标 = 请求格式；Direct = 流原生参数）。
class CaptureFakeBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {advertised}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    current.deviceId = request.config.preferredDeviceId.empty() ? "capture-device" : request.config.preferredDeviceId;
    current.sampleRate = request.sampleRate;
    current.sampleFormat = request.sampleFormat;
    current.channelCount = request.channelCount;
    current.bufferFrames = request.bufferFrames;
    current.actualMode = request.config.outputMode;
    queue = request.pcmQueue;
    userData = request.callbackUserData;
    return true;
  }

  [[nodiscard]] bool start() override {
    ++startCalls;
    return true;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    return true;
  }

  void uninitialize() noexcept override {
    ++uninitializeCalls;
    queue = nullptr;
    userData = nullptr;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return current; }

  void consumeFrames(std::uint32_t frames) {
    if (userData == nullptr) {
      throw std::runtime_error("capture backend not initialized");
    }
    const std::size_t bytesPerFrame =
        static_cast<std::size_t>(current.channelCount) * bytesPerSampleOf(current.sampleFormat);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(frames) * bytesPerFrame, 0U);
    AudioOutputDevice::renderCallback(userData, buffer.data(), frames);
    if (capture) {
      capture->insert(capture->end(), buffer.begin(), buffer.end());
    }
    renderedFrames += frames;
  }

  std::vector<std::uint8_t>* capture{nullptr};
  AudioDeviceFormat advertised{.deviceId = "capture-device",
                               .deviceName = "Capture Device",
                               .backendName = "fake",
                               .sampleRate = kCapSampleRate,
                               .sampleFormat = AudioSampleFormat::Float32,
                               .channelCount = kCapChannels,
                               .bufferFrames = 480,
                               .actualMode = AudioOutputMode::Mixed,
                               .fallbackApplied = false,
                               .supportedSampleFormats = {},
                               .supportedSampleRates = {}};
  AudioDeviceFormat current{};
  PcmBufferQueue* queue{nullptr};
  AudioOutputDevice* userData{nullptr};
  std::atomic<std::uint64_t> renderedFrames{0};
  std::atomic<int> initializeCalls{0};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  std::atomic<int> uninitializeCalls{0};
};

bool hasStateSince(const std::vector<BackendEvent>& events, PlaybackState state) {
  return std::any_of(events.begin(), events.end(), [state](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackStateChanged &&
           std::get<PlaybackStateChanged>(event.payload).state == state;
  });
}

bool hasEventSince(const std::vector<BackendEvent>& events, BackendEventType type) {
  return std::any_of(events.begin(), events.end(), [type](const BackendEvent& event) { return event.type == type; });
}

bool hasTrackChangedSince(const std::vector<BackendEvent>& events, const std::string& trackId) {
  return std::any_of(events.begin(), events.end(), [&trackId](const BackendEvent& event) {
    return event.type == BackendEventType::TrackChanged &&
           std::get<TrackChanged>(event.payload).request.trackId == trackId;
  });
}

// 以 ≈ 实时节奏消费 240 帧/步（5ms 音频 + 5ms 睡眠）：两树 worker 命令处理窗口都不会
// 造成人工欠载（learnings T11：fake 消费节奏必须 ≈ 实时，否则任何命令处理窗口都会制造
// 测试自造欠载静音块，破坏样本确定性）。
void pacedConsume(CaptureFakeBackend& fake, std::uint32_t frames) {
  std::uint32_t driven = 0U;
  while (driven < frames) {
    fake.consumeFrames(240U);
    driven += 240U;
    std::this_thread::sleep_for(5ms);
  }
}

void waitForStateSince(const EventLog& log, std::size_t fromCount, PlaybackState state) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = log.snapshot();
    const auto begin = snapshot.begin() + static_cast<std::ptrdiff_t>(std::min(fromCount, snapshot.size()));
    if (hasStateSince(std::vector<BackendEvent>(begin, snapshot.end()), state)) {
      return;
    }
    std::this_thread::sleep_for(1ms);
  }
  throw std::runtime_error("timed out waiting for playback state");
}

void waitForEventTypeSince(const EventLog& log, std::size_t fromCount, BackendEventType type) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = log.snapshot();
    const auto begin = snapshot.begin() + static_cast<std::ptrdiff_t>(std::min(fromCount, snapshot.size()));
    if (hasEventSince(std::vector<BackendEvent>(begin, snapshot.end()), type)) {
      return;
    }
    std::this_thread::sleep_for(1ms);
  }
  throw std::runtime_error("timed out waiting for playback event");
}

bool scanForUnderrun(const std::vector<BackendEvent>& events) {
  return std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::BufferUnderrun;
  });
}

TrackPlaybackRequest makeRequest(const std::filesystem::path& path, std::string trackId) {
  TrackPlaybackRequest request{};
  request.trackId = std::move(trackId);
  request.filePath = path;
  return request;
}

struct ScenarioSession {
  CaptureFakeBackend* fake{nullptr};
  std::shared_ptr<AudioPlaybackService> service;
  EventLog log;
  AudioSampleFormat format{AudioSampleFormat::Float32};
  bool direct{false};
  std::vector<std::uint8_t> captured;
};

void makeSession(ScenarioSession& session, AudioSampleFormat format, bool direct) {
  auto backend = std::make_unique<CaptureFakeBackend>();
  session.fake = backend.get();
  session.fake->capture = &session.captured;
  session.format = format;
  session.direct = direct;
  session.service = makeAudioPlaybackService(std::move(backend));
  session.service->setEventSink(session.log.sink());

  AudioOutputConfig config{};
  config.outputMode = direct ? AudioOutputMode::Direct : AudioOutputMode::Mixed;
  config.preferredDeviceId = "capture-device";
  config.targetSampleRate = kCapSampleRate;
  config.targetSampleFormat = format;
  config.targetChannelCount = kCapChannels;
  config.bufferDuration = 150ms;
  // 录制格式必须按请求交付：allowFallback=false 使协商失败即报错（防静默降级污染比较）。
  config.allowFallback = false;
  session.service->configureOutput(config);
}

EquivCapture finalizeSession(ScenarioSession& session) {
  const auto events = session.log.snapshot();
  EquivCapture capture;
  capture.bytes = std::move(session.captured);
  capture.format = session.fake->current.sampleFormat;
  capture.sampleRate = session.fake->current.sampleRate;
  capture.channelCount = session.fake->current.channelCount;
  capture.renderedFrames = session.fake->renderedFrames.load();
  capture.underrunObserved = scanForUnderrun(events);
  session.service->setEventSink(BackendEventSink{});
  session.service.reset();
  return capture;
}

void loadAndPlay(ScenarioSession& session, const TrackPlaybackRequest& request) {
  const std::size_t eventsBefore = session.log.size();
  session.service->loadTrack(request);
  waitForStateSince(session.log, eventsBefore, PlaybackState::Ready);
  session.service->play();
  waitForStateSince(session.log, eventsBefore, PlaybackState::Playing);
  // 冷启动/恢复无淡入（9 项默认 fade 全关）：Playing 即满增益，无握手等待。
}

EquivCapture capturePlaythrough(ScenarioSession& session, const TrackPlaybackRequest& request) {
  loadAndPlay(session, request);
  // 固定块数消费（600ms = 120 块 + 2 块零尾余量）：尾块内容跨树确定性来自"解码帧数
  // 相同 + 队列耗尽后渲染全零"，不以 PlaybackEnded 事件到达时刻驱动（消除事件/消费
  // 交错竞态）。事件事后核验自然播完确实发生。
  pacedConsume(*session.fake, 240U * 122U);
  const auto events = session.log.snapshot();
  const bool ended = hasEventSince(events, BackendEventType::PlaybackEnded) ||
                     hasStateSince(events, PlaybackState::Stopped);
  if (!ended) {
    throw std::runtime_error("playthrough: natural end not observed after fixed consumption");
  }
  session.service->stop();
  return finalizeSession(session);
}

EquivCapture capturePauseResume(ScenarioSession& session, const TrackPlaybackRequest& request) {
  loadAndPlay(session, request);
  pacedConsume(*session.fake, 2400U);  // 50ms
  const std::size_t eventsBeforePause = session.log.size();
  session.service->pause();
  waitForStateSince(session.log, eventsBeforePause, PlaybackState::Paused);
  session.service->resume();
  waitForStateSince(session.log, eventsBeforePause, PlaybackState::Playing);
  pacedConsume(*session.fake, 2400U);  // 50ms
  session.service->stop();
  return finalizeSession(session);
}

EquivCapture captureSeek(ScenarioSession& session, const TrackPlaybackRequest& request) {
  loadAndPlay(session, request);
  pacedConsume(*session.fake, 2400U);  // 50ms
  const std::size_t eventsBeforeSeek = session.log.size();
  session.service->seek(700ms);
  waitForEventTypeSince(session.log, eventsBeforeSeek, BackendEventType::PositionDiscontinuity);
  waitForStateSince(session.log, eventsBeforeSeek, PlaybackState::Playing);
  pacedConsume(*session.fake, 2400U);  // 700ms 起 50ms
  session.service->stop();
  return finalizeSession(session);
}

EquivCapture captureSwitch(ScenarioSession& session,
                           const TrackPlaybackRequest& requestA,
                           const TrackPlaybackRequest& requestB) {
  loadAndPlay(session, requestA);
  pacedConsume(*session.fake, 2400U);  // A 前 50ms
  const std::size_t eventsBeforeSwitch = session.log.size();
  session.service->loadTrack(requestB);
  session.service->play();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = session.log.snapshot();
    const auto begin = snapshot.begin() + static_cast<std::ptrdiff_t>(std::min(eventsBeforeSwitch, snapshot.size()));
    const auto window = std::vector<BackendEvent>(begin, snapshot.end());
    if (hasTrackChangedSince(window, requestB.trackId) && hasStateSince(window, PlaybackState::Playing)) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const auto events = session.log.snapshot();
  const auto begin = events.begin() + static_cast<std::ptrdiff_t>(std::min(eventsBeforeSwitch, events.size()));
  const auto window = std::vector<BackendEvent>(begin, events.end());
  if (!hasTrackChangedSince(window, requestB.trackId) || !hasStateSince(window, PlaybackState::Playing)) {
    throw std::runtime_error("switch: target track not adopted");
  }
  pacedConsume(*session.fake, 2400U);  // B 前 50ms
  session.service->stop();
  return finalizeSession(session);
}

std::string scenarioBaseName(AudioSampleFormat format, const char* scenario) {
  if (format == AudioSampleFormat::Unknown) {
    return scenario;
  }
  return std::string(scenario) + "_" + formatName(format);
}

}  // namespace

std::string formatName(AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::Float32:
      return "Float32";
    case AudioSampleFormat::Int16:
      return "Int16";
    case AudioSampleFormat::Int24:
      return "Int24";
    case AudioSampleFormat::Int32:
      return "Int32";
    default:
      return "Unknown";
  }
}

std::string equivalenceCaptureKey(AudioSampleFormat format, const char* scenario) {
  return scenarioBaseName(format, scenario);
}

EquivCaptureMap runEquivalenceCaptures(const EquivCaptureOptions& options) {
  const auto fixtureDir =
      options.fixtureDir.empty() ? std::filesystem::current_path() / "generated_task13_fixtures" : options.fixtureDir;

  const auto playPath = fixtureDir / "capture_play.wav";    // 600ms
  const auto trackPath = fixtureDir / "capture_track.wav";  // 1000ms
  writeWav(playPath, makeSine(kCapSampleRate * 600U / 1000U));
  writeWav(trackPath, makeSine(kCapSampleRate));

  // 每个 (场景 × 格式) 用全新会话（独立 service/fake/事件日志/捕获缓冲），杜绝跨场景
  // 事件或字节串扰。
  EquivCaptureMap results;
  for (const auto format : options.formats) {
    if (format == AudioSampleFormat::Unknown) {
      continue;
    }
    ScenarioSession playthrough;
    makeSession(playthrough, format, /*direct=*/false);
    results[equivalenceCaptureKey(format, "playthrough")] =
        capturePlaythrough(playthrough, makeRequest(playPath, "eq-playthrough"));

    ScenarioSession pauseResume;
    makeSession(pauseResume, format, /*direct=*/false);
    results[equivalenceCaptureKey(format, "pause_resume")] =
        capturePauseResume(pauseResume, makeRequest(trackPath, "eq-pause-resume"));

    ScenarioSession seek;
    makeSession(seek, format, /*direct=*/false);
    results[equivalenceCaptureKey(format, "seek")] = captureSeek(seek, makeRequest(trackPath, "eq-seek"));

    ScenarioSession switchMixed;
    makeSession(switchMixed, format, /*direct=*/false);
    results[equivalenceCaptureKey(format, "switch_mixed")] = captureSwitch(
        switchMixed, makeRequest(trackPath, "eq-switch-a"), makeRequest(trackPath, "eq-switch-b"));
  }
  if (options.includeDirectSwitch) {
    // Direct 恒按轨道原生参数协商（流 Int16/单声道），格式与目标设置无关 → 单键录制。
    ScenarioSession direct;
    makeSession(direct, AudioSampleFormat::Float32, /*direct=*/true);
    results["switch_direct"] =
        captureSwitch(direct, makeRequest(trackPath, "eq-direct-a"), makeRequest(trackPath, "eq-direct-b"));
  }
  return results;
}

}  // namespace seriona::audio
