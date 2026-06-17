#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint16_t kChannels = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr double kPi = 3.141592653589793238462643383279502884;

void fail(const std::string &message) {
  std::cerr << message << '\n';
  std::exit(1);
}

void check(bool condition, const std::string &message) {
  if (!condition) {
    fail(message);
  }
}

void writeU16(std::ofstream &stream, std::uint16_t value) {
  const auto bytes = std::array<unsigned char, 2>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ofstream &stream, std::uint32_t value) {
  const auto bytes = std::array<unsigned char, 4>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
      static_cast<unsigned char>((value >> 16U) & 0xFFU),
      static_cast<unsigned char>((value >> 24U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeTag(std::ofstream &stream, const char tag[4]) {
  stream.write(tag, 4);
}

void writeWav(const std::filesystem::path &path, const std::vector<std::int16_t> &samples) {
  const std::uint32_t frameCount = static_cast<std::uint32_t>(samples.size() / kChannels);
  const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  check(output.good(), "failed to open output file");

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

  check(output.good(), "failed while writing wav");
  check(frameCount == samples.size(), "frame count mismatch while writing wav");
}

struct WavInfo {
  std::uint16_t channels{};
  std::uint32_t sampleRate{};
  std::uint16_t bitsPerSample{};
  std::uint32_t frameCount{};
  std::vector<std::int16_t> samples;
};

WavInfo readWav(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  check(input.good(), "failed to open generated wav");

  auto readTag = [&input]() {
    std::array<char, 4> tag{};
    input.read(tag.data(), static_cast<std::streamsize>(tag.size()));
    check(input.good(), "failed to read wav tag");
    return tag;
  };

  auto readU16 = [&input]() {
    std::array<unsigned char, 2> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(input.good(), "failed to read u16");
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8U));
  };

  auto readU32 = [&input]() {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(input.good(), "failed to read u32");
    return static_cast<std::uint32_t>(bytes[0] |
                                      (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                      (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                      (static_cast<std::uint32_t>(bytes[3]) << 24U));
  };

  check(readTag() == std::array<char, 4>{'R', 'I', 'F', 'F'}, "missing RIFF tag");
  const auto riffSize = readU32();
  check(readTag() == std::array<char, 4>{'W', 'A', 'V', 'E'}, "missing WAVE tag");
  check(readTag() == std::array<char, 4>{'f', 'm', 't', ' '}, "missing fmt tag");
  check(readU32() == 16U, "unexpected fmt chunk size");
  check(readU16() == 1U, "unexpected PCM format");

  WavInfo info{};
  info.channels = readU16();
  info.sampleRate = readU32();
  const auto byteRate = readU32();
  const auto blockAlign = readU16();
  info.bitsPerSample = readU16();

  check(readTag() == std::array<char, 4>{'d', 'a', 't', 'a'}, "missing data tag");
  const auto dataSize = readU32();

  info.frameCount = dataSize / (info.channels * (info.bitsPerSample / 8U));
  info.samples.resize(dataSize / sizeof(std::int16_t));

  for (auto &sample : info.samples) {
    std::array<unsigned char, 2> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(input.good(), "failed to read sample");
    sample = static_cast<std::int16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8U));
  }

  check(riffSize == 36U + dataSize, "riff size mismatch");
  check(byteRate == info.sampleRate * info.channels * (info.bitsPerSample / 8U), "byte rate mismatch");
  check(blockAlign == info.channels * (info.bitsPerSample / 8U), "block align mismatch");
  return info;
}

std::vector<std::int16_t> makeSilence(std::uint32_t frames) {
  return std::vector<std::int16_t>(frames * kChannels, 0);
}

std::vector<std::int16_t> makeSine(std::uint32_t frames, double frequency, double amplitude) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * frequency * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    const auto value = static_cast<std::int16_t>(std::lround(std::sin(phase) * amplitude * 32767.0));
    samples.push_back(value);
  }

  return samples;
}

std::filesystem::path fixtureDir() {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  std::filesystem::create_directories(root);
  return root;
}

}

int main() {
  const auto root = fixtureDir();
  const auto silencePath = root / "silence_240f.wav";
  const auto sinePath = root / "sine_960f.wav";
  const auto seekPath = root / "sine_seek_1024f.wav";

  writeWav(silencePath, makeSilence(240));
  writeWav(sinePath, makeSine(960, 440.0, 0.5));
  writeWav(seekPath, makeSine(1024, 220.0, 0.25));

  const auto silence = readWav(silencePath);
  const auto sine = readWav(sinePath);
  const auto seek = readWav(seekPath);

  check(silence.channels == kChannels, "silence channel count mismatch");
  check(silence.sampleRate == kSampleRate, "silence sample rate mismatch");
  check(silence.bitsPerSample == kBitsPerSample, "silence bit depth mismatch");
  check(silence.frameCount == 240U, "silence frame count mismatch");
  check(silence.samples.size() == 240U, "silence sample count mismatch");
  check(std::all_of(silence.samples.begin(), silence.samples.end(), [](std::int16_t sample) { return sample == 0; }), "silence contains non-zero samples");

  check(sine.channels == kChannels, "sine channel count mismatch");
  check(sine.sampleRate == kSampleRate, "sine sample rate mismatch");
  check(sine.frameCount == 960U, "sine frame count mismatch");
  check(sine.samples.size() == 960U, "sine sample count mismatch");
  check(*std::max_element(sine.samples.begin(), sine.samples.end()) > 0, "sine should have positive samples");
  check(*std::min_element(sine.samples.begin(), sine.samples.end()) < 0, "sine should have negative samples");

  check(seek.frameCount == 1024U, "seek frame count mismatch");
  check(seek.samples.size() == 1024U, "seek sample count mismatch");
  check(seek.frameCount % 256U == 0U, "seek-friendly 256-sample alignment mismatch");
  check(seek.frameCount % 128U == 0U, "seek-friendly 128-sample alignment mismatch");

  return 0;
}
