#include "seriona/audio/ffmpeg_audio_source.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

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

std::string shellQuote(const std::filesystem::path& path) {
  const auto text = path.string();
  REQUIRE(text.find('\'') == std::string::npos);
  return "'" + text + "'";
}

void requireFfmpegCommand(const std::string& command) {
  REQUIRE(std::filesystem::exists("/usr/bin/ffmpeg"));
  const int exitCode = std::system(command.c_str());
  REQUIRE_MESSAGE(exitCode == 0, "ffmpeg command failed with exit=" << exitCode << ": " << command);
}

void transcodeMp3(const std::filesystem::path& sourceWav,
                  const std::filesystem::path& outputMp3,
                  const char* bitrate = "128k") {
  const auto command = std::string{"/usr/bin/ffmpeg -v error -nostdin -y -i "} + shellQuote(sourceWav) +
                       " -map_metadata -1 -id3v2_version 0 -write_id3v1 0 -codec:a libmp3lame -b:a " + bitrate + " " +
                       shellQuote(outputMp3);

  requireFfmpegCommand(command);
  REQUIRE(std::filesystem::exists(outputMp3));
}

void copyWithId3v1Tag(const std::filesystem::path& sourceMp3,
                     const std::filesystem::path& taggedMp3) {
  std::filesystem::copy_file(sourceMp3, taggedMp3, std::filesystem::copy_options::overwrite_existing);

  auto tag = std::array<unsigned char, 128>{};
  tag[0] = 'T';
  tag[1] = 'A';
  tag[2] = 'G';

  std::ofstream output(taggedMp3, std::ios::binary | std::ios::app);
  REQUIRE(output.good());
  output.write(reinterpret_cast<const char*>(tag.data()), static_cast<std::streamsize>(tag.size()));
  REQUIRE(output.good());
}

struct AudioPacketRange {
  std::int64_t position{0};
  int size{0};
};

std::vector<AudioPacketRange> audioPacketRanges(const std::filesystem::path& sourceMp3,
                                                int minimumSize = 0) {
  AVFormatContext* rawFormat = nullptr;
  REQUIRE(avformat_open_input(&rawFormat, sourceMp3.string().c_str(), nullptr, nullptr) >= 0);
  REQUIRE(avformat_find_stream_info(rawFormat, nullptr) >= 0);

  const int audioStreamIndex = av_find_best_stream(rawFormat, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  REQUIRE(audioStreamIndex >= 0);

  std::vector<AudioPacketRange> packets;
  auto packet = std::unique_ptr<AVPacket, void (*)(AVPacket*)>(av_packet_alloc(), [](AVPacket* value) {
    av_packet_free(&value);
  });
  REQUIRE(packet != nullptr);
  while (av_read_frame(rawFormat, packet.get()) >= 0) {
    if (packet->stream_index == audioStreamIndex && packet->pos >= 0 && packet->size >= minimumSize) {
      packets.push_back(AudioPacketRange{packet->pos, packet->size});
    }
    av_packet_unref(packet.get());
  }

  avformat_close_input(&rawFormat);

  return packets;
}

AudioPacketRange middleAudioPacketRange(const std::filesystem::path& sourceMp3,
                                        int minimumSize = 0) {
  auto packets = audioPacketRanges(sourceMp3, minimumSize);
  REQUIRE(packets.size() >= 3U);
  return packets[packets.size() / 2U];
}

void copyWithEmbeddedId3v1BlockAt(const std::filesystem::path& sourceMp3,
                                  const std::filesystem::path& taggedMp3,
                                  std::int64_t insertionOffset) {
  REQUIRE(insertionOffset > 0);

  std::ifstream input(sourceMp3, std::ios::binary);
  REQUIRE(input.good());
  std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
  REQUIRE(static_cast<std::uint64_t>(insertionOffset) < bytes.size());

  auto tag = std::array<std::uint8_t, 128>{};
  tag[0] = 'T';
  tag[1] = 'A';
  tag[2] = 'G';

  bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertionOffset), tag.begin(), tag.end());

  std::ofstream output(taggedMp3, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(output.good());
}

void copyWithEmbeddedId3v1BlockAtPacketBoundary(const std::filesystem::path& sourceMp3,
                                                const std::filesystem::path& taggedMp3) {
  const auto packet = middleAudioPacketRange(sourceMp3);
  copyWithEmbeddedId3v1BlockAt(sourceMp3, taggedMp3, packet.position);
}

