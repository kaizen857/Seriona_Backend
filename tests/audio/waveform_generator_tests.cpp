#include <doctest.h>

#include "seriona/audio/waveform_generator.h"

#include "../../src/audio/waveform_internal.h"
#include "../../src/audio/waveform_ffmpeg.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace seriona::audio;
using namespace seriona::audio::detail;

using PlannedBuildAudioWaveform = std::vector<int> (*)(const std::string&,
                                                       int,
                                                       int,
                                                       int&,
                                                       int,
                                                       std::int64_t,
                                                       std::int64_t,
                                                       const WaveformConfig&);

constexpr std::uint32_t kWaveformProbeSampleRate = 48'000;
constexpr std::uint16_t kWaveformProbeChannels = 1;
constexpr std::uint16_t kWaveformProbeStereoChannels = 2;
constexpr std::uint16_t kWaveformProbeBitsPerSample = 16;
constexpr double kWaveformProbePi = 3.141592653589793238462643383279502884;
constexpr int kWaveformProbeId3v1TagSize = 128;
constexpr int kWaveformPerfDurationSeconds = 180;
#if defined(NDEBUG)
constexpr bool kWaveformPerfHardGate = true;
#else
constexpr bool kWaveformPerfHardGate = false;
#endif
constexpr int kWaveformPerfSampleRate = 44'100;
constexpr int kWaveformPerfChannels = 1;
constexpr int kWaveformPerfBarCount = 400;
constexpr int kWaveformPerfTotalWidth = 800;
constexpr int kWaveformPerfMaxHeight = 64;

struct WaveformPerfFixture {
  std::string format;
  std::filesystem::path path;
  int hardLimitMs{0};
};

struct WaveformPerfRun {
  std::string format;
  std::string mode;
  int durationSeconds{0};
  int sampleRate{0};
  int channels{0};
  int configuredThreadCount{0};
  bool simdEnabled{true};
  std::uint32_t cpuCoreCount{0};
  bool avx2Detected{false};
  long long elapsedMs{0};
  int hardLimitMs{0};
  bool hardGate{true};
};

void writeWaveformProbeU16(std::ofstream& stream, std::uint16_t value) {
  const auto bytes = std::array<unsigned char, 2>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeWaveformProbeU32(std::ofstream& stream, std::uint32_t value) {
  const auto bytes = std::array<unsigned char, 4>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
      static_cast<unsigned char>((value >> 16U) & 0xFFU),
      static_cast<unsigned char>((value >> 24U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeWaveformProbeTag(std::ofstream& stream, const char tag[4]) { stream.write(tag, 4); }

std::vector<std::int16_t> makeWaveformProbeSine(std::uint32_t frames) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kWaveformProbeChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kWaveformProbePi * 440.0 * static_cast<double>(frame)) /
                         static_cast<double>(kWaveformProbeSampleRate);
    samples.push_back(static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0)));
  }

  return samples;
}

std::vector<std::int16_t> makeWaveformProbeSilence(std::uint32_t frames, std::uint16_t channels) {
  return std::vector<std::int16_t>(frames * channels, 0);
}

std::vector<std::int16_t> makeWaveformProbeImpulse(std::uint32_t frames,
                                                   std::uint32_t impulseFrame,
                                                   std::uint16_t channels) {
  auto samples = makeWaveformProbeSilence(frames, channels);
  if (impulseFrame < frames) {
    for (std::uint16_t channel = 0; channel < channels; ++channel) {
      samples[(impulseFrame * channels) + channel] = 32767;
    }
  }

  return samples;
}

std::vector<std::int16_t> makeWaveformProbeStereoSine(std::uint32_t frames, bool leftChannel) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kWaveformProbeStereoChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kWaveformProbePi * 440.0 * static_cast<double>(frame)) /
                         static_cast<double>(kWaveformProbeSampleRate);
    const auto value = static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0));
    samples.push_back(leftChannel ? value : 0);
    samples.push_back(leftChannel ? 0 : value);
  }

  return samples;
}

std::int64_t waveformProbeDurationUs(std::uint32_t frames) {
  return (static_cast<std::int64_t>(frames) * 1'000'000) / kWaveformProbeSampleRate;
}

std::filesystem::path waveformProbeFixtureDir() {
#ifdef SERIONA_WAVEFORM_FIXTURE_DIR
  const auto root = std::filesystem::path{SERIONA_WAVEFORM_FIXTURE_DIR};
#else
  const auto root = std::filesystem::temp_directory_path() / "seriona_waveform_generator_tests";
#endif
  std::filesystem::create_directories(root);
  return root;
}

void writeWaveformProbeWav(const std::filesystem::path& path,
                           const std::vector<std::int16_t>& samples,
                           std::uint16_t channels = kWaveformProbeChannels) {
  std::filesystem::create_directories(path.parent_path());
  const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());

  writeWaveformProbeTag(output, "RIFF");
  writeWaveformProbeU32(output, 36U + dataSize);
  writeWaveformProbeTag(output, "WAVE");
  writeWaveformProbeTag(output, "fmt ");
  writeWaveformProbeU32(output, 16U);
  writeWaveformProbeU16(output, 1U);
  writeWaveformProbeU16(output, channels);
  writeWaveformProbeU32(output, kWaveformProbeSampleRate);
  writeWaveformProbeU32(output, kWaveformProbeSampleRate * channels * (kWaveformProbeBitsPerSample / 8U));
  writeWaveformProbeU16(output, static_cast<std::uint16_t>(channels * (kWaveformProbeBitsPerSample / 8U)));
  writeWaveformProbeU16(output, kWaveformProbeBitsPerSample);
  writeWaveformProbeTag(output, "data");
  writeWaveformProbeU32(output, dataSize);

  for (const auto sample : samples) {
    writeWaveformProbeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
}

std::filesystem::path waveformProbeSineFixture(std::string name, std::uint32_t frames) {
  const auto path = waveformProbeFixtureDir() / std::move(name);
  writeWaveformProbeWav(path, makeWaveformProbeSine(frames));
  return path;
}

std::filesystem::path waveformProbeFixture(std::string name,
                                           const std::vector<std::int16_t>& samples,
                                           std::uint16_t channels) {
  const auto path = waveformProbeFixtureDir() / std::move(name);
  writeWaveformProbeWav(path, samples, channels);
  return path;
}

std::string shellQuote(const std::filesystem::path& path) {
  const auto text = path.string();
#if defined(_WIN32)
  REQUIRE(text.find('"') == std::string::npos);
  return "\"" + text + "\"";
#else
  REQUIRE(text.find('\'') == std::string::npos);
  return "'" + text + "'";
#endif
}

std::string shellQuoteText(const std::string& text) {
#if defined(_WIN32)
  REQUIRE(text.find('"') == std::string::npos);
  return "\"" + text + "\"";
#else
  REQUIRE(text.find('\'') == std::string::npos);
  return "'" + text + "'";
#endif
}

void requireWaveformFfmpegCommand(const std::string& command) {
  const int exitCode = std::system(command.c_str());
  REQUIRE_MESSAGE(exitCode == 0, "ffmpeg command failed with exit=" << exitCode << ": " << command);
}

void transcodeWaveformProbeMp3(const std::filesystem::path& sourceWav,
                               const std::filesystem::path& outputMp3,
                               const char* bitrate = "128k") {
  const auto command = std::string{"ffmpeg -v error -nostdin -y -i "} + shellQuote(sourceWav) +
                       " -map_metadata -1 -id3v2_version 0 -write_id3v1 0 -codec:a libmp3lame -b:a " + bitrate + " " +
                       shellQuote(outputMp3);

  requireWaveformFfmpegCommand(command);
  REQUIRE(std::filesystem::exists(outputMp3));
}

void transcodeWaveformProbeFlac(const std::filesystem::path& sourceWav, const std::filesystem::path& outputFlac) {
  const auto command = std::string{"ffmpeg -v error -nostdin -y -i "} + shellQuote(sourceWav) +
                       " -map_metadata -1 -codec:a flac " + shellQuote(outputFlac);

  requireWaveformFfmpegCommand(command);
  REQUIRE(std::filesystem::exists(outputFlac));
}

void transcodeWaveformProbeM4a(const std::filesystem::path& sourceWav, const std::filesystem::path& outputM4a) {
  const auto command = std::string{"ffmpeg -v error -nostdin -y -i "} + shellQuote(sourceWav) +
                       " -map_metadata -1 -codec:a aac -b:a 128k " + shellQuote(outputM4a);

  requireWaveformFfmpegCommand(command);
  REQUIRE(std::filesystem::exists(outputM4a));
}

