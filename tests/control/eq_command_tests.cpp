// SetEqualizerConfig 均衡器命令的帧级行为测试（B1.6/任务 20，验证任务 18/19 全链）。
// 层次一（reducer 级，直驱 ControlStateReducer，仿 configure_output_reducer_tests）：
//   - 校验拒绝矩阵：载荷缺失 / mode 非法 / 增益 NaN / ±Inf / 超 ±15 dB —— 逐类断言
//     错误码 InvalidCommand、CommandRejected 通知、零状态变化、generation 不递增；
//   - 合法生效：单 SetEqualizerConfig 意图（无重载尾意图）、generation 0→1（再生效→2）、
//     快照 config 与提交一致、player 状态不动；
//   - 曲线纯函数：平直全 0dB → 全 181 点 ≈0dB（1e-9）；单 band +15dB@中心频率 → 该点
//     ≈+15dB（0.1dB，取样只取中心频率或邻近平坦区，避开 31 档 Q=4.32 陡峭裙边）；
//     Band10/Band31 活跃频段前缀语义（超前缀增益不参与曲线）；reducer 侧 sampleRate=0
//     （fs=0 全轴不截断——20k 端点保留计算值）；频率轴 181 点端点精确 + 单调对数。
// 层次二（controller 订阅级，makeMediaController + harness fake，仿 configure_output
// executes_tests）：subscribeEqualizerState 订阅即推（gen0 空快照）、合法提交后回调
// gen+1 且 config 匹配、reject 命令无推送、subscribeSpectrum 初始空快照 gen0 且不被
// 均衡器命令扰动。
#include <doctest.h>

#include "control/control_state_reducer.h"
#include "control_test_harness.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/equalizer_tables.h"
#include "seriona/control/media_controller.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;
namespace control_test = seriona::control::test;

namespace {

// —— 数值断言辅助 ——

bool closeTo(float actual, float expected, float tolerance) {
  return std::abs(static_cast<double>(actual) - static_cast<double>(expected)) <= tolerance;
}

// —— 配置构造辅助 ——

bool sameConfig(const audio::EqualizerConfig& lhs, const audio::EqualizerConfig& rhs) {
  return lhs.enabled == rhs.enabled && lhs.mode == rhs.mode && lhs.preGainDb == rhs.preGainDb &&
         lhs.bandGainsDb == rhs.bandGainsDb && lhs.limiterEnabled == rhs.limiterEnabled;
}

audio::EqualizerConfig makeConfig(audio::EqualizerBandMode mode,
                                  float preGainDb,
                                  float bandGainDb,
                                  bool enabled = true) {
  audio::EqualizerConfig config{};
  config.enabled = enabled;
  config.mode = mode;
  config.preGainDb = preGainDb;
  config.bandGainsDb.fill(bandGainDb);
  return config;
}

audio::EqualizerConfig singleBandConfig(audio::EqualizerBandMode mode, std::size_t bandIndex, float gainDb) {
  audio::EqualizerConfig config = makeConfig(mode, 0.0F, 0.0F);
  config.bandGainsDb[bandIndex] = gainDb;
  return config;
}

MediaControlCommand eqCommand(audio::EqualizerConfig config) {
  return MediaControlCommand{.kind = MediaControlCommandKind::SetEqualizerConfig, .equalizerConfig = std::move(config)};
}

MediaControlCommand eqCommandWithoutConfig() {
  return MediaControlCommand{.kind = MediaControlCommandKind::SetEqualizerConfig};
}

// 曲线频率轴上的网格点索引（181 点 20–20k 对数轴）——取样前先找离目标频率最近的
// 点，确保断言落在中心频率邻近的平坦区而非陡峭裙边（任务 20 取样纪律）。
std::size_t gridIndexNearestHz(const audio::EqualizerStateSnapshot& snapshot, double centerHz) {
  std::size_t best = 0;
  double bestDelta = std::numeric_limits<double>::infinity();
  for (std::size_t index = 0; index < snapshot.curveFrequenciesHz.size(); ++index) {
    const double delta = std::abs(static_cast<double>(snapshot.curveFrequenciesHz[index]) - centerHz);
    if (delta < bestDelta) {
      bestDelta = delta;
      best = index;
    }
  }
  return best;
}

// 快照逐字段比对（generation/config/sampleRate/曲线/频率轴全同才算状态零变化）。
bool sameEqualizerSnapshot(const audio::EqualizerStateSnapshot& lhs, const audio::EqualizerStateSnapshot& rhs) {
  return lhs.generation == rhs.generation && lhs.sampleRate == rhs.sampleRate && sameConfig(lhs.config, rhs.config) &&
         lhs.curvePointsDb == rhs.curvePointsDb && lhs.curveFrequenciesHz == rhs.curveFrequenciesHz;
}

// —— reducer 级 fixture ——

struct ReducerFixture {
  ControlStateReducer reducer{};

