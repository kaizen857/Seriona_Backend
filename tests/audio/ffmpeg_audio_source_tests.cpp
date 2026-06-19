#include "seriona/audio/ffmpeg_audio_source.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint16_t kChannels = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr double kPi = 3.141592653589793238462643383279502884;

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

std::vector<std::int16_t> makeSine(std::uint32_t frames, double frequency, double amplitude) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * frequency * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    samples.push_back(static_cast<std::int16_t>(std::lround(std::sin(phase) * amplitude * 32767.0)));
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

std::filesystem::path fixtureDir() {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  std::filesystem::create_directories(root);
  return root;
}

std::filesystem::path sineFixture(std::string name, std::uint32_t frames) {
  const auto path = fixtureDir() / std::move(name);
  writeWav(path, makeSine(frames, 440.0, 0.5));
  return path;
}

std::uint64_t readAllFrames(FfmpegAudioSource& source) {
  std::uint64_t frames = 0;
  while (true) {
    auto result = source.readFrame();
    REQUIRE_FALSE(result.error.has_value());
    if (result.endOfStream) {
      return frames;
    }
    REQUIRE(result.frame.has_value());
    CHECK(result.frame->sampleRate == kSampleRate);
    CHECK(result.frame->channelCount == kChannels);
    CHECK(result.frame->frameCount > 0);
    CHECK_FALSE(result.frame->sampleBytes.empty());
    frames += result.frame->frameCount;
  }
}

FfmpegAudioFrame firstFrameAfter(FfmpegAudioSource& source) {
  while (true) {
    auto result = source.readFrame();
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE_FALSE(result.endOfStream);
    REQUIRE(result.frame.has_value());
    if (result.frame->frameCount > 0) {
      return *result.frame;
    }
  }
}

}

TEST_CASE("ffmpeg_audio_source decodes generated wav frames") {
  const auto path = sineFixture("ffmpeg_source_sine.wav", 4096);
  FfmpegAudioSource source;

  const auto error = source.open(path);
  REQUIRE_FALSE(error.has_value());

  CHECK(source.streamInfo().sampleRate == kSampleRate);
  CHECK(source.streamInfo().channelCount == kChannels);
  CHECK(source.streamInfo().sampleFormat == AudioSampleFormat::Int16);
  CHECK(source.streamInfo().duration > 0us);
  CHECK(readAllFrames(source) > 0U);
}

TEST_CASE("ffmpeg_audio_source maps missing file to open failed") {
  FfmpegAudioSource source;

  const auto error = source.open(fixtureDir() / "missing.wav");

  REQUIRE(error.has_value());
  CHECK(error->code == PlaybackErrorCode::OpenFailed);
}

TEST_CASE("ffmpeg_audio_source maps invalid input to unsupported format") {
  const auto path = fixtureDir() / "not_audio.txt";
  std::ofstream output(path, std::ios::trunc);
  output << "this is not an audio container";
  output.close();

  FfmpegAudioSource source;

  const auto error = source.open(path);

  REQUIRE(error.has_value());
  CHECK(error->code == PlaybackErrorCode::UnsupportedFormat);
}

TEST_CASE("ffmpeg_audio_source maps no-audio input to unsupported format") {
  const auto path = fixtureDir() / "image_only.pgm";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P5\n2 2\n255\n";
  const auto pixels = std::array<unsigned char, 4>{0, 64, 128, 255};
  output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  output.close();

  FfmpegAudioSource source;

  const auto error = source.open(path);

  REQUIRE(error.has_value());
  CHECK(error->code == PlaybackErrorCode::UnsupportedFormat);
}

TEST_CASE("ffmpeg_audio_source seek flushes stale decoder state") {
  const auto path = sineFixture("ffmpeg_source_seek.wav", kSampleRate * 2U);
  FfmpegAudioSource source;

  REQUIRE_FALSE(source.open(path).has_value());
  const auto beforeSeek = firstFrameAfter(source);
  REQUIRE_FALSE(source.seek(1000ms).has_value());
  const auto afterSeek = firstFrameAfter(source);

  CHECK(beforeSeek.position < 100ms);
  CHECK(afterSeek.position >= 900ms);
  CHECK(afterSeek.position > beforeSeek.position);
}

}
