#include "seriona/audio/ffmpeg_filter_pipeline.h"

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

constexpr std::uint32_t kSourceSampleRate = 48'000;
constexpr std::uint16_t kSourceChannels = 1;
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

std::vector<std::int16_t> makeSine(std::uint32_t frames) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kSourceChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kSourceSampleRate);
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
  writeU16(output, kSourceChannels);
  writeU32(output, kSourceSampleRate);
  writeU32(output, kSourceSampleRate * kSourceChannels * (kBitsPerSample / 8U));
  writeU16(output, static_cast<std::uint16_t>(kSourceChannels * (kBitsPerSample / 8U)));
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
  writeWav(path, makeSine(frames));
  return path;
}

std::vector<FfmpegAudioFrame> decodeFixture(const std::filesystem::path& path) {
  FfmpegAudioSource source;
  REQUIRE_FALSE(source.open(path).has_value());

  std::vector<FfmpegAudioFrame> frames;
  while (true) {
    auto result = source.readFrame();
    REQUIRE_FALSE(result.error.has_value());
    if (result.endOfStream) {
      return frames;
    }
    REQUIRE(result.frame.has_value());
    frames.push_back(std::move(*result.frame));
  }
}

std::vector<FfmpegAudioFrame> filterFrames(const std::vector<FfmpegAudioFrame>& input, const FfmpegFilterTargetFormat& target) {
  FfmpegFilterPipeline pipeline;
  REQUIRE_FALSE(pipeline.configure(target).has_value());

  std::vector<FfmpegAudioFrame> output;
  for (const auto& frame : input) {
    REQUIRE_FALSE(pipeline.pushFrame(frame).has_value());
    while (true) {
      auto result = pipeline.readFrame();
      REQUIRE_FALSE(result.error.has_value());
      REQUIRE_FALSE(result.endOfStream);
      if (!result.frame.has_value()) {
        break;
      }
      output.push_back(std::move(*result.frame));
    }
  }

  REQUIRE_FALSE(pipeline.signalEndOfInput().has_value());
  for (int guard = 0; guard < 64; ++guard) {
    auto result = pipeline.readFrame();
    REQUIRE_FALSE(result.error.has_value());
    if (result.endOfStream) {
      return output;
    }
    if (result.frame.has_value()) {
      output.push_back(std::move(*result.frame));
    }
  }

  FAIL("filter pipeline did not report EOF within guard limit");
  return output;
}

std::uint64_t countFrames(const std::vector<FfmpegAudioFrame>& frames) {
  std::uint64_t total = 0;
  for (const auto& frame : frames) {
    total += frame.frameCount;
  }
  return total;
}

int bytesPerSample(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2;
  case AudioSampleFormat::Float32:
  case AudioSampleFormat::Int32:
    return 4;
  case AudioSampleFormat::Int24:
    return 3;
  case AudioSampleFormat::Unknown:
    return 0;
  }

  return 0;
}

}

TEST_CASE("ffmpeg_filter_pipeline converts generated wav to target pcm") {
  const auto input = decodeFixture(sineFixture("ffmpeg_filter_convert.wav", kSourceSampleRate / 2U));
  REQUIRE_FALSE(input.empty());

  const auto target = FfmpegFilterTargetFormat{44'100, AudioSampleFormat::Float32, 2};
  const auto output = filterFrames(input, target);

  REQUIRE_FALSE(output.empty());
  CHECK(countFrames(output) >= 22'000U);
  CHECK(countFrames(output) <= 22'100U);
  for (const auto& frame : output) {
    CHECK(frame.sampleRate == target.sampleRate);
    CHECK(frame.channelCount == target.channelCount);
    CHECK(frame.sampleFormat == target.sampleFormat);
    CHECK(frame.frameCount > 0U);
    CHECK(frame.sampleBytes.size() == static_cast<std::size_t>(frame.frameCount) * target.channelCount * bytesPerSample(target.sampleFormat));
  }
}

TEST_CASE("ffmpeg_filter_pipeline drains EOF without losing filtered frames") {
  const auto sourceFrameCount = kSourceSampleRate / 4U;
  const auto input = decodeFixture(sineFixture("ffmpeg_filter_drain.wav", sourceFrameCount));
  REQUIRE_FALSE(input.empty());

  const auto target = FfmpegFilterTargetFormat{kSourceSampleRate, AudioSampleFormat::Int16, kSourceChannels};
  const auto output = filterFrames(input, target);

  REQUIRE_FALSE(output.empty());
  CHECK(countFrames(output) == sourceFrameCount);
}

TEST_CASE("ffmpeg_filter_pipeline maps invalid target to typed error") {
  FfmpegFilterPipeline pipeline;

  const auto error = pipeline.configure(FfmpegFilterTargetFormat{0, AudioSampleFormat::Unknown, 0});

  REQUIRE(error.has_value());
  CHECK((error->code == PlaybackErrorCode::UnsupportedFormat || error->code == PlaybackErrorCode::FormatNegotiationFailed));
}

}