void transcodeWaveformProbeMp4(const std::filesystem::path& sourceWav, const std::filesystem::path& outputMp4) {
  const auto command = std::string{"ffmpeg -v error -nostdin -y -i "} + shellQuote(sourceWav) +
                       " -map_metadata -1 -codec:a aac -b:a 128k -f mp4 " + shellQuote(outputMp4);

  requireWaveformFfmpegCommand(command);
  REQUIRE(std::filesystem::exists(outputMp4));
}

void generateWaveformPerfSourceWav(const std::filesystem::path& outputWav) {
  std::filesystem::create_directories(outputWav.parent_path());
  const auto sine = std::string{"sine=frequency=440:sample_rate="} + std::to_string(kWaveformPerfSampleRate) +
                    ":duration=" + std::to_string(kWaveformPerfDurationSeconds);
  const auto noise = std::string{"anoisesrc=sample_rate="} + std::to_string(kWaveformPerfSampleRate) +
                     ":duration=" + std::to_string(kWaveformPerfDurationSeconds) + ":amplitude=0.015:seed=12345";
  const auto filter = std::string{"[0:a][1:a]amix=inputs=2:normalize=0,volume=0.7,"} +
                      "aformat=sample_fmts=s16:sample_rates=" + std::to_string(kWaveformPerfSampleRate) +
                      ":channel_layouts=mono";
  const auto command = std::string{"ffmpeg -v error -nostdin -y -f lavfi -i "} + shellQuoteText(sine) +
                       " -f lavfi -i " + shellQuoteText(noise) + " -filter_complex " + shellQuoteText(filter) +
                       " -codec:a pcm_s16le " + shellQuote(outputWav);

  requireWaveformFfmpegCommand(command);
  REQUIRE(std::filesystem::exists(outputWav));
}

void copyWaveformProbeWithId3v1Tag(const std::filesystem::path& sourceMp3,
                                   const std::filesystem::path& taggedMp3) {
  std::filesystem::copy_file(sourceMp3, taggedMp3, std::filesystem::copy_options::overwrite_existing);

  auto tag = std::array<unsigned char, kWaveformProbeId3v1TagSize>{};
  tag[0] = 'T';
  tag[1] = 'A';
  tag[2] = 'G';

  std::ofstream output(taggedMp3, std::ios::binary | std::ios::app);
  REQUIRE(output.good());
  output.write(reinterpret_cast<const char*>(tag.data()), static_cast<std::streamsize>(tag.size()));
  REQUIRE(output.good());
}

WaveformPacketPtr makeSyntheticTerminalTagPacket(int streamIndex, std::uintmax_t fileSize) {
  constexpr int packetSize = 256;
  REQUIRE(fileSize >= static_cast<std::uintmax_t>(packetSize));

  WaveformPacketPtr packet{av_packet_alloc()};
  REQUIRE(packet != nullptr);
  REQUIRE(av_new_packet(packet.get(), packetSize) == 0);
  std::fill(packet->data, packet->data + packet->size, static_cast<std::uint8_t>(0x2A));

  auto* tail = packet->data + packet->size - kWaveformProbeId3v1TagSize;
  tail[0] = 'T';
  tail[1] = 'A';
  tail[2] = 'G';

  packet->stream_index = streamIndex;
  packet->pos = static_cast<std::int64_t>(fileSize - static_cast<std::uintmax_t>(packet->size));
  return packet;
}

void writeWaveformProbeWavWithTerminalTagBytes(const std::filesystem::path& path) {
  constexpr auto frames = std::uint32_t{1024};
  auto samples = makeWaveformProbeSilence(frames, kWaveformProbeChannels);
  const auto tailStart = samples.size() - (kWaveformProbeId3v1TagSize / sizeof(std::int16_t));
  samples[tailStart] = static_cast<std::int16_t>(0x4154);
  samples[tailStart + 1U] = static_cast<std::int16_t>(0x0047);
  writeWaveformProbeWav(path, samples, kWaveformProbeChannels);
}

std::array<unsigned char, kWaveformProbeId3v1TagSize> readWaveformProbeTail(const std::filesystem::path& path) {
  auto tail = std::array<unsigned char, kWaveformProbeId3v1TagSize>{};
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.good());
  input.seekg(-static_cast<std::streamoff>(tail.size()), std::ios::end);
  input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
  REQUIRE(input.gcount() == static_cast<std::streamsize>(tail.size()));
  return tail;
}

std::vector<int> scalarHeights(const std::filesystem::path& path,
                               int barCount,
                               std::int64_t startTimeUS,
                               std::int64_t endTimeUS,
                               std::int64_t durationUS) {
  const auto range = normalizeTimeRange(startTimeUS, endTimeUS, durationUS);
  return mapBarsToHeights(buildScalarWaveformBars(path, barCount, range), 100, WaveformConfig{});
}

std::vector<int> scalarHeightsForFrames(const std::filesystem::path& path, int barCount, std::uint32_t frames) {
  return scalarHeights(path, barCount, 0, 0, waveformProbeDurationUs(frames));
}

std::vector<BarData> mergeStrategyAChunks(const std::filesystem::path& path,
                                          int barCount,
                                          const WaveformTimeRange& range,
                                          int chunkCount) {
  auto merged = std::vector<BarData>(static_cast<std::size_t>(barCount));
  for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
    const auto chunk = processAudioChunkStrategyA(StrategyAChunkRequest{
        .filepath = path,
        .barCount = barCount,
        .timeRange = range,
        .chunkIndex = chunkIndex,
        .chunkCount = chunkCount,
        .config = WaveformConfig{},
    });
    REQUIRE(chunk.size() == merged.size());
    for (std::size_t index = 0; index < merged.size(); ++index) {
      merged[index].sumSquares += chunk[index].sumSquares;
      merged[index].actualCount += chunk[index].actualCount;
    }
  }

  return merged;
}

int maxHeightDelta(const std::vector<int>& left, const std::vector<int>& right) {
  REQUIRE(left.size() == right.size());
  int delta = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    delta = std::max(delta, std::abs(left[index] - right[index]));
  }
  return delta;
}

double barRms(const BarData& bar) {
  if (bar.actualCount == 0) {
    return 0.0;
  }
  return std::sqrt(std::max(0.0, bar.sumSquares / static_cast<double>(bar.actualCount)));
}

double maxRmsRelativeDelta(const std::vector<BarData>& left, const std::vector<BarData>& right) {
  REQUIRE(left.size() == right.size());
  double delta = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const double leftRms = barRms(left[index]);
    const double rightRms = barRms(right[index]);
    const double scale = std::max({1.0e-12, std::abs(leftRms), std::abs(rightRms)});
    delta = std::max(delta, std::abs(leftRms - rightRms) / scale);
  }
  return delta;
}

void requireStrategyAMatchesScalar(const std::filesystem::path& path,
                                   int barCount,
                                   std::int64_t startTimeUS,
                                   std::int64_t endTimeUS,
                                   std::int64_t durationUS,
                                   int chunkCount) {
  const auto range = normalizeTimeRange(startTimeUS, endTimeUS, durationUS);
  REQUIRE(range.hasDuration);

  const auto scalarBars = buildScalarWaveformBars(path, barCount, range);
  const auto strategyBars = mergeStrategyAChunks(path, barCount, range, chunkCount);
  const auto scalar = mapBarsToHeights(scalarBars, 100, WaveformConfig{});
  const auto strategy = mapBarsToHeights(strategyBars, 100, WaveformConfig{});
  const int delta = maxHeightDelta(strategy, scalar);
  const double rmsDelta = maxRmsRelativeDelta(strategyBars, scalarBars);

  std::cout << "strategy_a evidence: file=" << path.filename().string() << " bars=" << barCount
            << " chunks=" << chunkCount << " max_height_delta=" << delta
            << " max_rms_relative_delta=" << rmsDelta << '\n';

  CHECK((delta <= 1 || rmsDelta <= 0.01));
}

void requireStrategyBMatchesScalar(const std::filesystem::path& path,
                                   int barCount,
                                   std::int64_t startTimeUS,
                                   std::int64_t endTimeUS,
                                   std::int64_t durationUS) {
  const auto range = normalizeTimeRange(startTimeUS, endTimeUS, durationUS);
  REQUIRE(range.hasDuration);

  const auto scalarBars = buildScalarWaveformBars(path, barCount, range);
  const auto strategyBars = processPacketBatchStrategyB(StrategyBPacketBatchRequest{
      .filepath = path,
      .barCount = barCount,
      .timeRange = range,
      .config = WaveformConfig{},
  });
  const auto scalar = mapBarsToHeights(scalarBars, 100, WaveformConfig{});
  const auto strategy = mapBarsToHeights(strategyBars, 100, WaveformConfig{});
  const int delta = maxHeightDelta(strategy, scalar);
  const double rmsDelta = maxRmsRelativeDelta(strategyBars, scalarBars);

  std::cout << "strategy_b evidence: file=" << path.filename().string() << " bars=" << barCount
            << " max_height_delta=" << delta << " max_rms_relative_delta=" << rmsDelta << '\n';

  CHECK((delta <= 1 || rmsDelta <= 0.01));
}

