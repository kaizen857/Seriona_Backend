#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

// 慢机预算：极端饥饿（nice19 对 8×nice0 占位，≈600x）下单条 worker 命令的调度延迟
// 可达数秒（审计实测 0.5–4.6s）；固定 2s 等待会把"worker 尚未跑到"误判为失败
// （BE-02 崩溃链起点）。所有等待改为宽裕预算 + 条件轮询：条件满足即返回，正常档零等待。
constexpr auto kSlowMachineWaitBudget = std::chrono::seconds{180};

// 慢机 ring 生产闸门兜底上限（内容耗尽后的合法静音尾不依赖本闸门，事件驱动退出）。
constexpr auto kRingAvailableBudget = std::chrono::seconds{3};

// 慢机整轨排空预算：1ms ring（16 帧/回调）由 worker 每个进度 tick 填充一次，整轨
// 48000 帧需要 3000 个 tick；≈600x 饥饿下实测供数约 176 帧/s（本轮 9:1 rig），
// 通用 180s 预算只能推进约 660ms 即耗尽（check 原样失败）。整轨排空单独放宽到 900s：
// 条件满足即返回（正常档零等待），断言与帧语义均不变。
constexpr auto kSlowMachineDrainBudget = std::chrono::seconds{900};

constexpr std::uint32_t kSampleRate = 48'000;
constexpr double kPi = 3.141592653589793238462643383279502884;

void writeU16(std::ofstream& stream, std::uint16_t value) {
  const auto bytes = std::array<unsigned char, 2>{static_cast<unsigned char>(value & 0xFFU),
                                                  static_cast<unsigned char>((value >> 8U) & 0xFFU)};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ofstream& stream, std::uint32_t value) {
  const auto bytes = std::array<unsigned char, 4>{static_cast<unsigned char>(value & 0xFFU),
                                                  static_cast<unsigned char>((value >> 8U) & 0xFFU),
                                                  static_cast<unsigned char>((value >> 16U) & 0xFFU),
                                                  static_cast<unsigned char>((value >> 24U) & 0xFFU)};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeTag(std::ofstream& stream, const char tag[4]) { stream.write(tag, 4); }

std::filesystem::path writeSineFixture(std::string name, std::uint32_t frames) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  std::filesystem::create_directories(root);

  const std::uint32_t dataSize = frames * sizeof(std::int16_t);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());

  writeTag(output, "RIFF");
  writeU32(output, 36U + dataSize);
  writeTag(output, "WAVE");
  writeTag(output, "fmt ");
  writeU32(output, 16U);
  writeU16(output, 1U);
  writeU16(output, 1U);
  writeU32(output, kSampleRate);
  writeU32(output, kSampleRate * sizeof(std::int16_t));
  writeU16(output, sizeof(std::int16_t));
  writeU16(output, 16U);
  writeTag(output, "data");
  writeU32(output, dataSize);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0));
    writeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
  return path;
}

class SmallBufferBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    queue.store(request.pcmQueue, std::memory_order_release);
    userData.store(request.callbackUserData, std::memory_order_release);
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    initialized.store(true, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool start() override {
    ++startCalls;
    started.store(true, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    started.store(false, std::memory_order_release);
    return true;
  }

  void uninitialize() noexcept override {
    initialized.store(false, std::memory_order_release);
    started.store(false, std::memory_order_release);
    queue.store(nullptr, std::memory_order_release);
    userData.store(nullptr, std::memory_order_release);
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  void consume(std::uint32_t frames) {
    // 慢机守卫（建模真实设备）：未 initialize / 已 uninitialize 不触发回调——
    // 饥饿下 worker 可能尚未执行 loadTrack 的 initialize（原 REQUIRE 会致命）。
    auto* data = userData.load(std::memory_order_acquire);
    if (data == nullptr) {
      return;
    }
    // 慢机 ring 生产闸门：worker 未把内容写进 ring 前渲染会读到静音补零，位置失速
    // 且误报欠载（断言按"16 帧小缓冲持续供数"设计）。等可用帧就绪，宽裕截止兜底。
    if (auto* ring = queue.load(std::memory_order_acquire); ring != nullptr) {
      const auto deadline = std::chrono::steady_clock::now() + kRingAvailableBudget;
      while (ring->availableFrames() < frames && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }
    callbackBuffer.assign(static_cast<std::size_t>(frames) * format.channelCount * 4U, 0U);
    AudioOutputDevice::renderCallback(data, callbackBuffer.data(), frames);
    if (std::any_of(callbackBuffer.begin(), callbackBuffer.end(), [](std::uint8_t value) { return value != 0U; })) {
      ++nonSilentCallbacks;
    } else {
      ++silentCallbacks;
    }
  }

  AudioDeviceFormat format{.deviceId = "small-buffer",
                           .deviceName = "Small Buffer",
                           .backendName = "fake",
                           .sampleRate = kSampleRate,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 1,
                           .bufferFrames = 16,
                           .actualMode = AudioOutputMode::Mixed};
  // 设备面指针/计数由 worker 线程写、测试线程读 → atomic 防无同步轮询数据竞争。
  std::atomic<PcmBufferQueue*> queue{nullptr};
  std::atomic<AudioOutputDevice*> userData{nullptr};
  std::vector<std::uint8_t> callbackBuffer{};
  std::atomic<int> initializeCalls{0};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  std::atomic<int> nonSilentCallbacks{0};
  std::atomic<int> silentCallbacks{0};
  std::atomic<bool> initialized{false};
  std::atomic<bool> started{false};
};

// 事件收集：worker 线程（事件 sink）写、测试线程读 → 互斥保护；断言一律走快照
// （原始裸 vector 并发读写是 BE-02 heap corruption / smallbin double free 的根因）。
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

bool hasEvent(const std::vector<BackendEvent>& events, BackendEventType type) {
  return std::any_of(events.begin(), events.end(), [type](const BackendEvent& event) { return event.type == type; });
}

// 慢机轮询原语：谓词满足即返回，预算耗尽返回末值（正常档零等待）。
template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds budget = kSlowMachineWaitBudget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

// 慢机时钟新鲜度：反复投递 queryPlaybackClock（命令 FIFO 于 worker 串行域）直到快照
// 满足条件——原始直读在饥饿下会拿到 2s 超时的陈旧值。
template <typename Query, typename Predicate>
PlaybackClockSnapshot waitForClock(Query&& query,
                                   Predicate&& condition,
                                   std::chrono::milliseconds budget = kSlowMachineWaitBudget) {
  auto snapshot = query();
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!condition(snapshot) && std::chrono::steady_clock::now() < deadline) {
    snapshot = query();
  }
  return snapshot;
}

TrackPlaybackRequest requestFor(const std::filesystem::path& path) {
  return TrackPlaybackRequest{.trackId = "small-buffer-track",
                              .filePath = path,
                              .title = "Small Buffer Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}}

TEST_CASE("audio_player_small_buffer keeps playback running when decoded frames exceed queue capacity") {
  const auto path = writeSineFixture("audio_player_small_buffer.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  // 声明序即析构逆序：eventLog 先于 player 声明——测试中止时 player/service 先析构
  // （join worker + 清 sink），worker 迟到派发不会触碰已销毁日志（BE-02 崩溃放大路径）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  // 慢机就绪屏障：等 worker 真正执行到 initialize+start（fake 计数），而非 2s 屏障
  // 超时返回陈旧快照后就在未初始化设备上 consume（BE-02 首因）。
  REQUIRE(waitUntil([&] { return fake->initialized.load() && fake->started.load(); }));
  for (int index = 0; index < 40; ++index) {
    fake->consume(16U);
    static_cast<void>(player.queryPlaybackClock());
  }
  const auto clock = waitForClock([&] { return player.queryPlaybackClock(); },
                                  [](const PlaybackClockSnapshot& snapshot) { return snapshot.position > 0ms; });

  CHECK(clock.position > 0ms);
  const auto events = eventLog.snapshot();
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
}

TEST_CASE("audio_player_small_buffer refills playback without clock polling") {
  const auto path = writeSineFixture("audio_player_small_buffer_no_poll.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  // 慢机就绪屏障：initialize+start 完成前 consume 会读到未初始化设备（BE-02）。
  REQUIRE(waitUntil([&] { return fake->initialized.load() && fake->started.load(); }));
  // refill 由 worker 完成：固定 80×2ms 节拍窗在慢机上不足 → 谓词驱动（宽裕预算，
  // 条件满足即返回；正常档仍 ~20ms 内完成）。
  const auto refillDeadline = std::chrono::steady_clock::now() + kSlowMachineWaitBudget;
  while (fake->nonSilentCallbacks.load() <= 10 && std::chrono::steady_clock::now() < refillDeadline) {
    fake->consume(16U);
    std::this_thread::sleep_for(2ms);
  }

  const auto clock = waitForClock([&] { return player.queryPlaybackClock(); },
                                  [](const PlaybackClockSnapshot& snapshot) { return snapshot.position > 0ms; });
  CHECK(clock.position > 0ms);
  CHECK(fake->nonSilentCallbacks.load() > 10);
  CHECK(fake->started.load());
}

TEST_CASE("audio_player_small_buffer publishes progress while playing without clock polling") {
  const auto path = writeSineFixture("audio_player_small_buffer_progress_events.wav", kSampleRate * 2U);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  // 声明序即析构逆序：eventLog 先于 player 声明（测试中止路径防悬垂 sink）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  // 慢机就绪屏障：initialize+start 完成前 consume 会读到未初始化设备（BE-02）。
  REQUIRE(waitUntil([&] { return fake->initialized.load() && fake->started.load(); }));
  // 进度事件由 worker ticker 派发：固定 120×2ms 节拍窗在慢机上不足 → 谓词驱动（宽裕预算）。
  const auto progressDeadline = std::chrono::steady_clock::now() + kSlowMachineWaitBudget;
  while (std::chrono::steady_clock::now() < progressDeadline &&
         std::ranges::count_if(eventLog.snapshot(), [](const BackendEvent& event) {
           return event.type == BackendEventType::PlaybackPositionUpdated;
         }) < 3) {
    fake->consume(16U);
    std::this_thread::sleep_for(2ms);
  }

  std::vector<std::chrono::milliseconds> positions;
  for (const auto& event : eventLog.snapshot()) {
    if (event.type == BackendEventType::PlaybackPositionUpdated) {
      positions.push_back(std::get<PlaybackPositionUpdated>(event.payload).clock.position);
    }
  }
  REQUIRE(positions.size() >= 3U);
  CHECK(positions.back() > positions.front());
}

TEST_CASE("audio_player_small_buffer drains pending tail before playback ended") {
  const auto path = writeSineFixture("audio_player_small_buffer_tail.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  // 声明序即析构逆序：eventLog 先于 player 声明（测试中止路径防悬垂 sink）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  // 慢机就绪屏障：initialize+start 完成前 consume 会读到未初始化设备（BE-02）。
  REQUIRE(waitUntil([&] { return fake->initialized.load() && fake->started.load(); }));

  const auto drainDeadline = std::chrono::steady_clock::now() + kSlowMachineDrainBudget;
  // 消费循环只驱动渲染：进度 ticker 随 play 常驻（负责时钟推进与 PlaybackEnded 派发），
  // 循环内每轮再投一次 worker 时钟往返（饥饿下可达秒级）会让 16 帧/轮在预算内只推进
  // 几百毫秒、且循环后无消费的等待永远无法补齐。时钟在循环后用新鲜读取真值。
  // 迭代上限按"整轨 3000 次真实消费 + 闸门超时静音容忍"取 12000（4x 余量）。
  for (int index = 0; index < 12'000 && std::chrono::steady_clock::now() < drainDeadline &&
                     !hasEvent(eventLog.snapshot(), BackendEventType::PlaybackEnded);
       ++index) {
    fake->consume(16U);
  }
  // 慢机：等时钟真值到位（末次查询可能是 2s 超时的陈旧值）。
  const auto clock = waitForClock([&] { return player.queryPlaybackClock(); },
                                  [](const PlaybackClockSnapshot& snapshot) { return snapshot.position >= 990ms; });

  CHECK(clock.position >= 990ms);
  const auto events = eventLog.snapshot();
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
}

TEST_CASE("audio_player_small_buffer treats final partial callback as natural end rather than underrun") {
  const auto path = writeSineFixture("audio_player_small_buffer_tail_partial.wav", 1001U);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  // 声明序即析构逆序：eventLog 先于 player 声明（测试中止路径防悬垂 sink）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  // 慢机就绪屏障：initialize+start 完成前 consume 会读到未初始化设备（BE-02）。
  REQUIRE(waitUntil([&] { return fake->initialized.load() && fake->started.load(); }));

  const auto drainDeadline = std::chrono::steady_clock::now() + kSlowMachineWaitBudget;
  for (int index = 0; index < 400 && std::chrono::steady_clock::now() < drainDeadline &&
                     !hasEvent(eventLog.snapshot(), BackendEventType::PlaybackEnded);
       ++index) {
    fake->consume(16U);
    static_cast<void>(player.queryPlaybackClock());
  }

  const auto events = eventLog.snapshot();
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::BufferUnderrun;
  }));
}

TEST_CASE("audio_player_small_buffer suppresses tail underrun after seek near end") {
  const auto path = writeSineFixture("audio_player_small_buffer_seek_tail_partial.wav", 6001U);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  // 声明序即析构逆序：eventLog 先于 player 声明（测试中止路径防悬垂 sink）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  // 慢机就绪屏障：initialize+start 完成前 consume 会读到未初始化设备（BE-02）。
  REQUIRE(waitUntil([&] { return fake->initialized.load() && fake->started.load(); }));
  player.seek(104ms);
  static_cast<void>(player.queryPlaybackClock());

  const auto drainDeadline = std::chrono::steady_clock::now() + kSlowMachineWaitBudget;
  for (int index = 0; index < 400 && std::chrono::steady_clock::now() < drainDeadline &&
                     !hasEvent(eventLog.snapshot(), BackendEventType::PlaybackEnded);
       ++index) {
    fake->consume(16U);
    static_cast<void>(player.queryPlaybackClock());
  }

  const auto events = eventLog.snapshot();
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::BufferUnderrun;
  }));
}

TEST_CASE("audio_player_resume_from_stopped_does_not_start_device") {
  const auto path = writeSineFixture("audio_player_resume_from_stopped.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  // 声明序即析构逆序：eventLog 先于 player 声明（测试中止路径防悬垂 sink）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  static_cast<void>(player.queryPlaybackClock());
  player.stop();
  static_cast<void>(player.queryPlaybackClock());
  const int startsBeforeResume = fake->startCalls.load();
  player.resume();
  // 慢机：等 resume 的非法迁移错误真正派发（固定屏障在饥饿下会超时）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = eventLog.snapshot();
    return std::any_of(snapshot.begin(), snapshot.end(), [](const BackendEvent& event) {
      return event.type == BackendEventType::PlaybackError &&
             std::get<PlaybackError>(event.payload).message == "resume requires Paused state";
    });
  }));

  CHECK(fake->startCalls.load() == startsBeforeResume);
  CHECK_FALSE(fake->started.load());
  const auto events = eventLog.snapshot();
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).message == "resume requires Paused state";
  }));
}

