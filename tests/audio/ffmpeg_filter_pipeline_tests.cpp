#include "seriona/audio/ffmpeg_filter_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

constexpr std::uint32_t kSourceSampleRate = 48'000;
constexpr std::uint16_t kSourceChannels = 1;
constexpr std::uint16_t kStereoChannels = 2;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct FormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    if (context == nullptr) {
      return;
    }
    if ((context->oformat->flags & AVFMT_NOFILE) == 0 && context->pb != nullptr) {
      avio_closep(&context->pb);
    }
    avformat_free_context(context);
  }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

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

std::filesystem::path stereoPlanarFixture() {
  const auto path = fixtureDir() / "ffmpeg_source_stereo_planar.nut";

  AVFormatContext* rawFormat = nullptr;
  REQUIRE(avformat_alloc_output_context2(&rawFormat, nullptr, "nut", path.string().c_str()) >= 0);
  REQUIRE(rawFormat != nullptr);
  FormatContextPtr format(rawFormat);

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE_PLANAR);
  REQUIRE(codec != nullptr);
  AVStream* stream = avformat_new_stream(format.get(), nullptr);
  REQUIRE(stream != nullptr);

  CodecContextPtr codecContext(avcodec_alloc_context3(codec));
  REQUIRE(codecContext != nullptr);
  codecContext->sample_rate = static_cast<int>(kSourceSampleRate);
  codecContext->sample_fmt = AV_SAMPLE_FMT_S16P;
  codecContext->time_base = AVRational{1, static_cast<int>(kSourceSampleRate)};
  av_channel_layout_default(&codecContext->ch_layout, kStereoChannels);
  REQUIRE(avcodec_open2(codecContext.get(), codec, nullptr) >= 0);
  stream->time_base = codecContext->time_base;
  REQUIRE(avcodec_parameters_from_context(stream->codecpar, codecContext.get()) >= 0);

  REQUIRE(avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) >= 0);
  REQUIRE(avformat_write_header(format.get(), nullptr) >= 0);

  FramePtr frame(av_frame_alloc());
  REQUIRE(frame != nullptr);
  frame->nb_samples = 4;
  frame->format = codecContext->sample_fmt;
  frame->sample_rate = codecContext->sample_rate;
  frame->time_base = codecContext->time_base;
  frame->pts = 0;
  REQUIRE(av_channel_layout_copy(&frame->ch_layout, &codecContext->ch_layout) >= 0);
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  REQUIRE(av_frame_make_writable(frame.get()) >= 0);

  auto* left = reinterpret_cast<std::int16_t*>(frame->extended_data[0]);
  auto* right = reinterpret_cast<std::int16_t*>(frame->extended_data[1]);
  REQUIRE(left != nullptr);
  REQUIRE(right != nullptr);
  for (int sample = 0; sample < frame->nb_samples; ++sample) {
    left[sample] = static_cast<std::int16_t>(1000 + sample);
    right[sample] = static_cast<std::int16_t>(2000 + sample);
  }

  PacketPtr packet(av_packet_alloc());
  REQUIRE(packet != nullptr);
  REQUIRE(avcodec_send_frame(codecContext.get(), frame.get()) >= 0);
  while (avcodec_receive_packet(codecContext.get(), packet.get()) == 0) {
    av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
    packet->stream_index = stream->index;
    REQUIRE(av_interleaved_write_frame(format.get(), packet.get()) >= 0);
    av_packet_unref(packet.get());
  }
  REQUIRE(avcodec_send_frame(codecContext.get(), nullptr) >= 0);
  while (avcodec_receive_packet(codecContext.get(), packet.get()) == 0) {
    av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
    packet->stream_index = stream->index;
    REQUIRE(av_interleaved_write_frame(format.get(), packet.get()) >= 0);
    av_packet_unref(packet.get());
  }

  REQUIRE(av_write_trailer(format.get()) >= 0);
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

std::vector<std::int16_t> readInt16Samples(const FfmpegAudioFrame& frame) {
  std::vector<std::int16_t> samples;
  samples.reserve(frame.sampleBytes.size() / sizeof(std::int16_t));
  for (std::size_t index = 0; index + 1 < frame.sampleBytes.size(); index += sizeof(std::int16_t)) {
    samples.push_back(static_cast<std::int16_t>(static_cast<std::uint16_t>(frame.sampleBytes[index]) | (static_cast<std::uint16_t>(frame.sampleBytes[index + 1]) << 8U)));
  }
  return samples;
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

TEST_CASE("ffmpeg_filter_pipeline receives interleaved bytes from stereo planar source") {
  const auto input = decodeFixture(stereoPlanarFixture());
  REQUIRE_FALSE(input.empty());
  REQUIRE(input.front().frameCount >= 4U);
  REQUIRE(input.front().channelCount == kStereoChannels);
  REQUIRE(input.front().sampleFormat == AudioSampleFormat::Int16);
  REQUIRE(input.front().sampleBytes.size() == static_cast<std::size_t>(input.front().frameCount) * kStereoChannels * sizeof(std::int16_t));

  const auto decodedSamples = readInt16Samples(input.front());
  REQUIRE(decodedSamples.size() >= 8U);
  CHECK(decodedSamples[0] == 1000);
  CHECK(decodedSamples[1] == 2000);
  CHECK(decodedSamples[2] == 1001);
  CHECK(decodedSamples[3] == 2001);
  CHECK(decodedSamples[4] == 1002);
  CHECK(decodedSamples[5] == 2002);
  CHECK(decodedSamples[6] == 1003);
  CHECK(decodedSamples[7] == 2003);

  const auto target = FfmpegFilterTargetFormat{kSourceSampleRate, AudioSampleFormat::Int16, kStereoChannels};
  const auto output = filterFrames(input, target);
  REQUIRE_FALSE(output.empty());
  REQUIRE(output.front().sampleBytes.size() >= 8U * sizeof(std::int16_t));

  const auto filteredSamples = readInt16Samples(output.front());
  REQUIRE(filteredSamples.size() >= 8U);
  CHECK(filteredSamples[0] == 1000);
  CHECK(filteredSamples[1] == 2000);
  CHECK(filteredSamples[2] == 1001);
  CHECK(filteredSamples[3] == 2001);
}

TEST_CASE("ffmpeg_filter_pipeline maps invalid target to typed error") {
  FfmpegFilterPipeline pipeline;

  const auto error = pipeline.configure(FfmpegFilterTargetFormat{0, AudioSampleFormat::Unknown, 0});

  REQUIRE(error.has_value());
  CHECK((error->code == PlaybackErrorCode::UnsupportedFormat || error->code == PlaybackErrorCode::FormatNegotiationFailed));
}

}