  ControlReduction submit(const MediaControlCommand& command) {
    return reducer.reduceCommand(command);
  }
};

// —— controller 级 fixture（订阅链经完整 MediaController 入口）——

struct ControllerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  control_test::FakeMetadataSharingService* fakeMetadata{nullptr};
  std::unique_ptr<MediaController> controller{};

  ControllerFixture() {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService)},
                                     MediaControllerOptions{.runInlineForTests = true});
  }
};

// 默认出厂配置 = 均衡器关闭 + Band10 + 全 0dB（旧行为直通等价）。
bool isFactoryDefaultConfig(const audio::EqualizerConfig& config) {
  return !config.enabled && config.mode == audio::EqualizerBandMode::Band10 && config.preGainDb == 0.0F &&
         config.bandGainsDb == std::array<float, 31>{} && !config.limiterEnabled;
}

}  // namespace

// —— reducer 级：初始快照 / 校验拒绝 / 生效 / 曲线 ——

TEST_CASE("reducer starts with an empty generation-0 equalizer snapshot and a full frequency axis") {
  ReducerFixture fixture{};

  const auto& snapshot = fixture.reducer.equalizerState();

  CHECK(snapshot.generation == 0U);
  CHECK(snapshot.sampleRate == 0U);
  CHECK(isFactoryDefaultConfig(snapshot.config));
  // 频率轴：181 点、20–20k、端点精确、严格单调（逐点对应 curvePointsDb 的绘制轴）。
  CHECK(snapshot.curveFrequenciesHz.size() == 181U);
  CHECK(snapshot.curvePointsDb.size() == snapshot.curveFrequenciesHz.size());
  CHECK(snapshot.curveFrequenciesHz.front() == 20.0F);
  CHECK(snapshot.curveFrequenciesHz.back() == 20000.0F);
  for (std::size_t index = 1; index < snapshot.curveFrequenciesHz.size(); ++index) {
    const float current = snapshot.curveFrequenciesHz[index];
    const float previous = snapshot.curveFrequenciesHz[index - 1U];
    CHECK(std::isfinite(current));
    CHECK(current > previous);
    if (current <= previous) {
      break;  // 失败点已报告，避免整条轴重复刷屏
    }
  }
  // 出厂默认曲线：全 181 点平直 0dB（1e-9 容差）。
  for (std::size_t point = 0; point < snapshot.curvePointsDb.size(); ++point) {
    CHECK(closeTo(snapshot.curvePointsDb[point], 0.0F, 1e-9F));
  }
}