TEST_CASE("audio_player_seek_from_stopped_reports_one_error_without_clock_mutation") {
  const auto path = writeSineFixture("audio_player_seek_from_stopped.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  // 声明序即析构逆序：eventLog 先于 player 声明（测试中止路径防悬垂 sink）。
  EventLog eventLog;
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  static_cast<void>(player.queryPlaybackClock());
  player.stop();
  static_cast<void>(player.queryPlaybackClock());
  const auto beforeSeek = waitForClock([&] { return player.queryPlaybackClock(); },
                                       [](const PlaybackClockSnapshot& snapshot) { return !snapshot.continuous; });
  player.seek(500ms);
  // 慢机：等非法 seek 的 SeekFailed 错误真正派发（固定屏障在饥饿下会超时）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = eventLog.snapshot();
    return std::any_of(snapshot.begin(), snapshot.end(), [](const BackendEvent& event) {
      return event.type == BackendEventType::PlaybackError &&
             std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::SeekFailed;
    });
  }));
  const auto afterSeek = waitForClock([&] { return player.queryPlaybackClock(); },
                                      [&](const PlaybackClockSnapshot& snapshot) {
                                        return snapshot.position == beforeSeek.position;
                                      });
  const auto events = eventLog.snapshot();
  const auto errorCount = std::count_if(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::SeekFailed;
  });

  CHECK(errorCount == 1);
  CHECK(afterSeek.position == beforeSeek.position);
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PositionDiscontinuity;
  }));
}

}