void requireStrategyBEmptyBars(const std::vector<BarData>& bars, std::size_t expectedSize) {
  REQUIRE(bars.size() == expectedSize);
  CHECK(std::all_of(bars.begin(), bars.end(), [](const BarData& bar) {
    return bar.actualCount == 0U && bar.sumSquares == 0.0;
  }));
}

void requireBarsClose(const std::vector<BarData>& left, const std::vector<BarData>& right) {
  REQUIRE(left.size() == right.size());
  for (std::size_t index = 0; index < left.size(); ++index) {
    CHECK(left[index].actualCount == right[index].actualCount);
    CHECK(left[index].sumSquares == doctest::Approx(right[index].sumSquares).epsilon(0.01));
  }
}

bool kernelClose(double left, double right) {
  const auto delta = std::abs(left - right);
  const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
  return delta <= (scale * 0.01);
}

struct MonoKernelFixture {
  std::array<const void*, 1> planes{};
  WaveformKernelInput input{};
};

MonoKernelFixture monoKernelInput(const void* samples,
                                  WaveformKernelSampleFormat format,
                                  std::int64_t frameCount,
                                  int decimation) {
  auto fixture = MonoKernelFixture{};
  fixture.planes[0] = samples;
  fixture.input.planes = fixture.planes.data();
  fixture.input.sampleFormat = format;
  fixture.input.layout = WaveformKernelSampleLayout::Interleaved;
  fixture.input.frameCount = frameCount;
  fixture.input.channelCount = 1;
  fixture.input.decimation = decimation;
  return fixture;
}

WaveformKernelInput kernelInputFromFixture(const MonoKernelFixture& fixture) {
  auto input = fixture.input;
  input.planes = fixture.planes.data();
  return input;
}

double expectedPlanarFloatEnergy(const std::vector<float>& left, const std::vector<float>& right, int decimation) {
  double sumSquares = 0.0;
  for (std::size_t frame = 0; frame < left.size(); frame += static_cast<std::size_t>(decimation)) {
    sumSquares += ((static_cast<double>(left[frame]) * static_cast<double>(left[frame])) +
                   (static_cast<double>(right[frame]) * static_cast<double>(right[frame]))) /
                  2.0;
  }
  return sumSquares;
}

double expectedInterleavedInt16Energy(const std::vector<std::int16_t>& samples, int channels, int decimation) {
  double sumSquares = 0.0;
  for (std::size_t frame = 0; frame < samples.size() / static_cast<std::size_t>(channels);
       frame += static_cast<std::size_t>(decimation)) {
    double frameEnergy = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
      const auto sample = static_cast<double>(samples[(frame * static_cast<std::size_t>(channels)) +
                                                      static_cast<std::size_t>(channel)]) /
                          32768.0;
      frameEnergy += sample * sample;
    }
    sumSquares += frameEnergy / static_cast<double>(channels);
  }
  return sumSquares;
}

template <typename Action>
WaveformFfmpegError requireWaveformFfmpegError(Action action) {
  try {
    action();
  } catch (const WaveformFfmpegError& error) {
    return error;
  }

  FAIL("expected WaveformFfmpegError");
  return WaveformFfmpegError{WaveformFfmpegErrorCode::UnsupportedFormat, "expected WaveformFfmpegError", {}};
}

template <typename Action>
std::string requirePublicRuntimeError(Action action) {
  try {
    action();
  } catch (const WaveformFfmpegError& error) {
    FAIL("public buildAudioWaveform leaked private WaveformFfmpegError: " << error.what());
    return error.what();
  } catch (const std::runtime_error& error) {
    return error.what();
  }

  FAIL("expected std::runtime_error");
  return {};
}

std::vector<WaveformPerfFixture> makeWaveformPerfFixtures() {
  const auto root = waveformProbeFixtureDir();
  const auto sourceWav = root / "waveform_perf_180s_sine_noise.wav";
  const auto flac = root / "waveform_perf_180s_sine_noise.flac";
  const auto mp3 = root / "waveform_perf_180s_sine_noise.mp3";
  const auto m4a = root / "waveform_perf_180s_sine_noise.m4a";
  const auto mp4 = root / "waveform_perf_180s_sine_noise.mp4";

  generateWaveformPerfSourceWav(sourceWav);
  transcodeWaveformProbeFlac(sourceWav, flac);
  transcodeWaveformProbeMp3(sourceWav, mp3, "320k");
  transcodeWaveformProbeM4a(sourceWav, m4a);
  transcodeWaveformProbeMp4(sourceWav, mp4);

  return std::vector<WaveformPerfFixture>{
      WaveformPerfFixture{.format = "WAV", .path = sourceWav, .hardLimitMs = 500},
      WaveformPerfFixture{.format = "FLAC", .path = flac, .hardLimitMs = 500},
      WaveformPerfFixture{.format = "MP3", .path = mp3, .hardLimitMs = 500},
      WaveformPerfFixture{.format = "M4A", .path = m4a, .hardLimitMs = 1000},
      WaveformPerfFixture{.format = "MP4", .path = mp4, .hardLimitMs = 1000},
  };
}

WaveformPerfRun measureWaveformPerfRun(const WaveformPerfFixture& fixture,
                                       const WaveformConfig& config,
                                       std::string mode,
                                       bool hardGate) {
  {
    auto input = openWaveformInput(fixture.path);
    auto& stream = findBestAudioStream(*input);
    const auto duration = streamDurationUs(*input, stream);
    CHECK(sampleRate(*stream.codecpar) == kWaveformPerfSampleRate);
    CHECK(channelCount(*stream.codecpar) == kWaveformPerfChannels);
    CHECK(duration >= static_cast<std::int64_t>(kWaveformPerfDurationSeconds - 2) * 1'000'000);
    CHECK(duration <= static_cast<std::int64_t>(kWaveformPerfDurationSeconds + 2) * 1'000'000);
  }

  int barWidth = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto heights = buildAudioWaveform(fixture.path.string(),
                                          kWaveformPerfBarCount,
                                          kWaveformPerfTotalWidth,
                                          barWidth,
                                          kWaveformPerfMaxHeight,
                                          0,
                                          0,
                                          config);
  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();

  REQUIRE(heights.size() == static_cast<std::size_t>(kWaveformPerfBarCount));
  CHECK(barWidth >= 1);
  CHECK(std::all_of(heights.begin(), heights.end(), [](int height) {
    return height >= 2 && height <= kWaveformPerfMaxHeight;
  }));

  const auto nonMinimumBars = std::count_if(heights.begin(), heights.end(), [](int height) { return height > 2; });
  CHECK(nonMinimumBars > (kWaveformPerfBarCount / 2));

  auto run = WaveformPerfRun{.format = fixture.format,
                             .mode = std::move(mode),
                             .durationSeconds = kWaveformPerfDurationSeconds,
                             .sampleRate = kWaveformPerfSampleRate,
                             .channels = kWaveformPerfChannels,
                             .configuredThreadCount = config.threadCount,
                             .simdEnabled = config.enableSIMD,
                             .cpuCoreCount = std::thread::hardware_concurrency(),
                             .avx2Detected = supportsAvx2(),
                             .elapsedMs = elapsed,
                             .hardLimitMs = fixture.hardLimitMs,
                             .hardGate = hardGate};

  std::cout << "waveform perf evidence: format=" << run.format << " mode=" << run.mode
            << " duration_s=" << run.durationSeconds << " sample_rate=" << run.sampleRate
            << " channels=" << run.channels << " thread_count=" << run.configuredThreadCount
            << " simd_enabled=" << (run.simdEnabled ? "true" : "false")
            << " elapsed_ms=" << run.elapsedMs << " cpu_cores=" << run.cpuCoreCount
            << " avx2=" << (run.avx2Detected ? "true" : "false") << " hard_limit_ms=" << run.hardLimitMs
            << " hard_gate=" << (run.hardGate ? "true" : "false") << '\n';

  if (hardGate) {
    CHECK_MESSAGE(run.elapsedMs < run.hardLimitMs,
                  run.format << " waveform perf exceeded hard limit: elapsed=" << run.elapsedMs
                             << "ms limit=" << run.hardLimitMs << "ms");
  }

  return run;
}

std::filesystem::path waveformPerfReportPath() {
  return waveformProbeFixtureDir() / "waveform_perf_report.md";
}