TEST_CASE("reducer rejects SetEqualizerConfig missing config with InvalidCommand and zero state change") {
  ReducerFixture fixture{};
  const auto before = fixture.reducer.equalizerState();

  const auto reduction = fixture.submit(eqCommandWithoutConfig());

  CHECK_FALSE(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
  CHECK_FALSE(reduction.result.message.empty());
  CHECK(reduction.intents.empty());
  CHECK_FALSE(reduction.equalizerStateChanged);
  REQUIRE(reduction.notifications.size() == 1U);
  CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(reduction.notifications.front().errorCode == MediaControllerErrorCode::InvalidCommand);

  const auto& after = fixture.reducer.equalizerState();
  CHECK(sameEqualizerSnapshot(after, before));
}

TEST_CASE("reducer rejects SetEqualizerConfig invalid payload classes keeping state and generation unchanged") {
  ReducerFixture fixture{};
  const auto before = fixture.reducer.equalizerState();

  auto expectRejected = [&](const audio::EqualizerConfig& config) {
    const auto reduction = fixture.submit(eqCommand(config));
    CAPTURE(config.preGainDb);
    CHECK_FALSE(reduction.result.accepted);
    CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(reduction.intents.empty());
    CHECK_FALSE(reduction.equalizerStateChanged);
    REQUIRE(reduction.notifications.size() == 1U);
    CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
    CHECK(reduction.notifications.front().errorCode == MediaControllerErrorCode::InvalidCommand);
    // 状态与 generation 必须零变化（校验失败路径不触碰 equalizer_）。
    const auto& after = fixture.reducer.equalizerState();
    CHECK(sameEqualizerSnapshot(after, before));
  };

  constexpr float kAboveRange = 15.0F + 0.1F;
  constexpr float kBelowRange = -15.0F - 0.1F;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SUBCASE("band mode outside enum") {
    auto config = makeConfig(audio::EqualizerBandMode::Band10, 0.0F, 0.0F);
    config.mode = static_cast<audio::EqualizerBandMode>(7);
    expectRejected(config);
  }
  SUBCASE("pre gain above +15 dB") {
    expectRejected(makeConfig(audio::EqualizerBandMode::Band31, kAboveRange, 0.0F));
  }
  SUBCASE("pre gain below -15 dB") {
    expectRejected(makeConfig(audio::EqualizerBandMode::Band31, kBelowRange, 0.0F));
  }
  SUBCASE("pre gain is NaN") {
    expectRejected(makeConfig(audio::EqualizerBandMode::Band10, nan, 0.0F));
  }
  SUBCASE("pre gain is positive infinity") {
    expectRejected(makeConfig(audio::EqualizerBandMode::Band10, inf, 0.0F));
  }
  SUBCASE("pre gain is negative infinity") {
    expectRejected(makeConfig(audio::EqualizerBandMode::Band10, -inf, 0.0F));
  }
  SUBCASE("band gain above +15 dB") {
    auto config = makeConfig(audio::EqualizerBandMode::Band31, 0.0F, kAboveRange);
    config.bandGainsDb[30] = kAboveRange;
    expectRejected(config);
  }
  SUBCASE("band gain below -15 dB") {
    auto config = makeConfig(audio::EqualizerBandMode::Band31, 0.0F, kBelowRange);
    config.bandGainsDb[15] = kBelowRange;
    expectRejected(config);
  }
  SUBCASE("band gain is NaN") {
    auto config = makeConfig(audio::EqualizerBandMode::Band31, 0.0F, 0.0F);
    config.bandGainsDb[5] = nan;
    expectRejected(config);
  }
  SUBCASE("band gain is infinity") {
    auto config = makeConfig(audio::EqualizerBandMode::Band10, 0.0F, inf);
    config.bandGainsDb[30] = inf;
    expectRejected(config);
  }
}

TEST_CASE("reducer rejection after an applied config does not bump generation or replace the snapshot") {
  ReducerFixture fixture{};
  const audio::EqualizerConfig applied = makeConfig(audio::EqualizerBandMode::Band31, -1.5F, 0.0F);
  REQUIRE(fixture.submit(eqCommand(applied)).result.accepted);
  CHECK(fixture.reducer.equalizerState().generation == 1U);

  const auto before = fixture.reducer.equalizerState();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  auto expectRejected = [&](const audio::EqualizerConfig& config) {
    const auto reduction = fixture.submit(eqCommand(config));
    CHECK_FALSE(reduction.result.accepted);
    CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK_FALSE(reduction.equalizerStateChanged);
    CHECK(fixture.reducer.equalizerState().generation == 1U);
    CHECK(sameEqualizerSnapshot(fixture.reducer.equalizerState(), before));
  };

  SUBCASE("missing payload") {
    const auto reduction = fixture.submit(eqCommandWithoutConfig());
    CHECK_FALSE(reduction.result.accepted);
    CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(fixture.reducer.equalizerState().generation == 1U);
    CHECK(sameEqualizerSnapshot(fixture.reducer.equalizerState(), before));
  }
  SUBCASE("invalid mode") {
    auto config = makeConfig(audio::EqualizerBandMode::Band10, 0.0F, 0.0F);
    config.mode = static_cast<audio::EqualizerBandMode>(42);
    expectRejected(config);
  }
  SUBCASE("non-finite gains") {
    auto config = makeConfig(audio::EqualizerBandMode::Band31, nan, 0.0F);
    config.bandGainsDb[0] = inf;
    expectRejected(config);
  }
  SUBCASE("out-of-range gains") {
    auto config = makeConfig(audio::EqualizerBandMode::Band31, 0.0F, 15.5F);
    config.bandGainsDb[29] = -15.5F;
    expectRejected(config);
  }
}

TEST_CASE("reducer applies a valid SetEqualizerConfig with one intent and a generation-1 snapshot") {
  ReducerFixture fixture{};
  audio::EqualizerConfig config{};
  config.enabled = true;
  config.mode = audio::EqualizerBandMode::Band31;
  config.preGainDb = -2.0F;
  config.bandGainsDb[0] = 3.5F;
  config.bandGainsDb[30] = -4.0F;
  config.limiterEnabled = true;

  const auto reduction = fixture.submit(eqCommand(config));

  CHECK(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::None);
  CHECK_FALSE(reduction.playerStateChanged);
  CHECK(reduction.equalizerStateChanged);
  CHECK(reduction.notifications.empty());
  // 单意图转发，payload 与提交一致；无 LoadTrack/Seek/Play 等重载尾意图。
  REQUIRE(reduction.intents.size() == 1U);
  CHECK(reduction.intents[0].kind == ControlIntentKind::SetEqualizerConfig);
  REQUIRE(reduction.intents[0].equalizerConfig.has_value());
  CHECK(sameConfig(*reduction.intents[0].equalizerConfig, config));

  // 快照：generation 0→1、config 匹配、sampleRate 仍 0（reducer 无采样率知识，全轴曲线）。
  const auto& snapshot = fixture.reducer.equalizerState();
  CHECK(snapshot.generation == 1U);
  CHECK(snapshot.sampleRate == 0U);
  CHECK(sameConfig(snapshot.config, config));
  CHECK(snapshot.curveFrequenciesHz.front() == 20.0F);
  CHECK(snapshot.curveFrequenciesHz.back() == 20000.0F);

  // 播放快照不受均衡器配置影响（纯均衡参数路径）。
  const auto& player = fixture.reducer.playerState();
  CHECK(player.currentTrack.has_value() == false);
  CHECK(player.playback.state == PlaybackStatus::Stopped);

  // 再次生效 → generation 2 且快照被替换为新 config。
  const audio::EqualizerConfig second = makeConfig(audio::EqualizerBandMode::Band10, 1.0F, 0.0F);
  const auto secondReduction = fixture.submit(eqCommand(second));
  CHECK(secondReduction.result.accepted);
  CHECK(secondReduction.equalizerStateChanged);
  const auto& secondSnapshot = fixture.reducer.equalizerState();
  CHECK(secondSnapshot.generation == 2U);
  CHECK(sameConfig(secondSnapshot.config, second));
}

TEST_CASE("reducer computes a flat 0 dB curve when every gain is zero") {
  ReducerFixture fixture{};

  SUBCASE("Band10 zeroed configuration") {
    const auto config = makeConfig(audio::EqualizerBandMode::Band10, 0.0F, 0.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    for (const float value : fixture.reducer.equalizerState().curvePointsDb) {
      CHECK(closeTo(value, 0.0F, 1e-9F));
    }
  }
  SUBCASE("Band31 zeroed configuration") {
    const auto config = makeConfig(audio::EqualizerBandMode::Band31, 0.0F, 0.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    for (const float value : fixture.reducer.equalizerState().curvePointsDb) {
      CHECK(closeTo(value, 0.0F, 1e-9F));
    }
  }
}

TEST_CASE("reducer curve response peaks at the boosted band center plateau") {
  ReducerFixture fixture{};

  SUBCASE("Band31 band 17 center 1 kHz is within 0.1 dB of +15 dB at the neighbouring grid point") {
    const auto config = singleBandConfig(audio::EqualizerBandMode::Band31, 17U, 15.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    const auto& snapshot = fixture.reducer.equalizerState();
    // 曲线网格点在 1000 Hz 两侧 0.4% 内（平坦峰顶，非裙边）。
    const std::size_t sample = gridIndexNearestHz(snapshot, 1000.0);
    CHECK(closeTo(snapshot.curvePointsDb[sample], 15.0F, 0.1F));
    CHECK(closeTo(snapshot.curveFrequenciesHz[sample], 1000.0F, 10.0F));
  }
  SUBCASE("Band31 band 0 center 20 Hz coincides with grid point 0") {
    const auto config = singleBandConfig(audio::EqualizerBandMode::Band31, 0U, 15.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    const auto& snapshot = fixture.reducer.equalizerState();
    CHECK(snapshot.curveFrequenciesHz[0] == 20.0F);
    CHECK(closeTo(snapshot.curvePointsDb[0], 15.0F, 0.1F));
    // 远离提升频段处仍回到 0dB（20 kHz 与 20 Hz 相隔 3 个十倍频程）。
    CHECK(closeTo(snapshot.curvePointsDb[180], 0.0F, 0.1F));
  }
  SUBCASE("Band31 band 30 center 20 kHz coincides with grid point 180") {
    const auto config = singleBandConfig(audio::EqualizerBandMode::Band31, 30U, 15.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    const auto& snapshot = fixture.reducer.equalizerState();
    CHECK(snapshot.curveFrequenciesHz[180] == 20000.0F);
    CHECK(closeTo(snapshot.curvePointsDb[180], 15.0F, 0.1F));
  }
}

TEST_CASE("reducer curve honours the active band prefix per mode") {
  ReducerFixture fixture{};

  SUBCASE("Band10 ignores band gains beyond the 10-band prefix") {
    // 630 Hz（31 段表 index 15）在 Band10 下不活跃：整条曲线保持平直 0dB。
    const auto config = singleBandConfig(audio::EqualizerBandMode::Band10, 15U, 15.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    for (const float value : fixture.reducer.equalizerState().curvePointsDb) {
      CHECK(closeTo(value, 0.0F, 1e-9F));
    }
  }
  SUBCASE("Band10 applies a boost inside the 10-band prefix") {
    // 31.5 Hz（index 0）是 Band10 活跃首档：最近网格点（≈31.7 Hz）应 ≈ +15 dB。
    const auto config = singleBandConfig(audio::EqualizerBandMode::Band10, 0U, 15.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    const auto& snapshot = fixture.reducer.equalizerState();
    const std::size_t sample = gridIndexNearestHz(snapshot, audio::kEqualizer10BandCenterHz[0]);
    CHECK(closeTo(snapshot.curvePointsDb[sample], 15.0F, 0.1F));
  }
  SUBCASE("Band31 applies the same index that Band10 ignored") {
    // 630 Hz（index 15）在 Band31 下活跃：最近网格点（≈632 Hz，0.4% 偏心率）≈ +15 dB。
    const auto config = singleBandConfig(audio::EqualizerBandMode::Band31, 15U, 15.0F);
    REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
    const auto& snapshot = fixture.reducer.equalizerState();
    const std::size_t sample = gridIndexNearestHz(snapshot, audio::kEqualizer31BandCenterHz[15]);
    CHECK(closeTo(snapshot.curvePointsDb[sample], 15.0F, 0.1F));
  }
}

TEST_CASE("reducer keeps the full-axis curve when sample rate is unset (fs=0 truncation guard inactive)") {
  ReducerFixture fixture{};
  // 20 kHz 档 +15 dB：reducer 路径 sampleRate 恒 0 → 无 2×Nyquist 越界截断，
  // 20000 Hz 端点必须保留计算值（≈+15 dB）而非被守卫压到 0 dB。
  const auto config = singleBandConfig(audio::EqualizerBandMode::Band31, 30U, 15.0F);
  REQUIRE(fixture.submit(eqCommand(config)).result.accepted);
  const auto& snapshot = fixture.reducer.equalizerState();
  CHECK(snapshot.sampleRate == 0U);
  CHECK(snapshot.curveFrequenciesHz.back() == 20000.0F);
  CHECK(closeTo(snapshot.curvePointsDb.back(), 15.0F, 0.1F));
}

// —— controller 订阅级：订阅即推 / reject 无推 / 生效推 ——

TEST_CASE("controller equalizer subscription pushes the current generation-0 snapshot on subscribe") {
  ControllerFixture fixture{};
  control_test::ValueCollector<audio::EqualizerStateSnapshot> snapshots{};
  auto subscription = fixture.controller->subscribeEqualizerState([&](audio::EqualizerStateSnapshot snapshot) {
    snapshots.push(std::move(snapshot));
  });

  // runInlineForTests：订阅即同步回调一次当前快照（从未生效 = gen0 空快照）。
  REQUIRE(snapshots.count() == 1U);
  CHECK(snapshots.last().generation == 0U);
  CHECK(snapshots.last().sampleRate == 0U);
  CHECK(isFactoryDefaultConfig(snapshots.last().config));

  subscription.unsubscribe();
}

TEST_CASE("controller pushes generation+1 equalizer snapshots after accepted commands and none for rejections") {
  ControllerFixture fixture{};
  control_test::ValueCollector<audio::EqualizerStateSnapshot> eqSnapshots{};
  auto eqSubscription = fixture.controller->subscribeEqualizerState([&](audio::EqualizerStateSnapshot snapshot) {
    eqSnapshots.push(std::move(snapshot));
  });
  // 播放快照订阅计数：均衡器配置生效不得扰动播放状态发布面（无回归哨兵）。
  control_test::ValueCollector<PlayerStateSnapshot> playerSnapshots{};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot snapshot) {
    playerSnapshots.push(std::move(snapshot));
  });
  fixture.controller->start();
  REQUIRE(eqSnapshots.count() == 1U);  // 初始 gen0

  // reject 命令（载荷缺失）：无均衡器快照推送、播放快照也保持仅初始一次。
  const auto rejected = fixture.controller->submitCommand(eqCommandWithoutConfig());
  CHECK_FALSE(rejected.accepted);
  CHECK(rejected.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(eqSnapshots.count() == 1U);
  CHECK(playerSnapshots.count() == 1U);

  // 合法配置：回调收到 gen+1 快照且 config 匹配（reducer 生效路径发布）。
  const audio::EqualizerConfig config = makeConfig(audio::EqualizerBandMode::Band31, 0.0F, 0.0F);
  const auto accepted = fixture.controller->submitCommand(eqCommand(config));
  CHECK(accepted.accepted);
  CHECK(accepted.code == MediaControllerErrorCode::None);
  REQUIRE(eqSnapshots.count() == 2U);
  CHECK(eqSnapshots.values()[0].generation == 0U);
  const auto& applied = eqSnapshots.last();
  CHECK(applied.generation == 1U);
  CHECK(applied.sampleRate == 0U);  // fake 音频服务不回填实际输出率（任务 19 B2 生效路径职责）
  CHECK(sameConfig(applied.config, config));
  // 生效快照曲线与频率轴来自 reducer 快照（全 0dB 平直 + 端点精确）。
  for (const float value : applied.curvePointsDb) {
    CHECK(closeTo(value, 0.0F, 1e-6F));
  }
  CHECK(applied.curveFrequenciesHz.front() == 20.0F);
  CHECK(applied.curveFrequenciesHz.back() == 20000.0F);
  CHECK(playerSnapshots.count() == 1U);  // 均衡器配置不触碰播放状态

  // 第二次合法配置 → gen2 推送。
  const audio::EqualizerConfig second = singleBandConfig(audio::EqualizerBandMode::Band10, 0U, 6.0F);
  REQUIRE(fixture.controller->submitCommand(eqCommand(second)).accepted);
  REQUIRE(eqSnapshots.count() == 3U);
  CHECK(eqSnapshots.last().generation == 2U);
  CHECK(sameConfig(eqSnapshots.last().config, second));

  eqSubscription.unsubscribe();
  playerSubscription.unsubscribe();
}

TEST_CASE("controller spectrum subscription starts empty at generation 0 and is untouched by equalizer commands") {
  ControllerFixture fixture{};
  control_test::ValueCollector<audio::SpectrumSnapshot> spectrum{};
  auto subscription = fixture.controller->subscribeSpectrum([&](audio::SpectrumSnapshot snapshot) {
    spectrum.push(std::move(snapshot));
  });
  fixture.controller->start();

  // 频谱分析未接入（FFT 未生效）：订阅即推空快照 gen0，60 bin 全 0。
  REQUIRE(spectrum.count() == 1U);
  CHECK(spectrum.last().generation == 0U);
  CHECK(spectrum.last().sampleRate == 0U);
  CHECK(spectrum.last().timestampMs == 0U);
  for (const float bin : spectrum.last().binsDb) {
    CHECK(bin == 0.0F);
  }

  // 均衡器命令（合法 + 非法）都不应产生频谱更新。
  const auto accepted = fixture.controller->submitCommand(eqCommand(makeConfig(audio::EqualizerBandMode::Band10, 0.0F, 0.0F)));
  CHECK(accepted.accepted);
  const auto rejected = fixture.controller->submitCommand(eqCommandWithoutConfig());
  CHECK_FALSE(rejected.accepted);
  CHECK(spectrum.count() == 1U);
  CHECK(spectrum.last().generation == 0U);

  subscription.unsubscribe();
}

TEST_CASE("controller equalizer subscription after a later commit first receives the current snapshot") {
  ControllerFixture fixture{};
  fixture.controller->start();

  const audio::EqualizerConfig config = makeConfig(audio::EqualizerBandMode::Band31, -3.0F, 0.0F);
  REQUIRE(fixture.controller->submitCommand(eqCommand(config)).accepted);

  // 中途订阅：立即收到当前生效快照（gen1，config 匹配），之后继续增量推送。
  control_test::ValueCollector<audio::EqualizerStateSnapshot> snapshots{};
  auto subscription = fixture.controller->subscribeEqualizerState([&](audio::EqualizerStateSnapshot snapshot) {
    snapshots.push(std::move(snapshot));
  });
  REQUIRE(snapshots.count() == 1U);
  CHECK(snapshots.last().generation == 1U);
  CHECK(sameConfig(snapshots.last().config, config));

  const audio::EqualizerConfig second = singleBandConfig(audio::EqualizerBandMode::Band31, 0U, 8.0F);
  REQUIRE(fixture.controller->submitCommand(eqCommand(second)).accepted);
  REQUIRE(snapshots.count() == 2U);
  CHECK(snapshots.last().generation == 2U);
  CHECK(sameConfig(snapshots.last().config, second));

  subscription.unsubscribe();
}