void copyWithEmbeddedId3v1BlockInsidePacket(const std::filesystem::path& sourceMp3,
                                            const std::filesystem::path& taggedMp3) {
  const auto packet = middleAudioPacketRange(sourceMp3, 512);
  copyWithEmbeddedId3v1BlockAt(sourceMp3, taggedMp3, packet.position + 80);
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

TEST_CASE("ffmpeg_audio_source ignores terminal mp3 id3v1 tag bytes") {
  if (!std::filesystem::exists("/usr/bin/ffmpeg")) {
    INFO("/usr/bin/ffmpeg not available; skipping mp3 tail corruption regression test");
    return;
  }

  const auto sourceWav = sineFixture("ffmpeg_source_id3v1_source.wav", kSampleRate * 2U);
  const auto untaggedMp3 = fixtureDir() / "ffmpeg_source_id3v1_untagged.mp3";
  const auto taggedMp3 = fixtureDir() / "ffmpeg_source_id3v1_tagged.mp3";
  transcodeMp3(sourceWav, untaggedMp3);
  copyWithId3v1Tag(untaggedMp3, taggedMp3);

  FfmpegAudioSource source;
  REQUIRE_FALSE(source.open(taggedMp3).has_value());

  CHECK(readAllFrames(source) > 0U);
}

TEST_CASE("ffmpeg_audio_source trims embedded mp3 id3v1 packet prefixes without losing decode progress") {
  if (!std::filesystem::exists("/usr/bin/ffmpeg")) {
    INFO("/usr/bin/ffmpeg not available; skipping embedded mp3 ID3v1 regression test");
    return;
  }

  const auto sourceWav = sineFixture("ffmpeg_source_embedded_id3v1_source.wav", kSampleRate * 4U);
  const auto cleanMp3 = fixtureDir() / "ffmpeg_source_embedded_id3v1_clean.mp3";
  const auto taggedMp3 = fixtureDir() / "ffmpeg_source_embedded_id3v1_tagged.mp3";
  transcodeMp3(sourceWav, cleanMp3, "320k");
  copyWithEmbeddedId3v1BlockAtPacketBoundary(cleanMp3, taggedMp3);

  FfmpegAudioSource cleanSource;
  REQUIRE_FALSE(cleanSource.open(cleanMp3).has_value());
  const auto cleanFrames = readAllFrames(cleanSource);

  FfmpegAudioSource taggedSource;
  REQUIRE_FALSE(taggedSource.open(taggedMp3).has_value());
  const auto taggedFrames = readAllFrames(taggedSource);

  CHECK(taggedFrames == cleanFrames);
}

TEST_CASE("ffmpeg_audio_source removes embedded mp3 id3v1 blocks inside packets without catastrophic decode loss") {
  if (!std::filesystem::exists("/usr/bin/ffmpeg")) {
    INFO("/usr/bin/ffmpeg not available; skipping embedded in-packet mp3 ID3v1 regression test");
    return;
  }

  const auto sourceWav = sineFixture("ffmpeg_source_embedded_midpacket_id3v1_source.wav", kSampleRate * 4U);
  const auto cleanMp3 = fixtureDir() / "ffmpeg_source_embedded_midpacket_id3v1_clean.mp3";
  const auto taggedMp3 = fixtureDir() / "ffmpeg_source_embedded_midpacket_id3v1_tagged.mp3";
  transcodeMp3(sourceWav, cleanMp3, "320k");
  copyWithEmbeddedId3v1BlockInsidePacket(cleanMp3, taggedMp3);

  FfmpegAudioSource cleanSource;
  REQUIRE_FALSE(cleanSource.open(cleanMp3).has_value());
  const auto cleanFrames = readAllFrames(cleanSource);

  FfmpegAudioSource taggedSource;
  REQUIRE_FALSE(taggedSource.open(taggedMp3).has_value());
  const auto taggedFrames = readAllFrames(taggedSource);

  CHECK(taggedFrames <= cleanFrames);
  CHECK(taggedFrames >= cleanFrames - 1152U);
}

TEST_CASE("ffmpeg_audio_source tail guard keeps legitimate final mp3 frame") {
  const auto info = FfmpegAudioStreamInfo{44'100U, 2U, AudioSampleFormat::Float32, std::chrono::microseconds{320'626'667}};
  const auto finalFrame = FfmpegAudioFrame{44'100U,
                                           2U,
                                           AudioSampleFormat::Float32,
                                           0,
                                           std::chrono::microseconds{320'626'939},
                                           1'093U,
                                           {}};

  CHECK_FALSE(seriona::audio::detail::shouldTerminateCorruptedMp3Tail(info, finalFrame, false));
  CHECK_FALSE(seriona::audio::detail::shouldTerminateCorruptedMp3Tail(info, finalFrame, true));
}

TEST_CASE("ffmpeg_audio_source tail guard truncates post-invalid mp3 tail drift and signature jumps") {
  const auto info = FfmpegAudioStreamInfo{44'100U, 2U, AudioSampleFormat::Float32, std::chrono::microseconds{320'626'667}};
  const auto lateFrameSameSignature = FfmpegAudioFrame{44'100U,
                                                       2U,
                                                       AudioSampleFormat::Float32,
                                                       0,
                                                       std::chrono::microseconds{320'679'184},
                                                       1'152U,
                                                       {}};
  const auto lateFrameDifferentSignature = FfmpegAudioFrame{12'000U,
                                                            2U,
                                                            AudioSampleFormat::Float32,
                                                            0,
                                                            std::chrono::microseconds{320'705'306},
                                                            1'152U,
                                                            {}};

  CHECK(seriona::audio::detail::shouldTerminateCorruptedMp3Tail(info, lateFrameSameSignature, true));
  CHECK(seriona::audio::detail::shouldTerminateCorruptedMp3Tail(info, lateFrameDifferentSignature, true));
  CHECK_FALSE(seriona::audio::detail::shouldTerminateCorruptedMp3Tail(info, lateFrameDifferentSignature, false));
}

}