void writeWaveformPerfReport(const std::vector<WaveformPerfRun>& runs) {
  const auto path = waveformPerfReportPath();
  std::ofstream report(path, std::ios::trunc);
  REQUIRE(report.good());

  report << "# Waveform performance report\n\n";
  report << "Manual QA command: `ctest --test-dir build -R 'seriona.audio.waveform.perf' --output-on-failure`\n\n";
  report << "| fixture format | mode | duration s | sample rate Hz | channels | thread count | SIMD enabled | "
            "elapsed ms | hard limit ms | hard gate | CPU core count | AVX2 detected |\n";
  report << "|---|---|---:|---:|---:|---:|---|---:|---:|---|---:|---|\n";
  for (const auto& run : runs) {
    report << "| " << run.format << " | " << run.mode << " | " << run.durationSeconds << " | "
           << run.sampleRate << " | " << run.channels << " | " << run.configuredThreadCount << " | "
           << (run.simdEnabled ? "true" : "false") << " | " << run.elapsedMs << " | " << run.hardLimitMs
           << " | " << (run.hardGate ? "true" : "false") << " | " << run.cpuCoreCount << " | "
           << (run.avx2Detected ? "true" : "false") << " |\n";
  }

  REQUIRE(report.good());
}

}

TEST_CASE("waveform contract public header exposes planned defaults") {
  const auto config = WaveformConfig{};

  CHECK(config.dbFloor == -55.0F);
  CHECK(config.dbCeiling == 0.0F);
  CHECK(config.enableSIMD);
  CHECK(config.threadCount == 0);
}

TEST_CASE("waveform contract build function matches planned signature") {
  static_assert(std::is_same_v<decltype(&buildAudioWaveform), PlannedBuildAudioWaveform>);

  const PlannedBuildAudioWaveform function = &buildAudioWaveform;
  CHECK(function != nullptr);
}

TEST_CASE("waveform basic config remains a value type") {
  static_assert(std::is_copy_constructible_v<WaveformConfig>);
  static_assert(std::is_copy_assignable_v<WaveformConfig>);

  auto config = WaveformConfig{};
  config.threadCount = 4;

  const auto copied = config;
  CHECK(copied.threadCount == 4);
}

TEST_CASE("waveform basic internal result types preserve bar aggregates") {
  const auto chunk = ChunkResult{.bars = {BarData{.sumSquares = 2.5, .actualCount = 5}}};

  CHECK(chunk.bars.size() == 1U);
  CHECK(chunk.bars[0].sumSquares == 2.5);
  CHECK(chunk.bars[0].actualCount == 5U);
}

TEST_CASE("waveform basic shape normalization locks exact invalid and formula behavior") {
  int barWidth = 99;

  CHECK_FALSE(normalizeShape(0, 800, 120, barWidth));
  CHECK(barWidth == 0);

  barWidth = 99;
  CHECK_FALSE(normalizeShape(-4, 800, 120, barWidth));
  CHECK(barWidth == 0);

  barWidth = 99;
  CHECK_FALSE(normalizeShape(4, 0, 120, barWidth));
  CHECK(barWidth == 0);

  barWidth = 99;
  CHECK_FALSE(normalizeShape(4, -20, 120, barWidth));
  CHECK(barWidth == 0);

  barWidth = 99;
  CHECK_FALSE(normalizeShape(4, 800, 0, barWidth));
  CHECK(barWidth == 0);

  barWidth = 99;
  CHECK_FALSE(normalizeShape(4, 800, -2, barWidth));
  CHECK(barWidth == 0);

  CHECK(normalizeShape(8, 4, 120, barWidth));
  CHECK(barWidth == 1);

  CHECK(normalizeShape(400, 800, 120, barWidth));
  CHECK(barWidth == 1);

  CHECK(normalizeShape(4, 40, 120, barWidth));
  CHECK(barWidth == 8);
}

TEST_CASE("waveform basic time normalization clamps exact cue boundaries") {
  const auto negativeStart = normalizeTimeRange(-500, 1'000, 2'000);
  CHECK(negativeStart.startTimeUS == 0);
  CHECK(negativeStart.endTimeUS == 1'000);
  CHECK(negativeStart.hasDuration);

  const auto openEnd = normalizeTimeRange(250, 0, 2'000);
  CHECK(openEnd.startTimeUS == 250);
  CHECK(openEnd.endTimeUS == 2'000);
  CHECK(openEnd.hasDuration);

  const auto beyondDuration = normalizeTimeRange(250, 4'000, 2'000);
  CHECK(beyondDuration.startTimeUS == 250);
  CHECK(beyondDuration.endTimeUS == 2'000);
  CHECK(beyondDuration.hasDuration);

  const auto emptyWindow = normalizeTimeRange(1'500, 1'000, 2'000);
  CHECK(emptyWindow.startTimeUS == 1'500);
  CHECK(emptyWindow.endTimeUS == 1'500);
  CHECK_FALSE(emptyWindow.hasDuration);

  const auto zeroDuration = normalizeTimeRange(0, 0, 0);
  CHECK(zeroDuration.startTimeUS == 0);
  CHECK(zeroDuration.endTimeUS == 0);
  CHECK_FALSE(zeroDuration.hasDuration);
}

TEST_CASE("waveform basic strategy selection maps only complex containers to packet batches") {
  CHECK(selectWaveformStrategy("flac") == WaveformStrategy::SeekChunks);
  CHECK(selectWaveformStrategy("mp3") == WaveformStrategy::SeekChunks);
  CHECK(selectWaveformStrategy("ogg") == WaveformStrategy::SeekChunks);
  CHECK(selectWaveformStrategy("") == WaveformStrategy::SeekChunks);

  CHECK(selectWaveformStrategy("mov") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("mp4") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("m4a") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("3gp") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("3g2") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("mj2") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("matroska") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("webm") == WaveformStrategy::PacketBatches);
  CHECK(selectWaveformStrategy("Matroska,WebM") == WaveformStrategy::PacketBatches);
}

TEST_CASE("waveform basic db mapping distinguishes no samples silence tiny rms and clipping") {
  const auto bars = std::vector<BarData>{
      BarData{.sumSquares = 0.0, .actualCount = 0},
      BarData{.sumSquares = 0.0, .actualCount = 8},
      BarData{.sumSquares = 1.0e-24, .actualCount = 1},
      BarData{.sumSquares = 0.01, .actualCount = 1},
      BarData{.sumSquares = 4.0, .actualCount = 1},
  };

  const auto heights = mapBarsToHeights(bars, 110, WaveformConfig{});
  CHECK(heights == std::vector<int>{0, 2, 2, 70, 110});

  const auto zeroMaxHeight = mapBarsToHeights(bars, 0, WaveformConfig{});
  CHECK(zeroMaxHeight == std::vector<int>{0, 0, 0, 0, 0});
}

TEST_CASE("waveform public generated wav returns requested nonzero vector and formula width") {
  constexpr auto frames = std::uint32_t{48'000};
  constexpr int barCount = 400;
  const auto path = waveformProbeSineFixture("waveform_public_sine.wav", frames);
  auto config = WaveformConfig{};
  config.threadCount = 1;
  int barWidth = 99;

  const auto heights = buildAudioWaveform(path.string(), barCount, 800, barWidth, 100, 0, 0, config);

  REQUIRE(heights.size() == static_cast<std::size_t>(barCount));
  CHECK(barWidth == 1);
  CHECK(std::any_of(heights.begin(), heights.end(), [](int height) { return height > 2; }));
}

TEST_CASE("waveform public barCount at or below zero returns empty vector and zero width") {
  int barWidth = 99;

  const auto empty = buildAudioWaveform((waveformProbeFixtureDir() / "missing_public_zero.wav").string(),
                                       0,
                                       800,
                                       barWidth,
                                       100);

  CHECK(empty.empty());
  CHECK(barWidth == 0);

  barWidth = 99;
  const auto negative = buildAudioWaveform((waveformProbeFixtureDir() / "missing_public_negative.wav").string(),
                                          -4,
                                          800,
                                          barWidth,
                                          100);

  CHECK(negative.empty());
  CHECK(barWidth == 0);
}

TEST_CASE("waveform public invalid dimensions return zero bars and zero width") {
  const auto missingPath = (waveformProbeFixtureDir() / "missing_public_invalid_dimensions.wav").string();
  int barWidth = 99;

  const auto invalidWidth = buildAudioWaveform(missingPath, 5, 0, barWidth, 100);

  CHECK(invalidWidth == std::vector<int>(5U, 0));
  CHECK(barWidth == 0);

  barWidth = 99;
  const auto invalidHeight = buildAudioWaveform(missingPath, 5, 50, barWidth, 0);

  CHECK(invalidHeight == std::vector<int>(5U, 0));
  CHECK(barWidth == 0);
}

TEST_CASE("waveform public empty normalized time range returns zero bars") {
  constexpr auto frames = std::uint32_t{48'000};
  constexpr int barCount = 6;
  const auto path = waveformProbeSineFixture("waveform_public_empty_range.wav", frames);
  int barWidth = 99;

  const auto heights = buildAudioWaveform(path.string(),
                                         barCount,
                                         60,
                                         barWidth,
                                         100,
                                         800'000,
                                         250'000,
                                         WaveformConfig{});

  CHECK(heights == std::vector<int>(static_cast<std::size_t>(barCount), 0));
  CHECK(barWidth == 8);
}

TEST_CASE("waveform public missing file throws runtime_error with path text") {
  const auto path = waveformProbeFixtureDir() / "missing_public_throw.wav";
  int barWidth = 99;

  const auto message = requirePublicRuntimeError([&] {
    static_cast<void>(buildAudioWaveform(path.string(), 4, 40, barWidth, 100));
  });

  CHECK(message.find("missing_public_throw.wav") != std::string::npos);
  CHECK(message.find("failed to build waveform") != std::string::npos);
}

TEST_CASE("waveform public no-audio input throws runtime_error with useful text") {
  const auto path = waveformProbeFixtureDir() / "waveform_public_image_only.pgm";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P5\n2 2\n255\n";
  const auto pixels = std::array<unsigned char, 4>{0, 64, 128, 255};
  output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  output.close();
  int barWidth = 99;

  const auto message = requirePublicRuntimeError([&] {
    static_cast<void>(buildAudioWaveform(path.string(), 4, 40, barWidth, 100));
  });

  CHECK(message.find("waveform_public_image_only.pgm") != std::string::npos);
  CHECK(message.find("audio stream") != std::string::npos);
}

TEST_CASE("waveform public malformed input throws runtime_error with ffmpeg detail") {
  const auto path = waveformProbeFixtureDir() / "waveform_public_not_audio.txt";
  std::ofstream output(path, std::ios::trunc);
  output << "this is not an audio container";
  output.close();
  int barWidth = 99;

  const auto message = requirePublicRuntimeError([&] {
    static_cast<void>(buildAudioWaveform(path.string(), 4, 40, barWidth, 100));
  });

  CHECK(message.find("waveform_public_not_audio.txt") != std::string::npos);
  CHECK(message.find("failed to open audio container") != std::string::npos);
}

TEST_CASE("waveform public format coverage builds wav flac mp3 m4a and mp4 fixtures") {
  constexpr auto frames = std::uint32_t{96'000};
  constexpr int barCount = 32;
  const auto wav = waveformProbeSineFixture("waveform_public_format_source.wav", frames);
  const auto flac = waveformProbeFixtureDir() / "waveform_public_format_source.flac";
  const auto mp3 = waveformProbeFixtureDir() / "waveform_public_format_source.mp3";
  const auto m4a = waveformProbeFixtureDir() / "waveform_public_format_source.m4a";
  const auto mp4 = waveformProbeFixtureDir() / "waveform_public_format_source.mp4";
  transcodeWaveformProbeFlac(wav, flac);
  transcodeWaveformProbeMp3(wav, mp3);
  transcodeWaveformProbeM4a(wav, m4a);
  transcodeWaveformProbeMp4(wav, mp4);

  const auto fixtures = std::vector<WaveformPerfFixture>{
      WaveformPerfFixture{.format = "WAV", .path = wav, .hardLimitMs = 0},
      WaveformPerfFixture{.format = "FLAC", .path = flac, .hardLimitMs = 0},
      WaveformPerfFixture{.format = "MP3", .path = mp3, .hardLimitMs = 0},
      WaveformPerfFixture{.format = "M4A", .path = m4a, .hardLimitMs = 0},
      WaveformPerfFixture{.format = "MP4", .path = mp4, .hardLimitMs = 0},
  };

  for (const auto& fixture : fixtures) {
    int barWidth = 0;
    const auto heights = buildAudioWaveform(fixture.path.string(), barCount, 128, barWidth, 64);

    INFO("format=" << fixture.format << " file=" << fixture.path.filename().string());
    REQUIRE(heights.size() == static_cast<std::size_t>(barCount));
    CHECK(barWidth >= 1);
    CHECK(std::all_of(heights.begin(), heights.end(), [](int height) { return height >= 2 && height <= 64; }));
    CHECK(std::any_of(heights.begin(), heights.end(), [](int height) { return height > 2; }));
  }
}

TEST_CASE("waveform public cue window excludes impulses outside the requested subrange") {
  constexpr auto frames = std::uint32_t{48'000};
  constexpr int barCount = 4;
  auto samples = makeWaveformProbeSilence(frames, kWaveformProbeChannels);
  samples[6'000] = 32767;
  samples[27'000] = 32767;
  samples[42'000] = 32767;
  const auto path = waveformProbeFixture("waveform_public_cue.wav", samples, kWaveformProbeChannels);
  int barWidth = 0;

  const auto heights = buildAudioWaveform(path.string(), barCount, 40, barWidth, 100, 250'000, 750'000, WaveformConfig{});

  REQUIRE(heights.size() == static_cast<std::size_t>(barCount));
  CHECK(barWidth >= 1);
  CHECK(heights[0] == 2);
  CHECK(heights[1] == 2);
  CHECK(heights[2] > 2);
  CHECK(heights[3] == 2);
}

TEST_CASE("waveform ffmpeg input reports missing files with private open error") {
  const auto error = requireWaveformFfmpegError([] {
    static_cast<void>(openWaveformInput(waveformProbeFixtureDir() / "missing.wav"));
  });

  CHECK(error.code() == WaveformFfmpegErrorCode::OpenFailed);
  CHECK(error.detail().find("missing.wav") != std::string::npos);
}

TEST_CASE("waveform ffmpeg input reports invalid text files as unsupported format") {
  const auto path = waveformProbeFixtureDir() / "not_audio.txt";
  std::ofstream output(path, std::ios::trunc);
  output << "this is not an audio container";
  output.close();

  const auto error = requireWaveformFfmpegError([&] { static_cast<void>(openWaveformInput(path)); });

  CHECK(error.code() == WaveformFfmpegErrorCode::UnsupportedFormat);
  CHECK_FALSE(error.detail().empty());
}

TEST_CASE("waveform ffmpeg input reports image-only pgm as missing audio stream") {
  const auto path = waveformProbeFixtureDir() / "image_only.pgm";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P5\n2 2\n255\n";
  const auto pixels = std::array<unsigned char, 4>{0, 64, 128, 255};
  output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  output.close();

  auto input = openWaveformInput(path);
  const auto error = requireWaveformFfmpegError([&] { static_cast<void>(findBestAudioStream(*input)); });

  CHECK(error.code() == WaveformFfmpegErrorCode::AudioStreamNotFound);
  CHECK_FALSE(error.detail().empty());
}

TEST_CASE("waveform ffmpeg probe reads generated wav stream facts and cloned packets") {
  const auto path = waveformProbeSineFixture("waveform_probe_sine.wav", 4096);

  auto input = openWaveformInput(path);
  auto& stream = findBestAudioStream(*input);
  auto decoder = openDecoderForStream(stream);

  REQUIRE(stream.codecpar != nullptr);
  CHECK(sampleRate(*stream.codecpar) == static_cast<int>(kWaveformProbeSampleRate));
  CHECK(sampleRate(*decoder) == static_cast<int>(kWaveformProbeSampleRate));
  CHECK(channelCount(*stream.codecpar) == kWaveformProbeChannels);
  CHECK(channelCount(*decoder) == kWaveformProbeChannels);
  CHECK(sampleFormat(*stream.codecpar) == AV_SAMPLE_FMT_S16);
  CHECK(sampleFormat(*decoder) == AV_SAMPLE_FMT_S16);
  CHECK(streamDurationUs(*input, stream) > 0);
  CHECK(timeBase(stream).den > 0);
  CHECK(formatName(*input).find("wav") != std::string::npos);

  WaveformCodecParametersPtr copiedParameters{avcodec_parameters_alloc()};
  REQUIRE(copiedParameters != nullptr);
  REQUIRE(avcodec_parameters_copy(copiedParameters.get(), stream.codecpar) >= 0);
  CHECK(sampleRate(*copiedParameters) == static_cast<int>(kWaveformProbeSampleRate));
  CHECK(channelCount(*copiedParameters) == kWaveformProbeChannels);

  WaveformPacketPtr packet{av_packet_alloc()};
  REQUIRE(packet != nullptr);
  while (packet->stream_index != stream.index) {
    const int readResult = av_read_frame(input.get(), packet.get());
    REQUIRE(readResult >= 0);
    if (packet->stream_index != stream.index) {
      av_packet_unref(packet.get());
    }
  }

  const int originalSize = packet->size;
  auto cloned = clonePacket(*packet);
  REQUIRE(cloned != nullptr);
  CHECK(cloned->stream_index == packet->stream_index);
  CHECK(cloned->size == originalSize);
  av_packet_unref(packet.get());
  CHECK(cloned->size == originalSize);
  CHECK(cloned->data != nullptr);
}

TEST_CASE("waveform scalar sine fixture produces stable nonzero bars") {
  constexpr auto frames = std::uint32_t{48'000};
  const auto path = waveformProbeSineFixture("waveform_scalar_sine.wav", frames);

  const auto heights = scalarHeightsForFrames(path, 8, frames);

  REQUIRE(heights.size() == 8U);
  const auto [minHeight, maxHeight] = std::minmax_element(heights.begin(), heights.end());
  REQUIRE(minHeight != heights.end());
  CHECK(*minHeight > 2);
  CHECK((*maxHeight - *minHeight) <= 1);
}

TEST_CASE("waveform scalar silence maps sampled bars to minimum visible height") {
  constexpr auto frames = std::uint32_t{48'000};
  const auto path = waveformProbeFixture("waveform_scalar_silence.wav",
                                        makeWaveformProbeSilence(frames, kWaveformProbeChannels),
                                        kWaveformProbeChannels);
  const auto range = normalizeTimeRange(0, 0, waveformProbeDurationUs(frames));

  const auto bars = buildScalarWaveformBars(path, 4, range);
  const auto heights = mapBarsToHeights(bars, 100, WaveformConfig{});

  REQUIRE(bars.size() == 4U);
  CHECK(std::all_of(bars.begin(), bars.end(), [](const BarData& bar) { return bar.actualCount > 0; }));
  CHECK(heights == std::vector<int>{2, 2, 2, 2});
}

TEST_CASE("waveform scalar impulse lands only in the expected bar") {
  constexpr auto frames = std::uint32_t{48'000};
  const auto path = waveformProbeFixture("waveform_scalar_impulse.wav",
                                        makeWaveformProbeImpulse(frames, 30'000, kWaveformProbeChannels),
                                        kWaveformProbeChannels);

  const auto heights = scalarHeightsForFrames(path, 4, frames);

  REQUIRE(heights.size() == 4U);
  CHECK(heights[0] == 2);
  CHECK(heights[1] == 2);
  CHECK(heights[2] > 2);
  CHECK(heights[3] == 2);
}

TEST_CASE("waveform scalar stereo averages left-only and right-only channel energy") {
  constexpr auto frames = std::uint32_t{48'000};
  const auto leftPath = waveformProbeFixture("waveform_scalar_stereo_left.wav",
                                            makeWaveformProbeStereoSine(frames, true),
                                            kWaveformProbeStereoChannels);
  const auto rightPath = waveformProbeFixture("waveform_scalar_stereo_right.wav",
                                             makeWaveformProbeStereoSine(frames, false),
                                             kWaveformProbeStereoChannels);

  const auto leftHeights = scalarHeightsForFrames(leftPath, 8, frames);
  const auto rightHeights = scalarHeightsForFrames(rightPath, 8, frames);

  REQUIRE(leftHeights.size() == 8U);
  REQUIRE(rightHeights.size() == 8U);
  CHECK(leftHeights == rightHeights);
  CHECK(std::all_of(leftHeights.begin(), leftHeights.end(), [](int height) { return height > 2; }));
}

TEST_CASE("waveform scalar short audio leaves bars without samples at zero") {
  const auto path = waveformProbeFixture("waveform_scalar_short.wav",
                                        std::vector<std::int16_t>{12000, 12000, 12000, 12000},
                                        kWaveformProbeChannels);
  const auto range = normalizeTimeRange(0, 0, waveformProbeDurationUs(4));

  const auto bars = buildScalarWaveformBars(path, 8, range);
  const auto heights = mapBarsToHeights(bars, 100, WaveformConfig{});

  REQUIRE(bars.size() == 8U);
  REQUIRE(heights.size() == 8U);
  CHECK(bars[0].actualCount == 1U);
  CHECK(bars[1].actualCount == 0U);
  CHECK(bars[2].actualCount == 1U);
  CHECK(bars[3].actualCount == 0U);
  CHECK(bars[4].actualCount == 1U);
  CHECK(bars[5].actualCount == 0U);
  CHECK(bars[6].actualCount == 1U);
  CHECK(bars[7].actualCount == 0U);
  CHECK(heights[1] == 0);
  CHECK(heights[3] == 0);
  CHECK(heights[5] == 0);
  CHECK(heights[7] == 0);
}

TEST_CASE("waveform scalar cue subrange excludes samples outside the requested window") {
  constexpr auto frames = std::uint32_t{48'000};
  auto samples = makeWaveformProbeSilence(frames, kWaveformProbeChannels);
  samples[6'000] = 32767;
  samples[27'000] = 32767;
  samples[42'000] = 32767;
  const auto path = waveformProbeFixture("waveform_scalar_cue.wav", samples, kWaveformProbeChannels);

  const auto heights = scalarHeights(path, 4, 250'000, 750'000, waveformProbeDurationUs(frames));

  REQUIRE(heights.size() == 4U);
  CHECK(heights[0] == 2);
  CHECK(heights[1] == 2);
  CHECK(heights[2] > 2);
  CHECK(heights[3] == 2);
}

TEST_CASE("waveform scalar invalid container propagates private ffmpeg failure") {
  const auto path = waveformProbeFixtureDir() / "waveform_scalar_not_audio.txt";
  std::ofstream output(path, std::ios::trunc);
  output << "this is not an audio container";
  output.close();
  const auto range = normalizeTimeRange(0, 1'000, 1'000);

  CHECK_THROWS_AS(static_cast<void>(buildScalarWaveformBars(path, 4, range)), WaveformFfmpegError);
}

TEST_CASE("waveform id3v1 strips terminal MP3 tag and preserves scalar waveform parity") {
  constexpr auto frames = std::uint32_t{96'000};
  const auto sourceWav = waveformProbeSineFixture("waveform_id3v1_source.wav", frames);
  const auto untaggedMp3 = waveformProbeFixtureDir() / "waveform_id3v1_untagged.mp3";
  const auto taggedMp3 = waveformProbeFixtureDir() / "waveform_id3v1_tagged.mp3";
  transcodeWaveformProbeMp3(sourceWav, untaggedMp3);
  copyWaveformProbeWithId3v1Tag(untaggedMp3, taggedMp3);

  auto input = openWaveformInput(taggedMp3);
  auto& stream = findBestAudioStream(*input);
  auto terminalPacket = makeSyntheticTerminalTagPacket(stream.index, std::filesystem::file_size(taggedMp3));
  const int originalPacketSize = terminalPacket->size;

  CHECK(stripTrailingId3v1TagIfPresent(*terminalPacket, *input, stream));
  CHECK(terminalPacket->size == originalPacketSize - kWaveformProbeId3v1TagSize);

  const auto range = normalizeTimeRange(0, 0, waveformProbeDurationUs(frames));
  const auto untaggedHeights = mapBarsToHeights(buildScalarWaveformBars(untaggedMp3, 16, range), 100, WaveformConfig{});
  const auto taggedHeights = mapBarsToHeights(buildScalarWaveformBars(taggedMp3, 16, range), 100, WaveformConfig{});

  REQUIRE(untaggedHeights.size() == taggedHeights.size());
  REQUIRE(taggedHeights.size() >= 2U);
  int maxDelta = 0;
  for (std::size_t index = 0; index < taggedHeights.size(); ++index) {
    maxDelta = std::max(maxDelta, std::abs(taggedHeights[index] - untaggedHeights[index]));
  }
  const auto taggedPreviousMax = *std::max_element(taggedHeights.begin(), taggedHeights.end() - 1);
  const int finalDelta = std::abs(taggedHeights.back() - untaggedHeights.back());

  std::cout << "id3v1 mp3 evidence: packet_size_before=" << originalPacketSize
            << " packet_size_after=" << terminalPacket->size << " untagged_final=" << untaggedHeights.back()
            << " tagged_final=" << taggedHeights.back() << " final_delta=" << finalDelta
            << " max_delta=" << maxDelta << '\n';

  CHECK(maxDelta <= 1);
  CHECK(finalDelta <= 1);
  CHECK(taggedHeights.back() <= taggedPreviousMax + 2);
}

TEST_CASE("waveform id3v1 does not strip non-MP3 terminal TAG bytes") {
  const auto path = waveformProbeFixtureDir() / "waveform_id3v1_false_positive.wav";
  writeWaveformProbeWavWithTerminalTagBytes(path);
  const auto fileTail = readWaveformProbeTail(path);
  REQUIRE(fileTail[0] == 'T');
  REQUIRE(fileTail[1] == 'A');
  REQUIRE(fileTail[2] == 'G');

  auto input = openWaveformInput(path);
  auto& stream = findBestAudioStream(*input);
  auto terminalPacket = makeSyntheticTerminalTagPacket(stream.index, std::filesystem::file_size(path));
  const int originalPacketSize = terminalPacket->size;
  const auto originalBytes = std::vector<std::uint8_t>(terminalPacket->data, terminalPacket->data + terminalPacket->size);

  CHECK_FALSE(stripTrailingId3v1TagIfPresent(*terminalPacket, *input, stream));
  CHECK(terminalPacket->size == originalPacketSize);
  CHECK(std::equal(originalBytes.begin(), originalBytes.end(), terminalPacket->data));

  const auto range = normalizeTimeRange(0, 0, waveformProbeDurationUs(1024));
  const auto heights = mapBarsToHeights(buildScalarWaveformBars(path, 4, range), 100, WaveformConfig{});
  REQUIRE(heights.size() == 4U);

  std::cout << "id3v1 non-mp3 evidence: packet_size_before=" << originalPacketSize
            << " packet_size_after=" << terminalPacket->size << " final_height=" << heights.back() << '\n';
}

TEST_CASE("waveform simd decimation and forced scalar dispatch stay deterministic") {
  CHECK(waveformDecimationForSampleRate(0) == 1);
  CHECK(waveformDecimationForSampleRate(44'100) == 1);
  CHECK(waveformDecimationForSampleRate(48'000) == 1);
  CHECK(waveformDecimationForSampleRate(96'000) == 3);
  CHECK(waveformDecimationForSampleRate(192'000) == 6);

  const auto samples = std::vector<float>{0.0F, 0.25F, -0.5F, 0.75F, -1.0F, 0.5F, -0.25F, 1.0F,
                                          0.125F, -0.375F, 0.625F, -0.875F, 0.375F, -0.125F, 0.875F, -0.625F};
  const auto fixture = monoKernelInput(samples.data(), WaveformKernelSampleFormat::Float32, samples.size(), 1);
  const auto input = kernelInputFromFixture(fixture);
  auto config = WaveformConfig{};
  config.enableSIMD = false;

  const auto scalar = computeScalarWaveformKernel(input);
  const auto dispatched = computeWaveformKernel(input, config);

  CHECK(dispatched.backend == WaveformKernelBackend::Scalar);
  CHECK(dispatched.actualCount == scalar.actualCount);
  CHECK(kernelClose(dispatched.sumSquares, scalar.sumSquares));
}

TEST_CASE("waveform simd scalar kernels cover planar interleaved stride and pcm formats") {
  const auto left = std::vector<float>{1.0F, 0.5F, -0.5F, -1.0F, 0.25F, -0.25F};
  const auto right = std::vector<float>{0.0F, -0.5F, 0.5F, 1.0F, -0.25F, 0.25F};
  const auto planarPlanes = std::array<const void*, 2>{left.data(), right.data()};
  auto planarInput = WaveformKernelInput{};
  planarInput.planes = planarPlanes.data();
  planarInput.sampleFormat = WaveformKernelSampleFormat::Float32;
  planarInput.layout = WaveformKernelSampleLayout::Planar;
  planarInput.frameCount = static_cast<std::int64_t>(left.size());
  planarInput.channelCount = 2;
  planarInput.decimation = 2;

  const auto planar = computeScalarWaveformKernel(planarInput);
  CHECK(planar.backend == WaveformKernelBackend::Scalar);
  CHECK(planar.actualCount == 3U);
  CHECK(kernelClose(planar.sumSquares, expectedPlanarFloatEnergy(left, right, 2)));

  const auto interleaved = std::vector<std::int16_t>{32767, 0, -32768, 16384, 8192, -8192, 0, 32767};
  const auto interleavedPlanes = std::array<const void*, 1>{interleaved.data()};
  auto interleavedInput = WaveformKernelInput{};
  interleavedInput.planes = interleavedPlanes.data();
  interleavedInput.sampleFormat = WaveformKernelSampleFormat::Int16;
  interleavedInput.layout = WaveformKernelSampleLayout::Interleaved;
  interleavedInput.frameCount = 4;
  interleavedInput.channelCount = 2;
  interleavedInput.decimation = 1;

  const auto interleavedResult = computeScalarWaveformKernel(interleavedInput);
  CHECK(interleavedResult.actualCount == 4U);
  CHECK(kernelClose(interleavedResult.sumSquares, expectedInterleavedInt16Energy(interleaved, 2, 1)));

  const auto int32Samples = std::vector<std::int32_t>{2'147'483'647, -2'147'483'648, 1'073'741'824, -536'870'912};
  const auto int32Fixture = monoKernelInput(int32Samples.data(), WaveformKernelSampleFormat::Int32, int32Samples.size(), 2);
  const auto int32Input = kernelInputFromFixture(int32Fixture);
  const auto int32Result = computeScalarWaveformKernel(int32Input);
  CHECK(int32Result.actualCount == 2U);
  CHECK(int32Result.sumSquares > 1.0);

  const auto u8Samples = std::vector<std::uint8_t>{0, 128, 255, 64, 192};
  const auto u8Fixture = monoKernelInput(u8Samples.data(), WaveformKernelSampleFormat::UInt8, u8Samples.size(), 1);
  const auto u8Input = kernelInputFromFixture(u8Fixture);
  const auto u8Result = computeScalarWaveformKernel(u8Input);
  CHECK(u8Result.actualCount == 5U);
  CHECK(u8Result.sumSquares > 1.0);
}

TEST_CASE("waveform simd avx2 kernels match scalar under shared decimation or fall back safely") {
  const auto checkParity = [](const WaveformKernelInput& input) {
    const auto scalar = computeScalarWaveformKernel(input);
    const auto dispatched = computeWaveformKernel(input, WaveformConfig{});

    CHECK(dispatched.actualCount == scalar.actualCount);
    CHECK(kernelClose(dispatched.sumSquares, scalar.sumSquares));

    if (!supportsAvx2()) {
      CHECK(dispatched.backend == WaveformKernelBackend::Scalar);
      return;
    }

    REQUIRE(waveformKernelCanUseAvx2(input));
    const auto avx2 = computeAvx2WaveformKernel(input);
    CHECK(avx2.backend == WaveformKernelBackend::Avx2);
    CHECK(avx2.actualCount == scalar.actualCount);
    CHECK(kernelClose(avx2.sumSquares, scalar.sumSquares));
    CHECK(dispatched.backend == WaveformKernelBackend::Avx2);
  };

  const auto samples = std::vector<float>{0.0F, 0.1F, -0.2F, 0.3F, -0.4F, 0.5F, -0.6F, 0.7F,
                                          -0.8F, 0.9F, -1.0F, 0.75F, -0.5F, 0.25F, -0.125F, 0.0625F,
                                          0.03125F, -0.015625F, 0.875F, -0.9375F, 0.4375F, -0.3125F,
                                          0.1875F, -0.6875F};
  const auto fixture = monoKernelInput(samples.data(), WaveformKernelSampleFormat::Float32, samples.size(), 3);
  checkParity(kernelInputFromFixture(fixture));

  const auto interleavedInt16 = std::vector<std::int16_t>{32767, 0, -32768, 16384, 8192, -8192, 0, 32767,
                                                          -4096, 2048, 1024, -1024, 30000, -30000, 1234, -1234};
  const auto interleavedPlanes = std::array<const void*, 1>{interleavedInt16.data()};
  auto interleavedInput = WaveformKernelInput{};
  interleavedInput.planes = interleavedPlanes.data();
  interleavedInput.sampleFormat = WaveformKernelSampleFormat::Int16;
  interleavedInput.layout = WaveformKernelSampleLayout::Interleaved;
  interleavedInput.frameCount = 8;
  interleavedInput.channelCount = 2;
  interleavedInput.decimation = 2;
  checkParity(interleavedInput);

  const auto int32Samples = std::vector<std::int32_t>{2'147'483'647, -2'147'483'648, 1'073'741'824, -536'870'912,
                                                      268'435'456, -134'217'728, 67'108'864, -33'554'432};
  const auto int32Fixture = monoKernelInput(int32Samples.data(), WaveformKernelSampleFormat::Int32, int32Samples.size(), 2);
  checkParity(kernelInputFromFixture(int32Fixture));

  const auto u8Samples = std::vector<std::uint8_t>{0, 128, 255, 64, 192, 32, 224, 96, 160};
  const auto u8Fixture = monoKernelInput(u8Samples.data(), WaveformKernelSampleFormat::UInt8, u8Samples.size(), 2);
  checkParity(kernelInputFromFixture(u8Fixture));
}

TEST_CASE("waveform strategy_a seek chunks match scalar for wav flac and mp3 fixtures") {
  constexpr auto frames = std::uint32_t{96'000};
  constexpr int barCount = 24;
  constexpr int chunkCount = 3;
  const auto sourceWav = waveformProbeSineFixture("waveform_strategy_a_source.wav", frames);
  const auto flac = waveformProbeFixtureDir() / "waveform_strategy_a_source.flac";
  const auto mp3 = waveformProbeFixtureDir() / "waveform_strategy_a_source.mp3";
  transcodeWaveformProbeFlac(sourceWav, flac);
  transcodeWaveformProbeMp3(sourceWav, mp3);
  const auto duration = waveformProbeDurationUs(frames);

  requireStrategyAMatchesScalar(sourceWav, barCount, 0, 0, duration, chunkCount);
  requireStrategyAMatchesScalar(flac, barCount, 0, 0, duration, chunkCount);
  requireStrategyAMatchesScalar(mp3, barCount, 0, 0, duration, chunkCount);
}

TEST_CASE("waveform strategy_a cue window discards backward seek overlap") {
  constexpr auto frames = std::uint32_t{48'000};
  constexpr int barCount = 4;
  constexpr int chunkCount = 4;
  auto samples = makeWaveformProbeSilence(frames, kWaveformProbeChannels);
  samples[6'000] = 32767;
  samples[27'000] = 32767;
  samples[42'000] = 32767;
  const auto path = waveformProbeFixture("waveform_strategy_a_cue.wav", samples, kWaveformProbeChannels);
  const auto range = normalizeTimeRange(250'000, 750'000, waveformProbeDurationUs(frames));

  const auto scalarBars = buildScalarWaveformBars(path, barCount, range);
  const auto strategyBars = mergeStrategyAChunks(path, barCount, range, chunkCount);
  const auto scalar = mapBarsToHeights(scalarBars, 100, WaveformConfig{});
  const auto strategy = mapBarsToHeights(strategyBars, 100, WaveformConfig{});

  REQUIRE(strategy.size() == 4U);
  CHECK(maxHeightDelta(strategy, scalar) <= 1);
  CHECK(strategy[0] == 2);
  CHECK(strategy[1] == 2);
  CHECK(strategy[2] > 2);
  CHECK(strategy[3] == 2);
}

TEST_CASE("waveform strategy_b packet batches match scalar for m4a and mp4 fixtures") {
  constexpr auto frames = std::uint32_t{96'000};
  constexpr int barCount = 24;
  const auto sourceWav = waveformProbeSineFixture("waveform_strategy_b_source.wav", frames);
  const auto m4a = waveformProbeFixtureDir() / "waveform_strategy_b_source.m4a";
  const auto mp4 = waveformProbeFixtureDir() / "waveform_strategy_b_source.mp4";
  transcodeWaveformProbeM4a(sourceWav, m4a);
  transcodeWaveformProbeMp4(sourceWav, mp4);
  const auto duration = waveformProbeDurationUs(frames);

  requireStrategyBMatchesScalar(m4a, barCount, 0, 0, duration);
  requireStrategyBMatchesScalar(mp4, barCount, 250'000, 1'750'000, duration);
}

TEST_CASE("waveform strategy_b empty normalized window returns full-size zero bars") {
  constexpr auto frames = std::uint32_t{48'000};
  constexpr int barCount = 6;
  const auto sourceWav = waveformProbeSineFixture("waveform_strategy_b_empty_source.wav", frames);
  const auto m4a = waveformProbeFixtureDir() / "waveform_strategy_b_empty_source.m4a";
  transcodeWaveformProbeM4a(sourceWav, m4a);
  const auto emptyRange = normalizeTimeRange(500'000, 500'000, waveformProbeDurationUs(frames));
  REQUIRE_FALSE(emptyRange.hasDuration);

  const auto bars = processPacketBatchStrategyB(StrategyBPacketBatchRequest{
      .filepath = m4a,
      .barCount = barCount,
      .timeRange = emptyRange,
      .config = WaveformConfig{},
  });

  requireStrategyBEmptyBars(bars, static_cast<std::size_t>(barCount));
}

TEST_CASE("waveform thread_pool auto mode matches explicit single thread strategy A output") {
  constexpr auto frames = std::uint32_t{48'000};
  constexpr int barCount = 12;
  const auto path = waveformProbeSineFixture("waveform_thread_pool_strategy_a.wav", frames);
  const auto range = normalizeTimeRange(0, 0, waveformProbeDurationUs(frames));
  REQUIRE(range.hasDuration);

  auto singleThreadConfig = WaveformConfig{};
  singleThreadConfig.threadCount = 1;
  auto autoThreadConfig = WaveformConfig{};
  autoThreadConfig.threadCount = 0;

  const auto singleThreadBars = processAudioChunksStrategyAWithThreadPool(path, barCount, range, singleThreadConfig);
  const auto autoThreadBars = processAudioChunksStrategyAWithThreadPool(path, barCount, range, autoThreadConfig);

  requireBarsClose(singleThreadBars, autoThreadBars);
}

TEST_CASE("waveform thread_pool explicit two threads runs tasks concurrently and merges deterministically") {
  auto config = WaveformConfig{};
  config.threadCount = 2;

  std::mutex mutex;
  std::condition_variable startedCondition;
  int startedTasks = 0;
  int activeTasks = 0;
  int maxActiveTasks = 0;

  const auto makeTask = [&](double sumSquares, std::uint64_t actualCount) {
    return [&, sumSquares, actualCount] {
      {
        std::unique_lock lock{mutex};
        ++startedTasks;
        ++activeTasks;
        maxActiveTasks = std::max(maxActiveTasks, activeTasks);
        startedCondition.notify_all();
        if (!startedCondition.wait_for(lock, std::chrono::seconds{2}, [&] { return startedTasks == 2; })) {
          throw std::runtime_error{"explicit two-thread scheduler did not run tasks concurrently"};
        }
        --activeTasks;
      }

      return std::vector<BarData>{BarData{.sumSquares = sumSquares, .actualCount = actualCount}};
    };
  };

  auto tasks = std::vector<WaveformBarTask>{makeTask(1.5, 3), makeTask(2.5, 5)};
  const auto bars = scheduleWaveformBarTasks(1, std::move(tasks), config, "thread_pool explicit two-thread test");

  REQUIRE(bars.size() == 1U);
  CHECK(bars[0].sumSquares == doctest::Approx(4.0));
  CHECK(bars[0].actualCount == 8U);
  CHECK(maxActiveTasks == 2);
}

TEST_CASE("waveform thread_pool injected task failure propagates as runtime_error") {
  auto config = WaveformConfig{};
  config.threadCount = 2;

  auto tasks = std::vector<WaveformBarTask>{
      [] { return std::vector<BarData>{BarData{.sumSquares = 1.0, .actualCount = 1}}; },
      []() -> std::vector<BarData> { throw std::logic_error{"injected chunk failure"}; },
  };

  try {
    static_cast<void>(scheduleWaveformBarTasks(1, std::move(tasks), config, "thread_pool injected failure test"));
    FAIL("expected std::runtime_error");
  } catch (const std::runtime_error& error) {
    const auto message = std::string{error.what()};
    CHECK(message.find("thread_pool injected failure test") != std::string::npos);
    CHECK(message.find("injected chunk failure") != std::string::npos);
  }
}

TEST_CASE("waveform perf release fixtures meet hard limits and record comparison run") {
  const auto fixtures = makeWaveformPerfFixtures();
  auto productionConfig = WaveformConfig{};
  productionConfig.threadCount = 0;
  productionConfig.enableSIMD = true;

  auto runs = std::vector<WaveformPerfRun>{};
  runs.reserve(fixtures.size() + 1U);
  for (const auto& fixture : fixtures) {
    runs.push_back(measureWaveformPerfRun(fixture, productionConfig, "production", kWaveformPerfHardGate));
  }

  auto simdDisabledConfig = productionConfig;
  simdDisabledConfig.enableSIMD = false;
  const auto flac = std::find_if(fixtures.begin(), fixtures.end(), [](const WaveformPerfFixture& fixture) {
    return fixture.format == "FLAC";
  });
  REQUIRE(flac != fixtures.end());
  runs.push_back(measureWaveformPerfRun(*flac, simdDisabledConfig, "comparison_enableSIMD_false", false));

  writeWaveformPerfReport(runs);
}
