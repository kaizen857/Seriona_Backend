#include "seriona/audio/buffer/pcm_buffer_queue.h"

#include <doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace seriona::audio {
namespace {

using StereoFrame = std::array<std::uint8_t, 4>;

constexpr PcmBufferQueueConfig queueConfig(std::uint32_t frames) {
  return PcmBufferQueueConfig{frames, static_cast<std::uint32_t>(sizeof(StereoFrame))};
}

}

TEST_CASE("pcm_buffer_queue empty read writes silence and counts underrun") {
  PcmBufferQueue queue(queueConfig(4));
  std::array<StereoFrame, 3> output{StereoFrame{1, 1, 1, 1}, StereoFrame{2, 2, 2, 2}, StereoFrame{3, 3, 3, 3}};

  const auto result = queue.read(output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(result.requestedFrames == 3U);
  CHECK(result.copiedFrames == 0U);
  CHECK(result.silenceFrames == 3U);
  for (const auto& frame : output) {
    CHECK(frame == StereoFrame{0, 0, 0, 0});
  }

  const auto counters = queue.counters();
  CHECK(counters.underrunCount == 1U);
  CHECK(counters.silenceFrames == 3U);
  CHECK(counters.consumedFrames == 0U);
}

TEST_CASE("pcm_buffer_queue full write fails without dropping queued frames") {
  PcmBufferQueue queue(queueConfig(2));
  const std::array<StereoFrame, 2> initial{StereoFrame{1, 2, 3, 4}, StereoFrame{5, 6, 7, 8}};
  const std::array<StereoFrame, 1> extra{StereoFrame{9, 10, 11, 12}};
  std::array<StereoFrame, 2> output{};

  CHECK(queue.write(initial.data(), static_cast<std::uint32_t>(initial.size())));
  CHECK_FALSE(queue.write(extra.data(), static_cast<std::uint32_t>(extra.size())));
  CHECK(queue.availableFrames() == 2U);

  const auto result = queue.read(output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(result.copiedFrames == 2U);
  CHECK(result.silenceFrames == 0U);
  CHECK(output == initial);

  const auto counters = queue.counters();
  CHECK(counters.submittedFrames == 2U);
  CHECK(counters.consumedFrames == 2U);
  CHECK(counters.overflowCount == 1U);
  CHECK(counters.droppedFrames == 1U);
  CHECK(counters.droppedBytes == sizeof(StereoFrame));
}

TEST_CASE("pcm_buffer_queue partial read preserves order and fills remaining silence") {
  PcmBufferQueue queue(queueConfig(4));
  const std::array<StereoFrame, 2> input{StereoFrame{10, 11, 12, 13}, StereoFrame{20, 21, 22, 23}};
  std::array<StereoFrame, 4> output{StereoFrame{1, 1, 1, 1}, StereoFrame{1, 1, 1, 1}, StereoFrame{1, 1, 1, 1}, StereoFrame{1, 1, 1, 1}};

  CHECK(queue.write(input.data(), static_cast<std::uint32_t>(input.size())));
  const auto result = queue.read(output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(result.copiedFrames == 2U);
  CHECK(result.silenceFrames == 2U);
  CHECK(output[0] == input[0]);
  CHECK(output[1] == input[1]);
  CHECK(output[2] == StereoFrame{0, 0, 0, 0});
  CHECK(output[3] == StereoFrame{0, 0, 0, 0});
  CHECK(queue.availableFrames() == 0U);

  const auto counters = queue.counters();
  CHECK(counters.consumedFrames == 2U);
  CHECK(counters.underrunCount == 1U);
  CHECK(counters.silenceFrames == 2U);
}

TEST_CASE("pcm_buffer_queue wraps ring storage across reads and writes") {
  PcmBufferQueue queue(queueConfig(3));
  const std::array<StereoFrame, 3> first{StereoFrame{1, 0, 0, 0}, StereoFrame{2, 0, 0, 0}, StereoFrame{3, 0, 0, 0}};
  const std::array<StereoFrame, 2> second{StereoFrame{4, 0, 0, 0}, StereoFrame{5, 0, 0, 0}};
  std::array<StereoFrame, 2> consumed{};
  std::array<StereoFrame, 3> output{};

  CHECK(queue.write(first.data(), static_cast<std::uint32_t>(first.size())));
  CHECK(queue.read(consumed.data(), static_cast<std::uint32_t>(consumed.size())).copiedFrames == 2U);
  CHECK(queue.write(second.data(), static_cast<std::uint32_t>(second.size())));
  CHECK(queue.read(output.data(), static_cast<std::uint32_t>(output.size())).copiedFrames == 3U);

  CHECK(output[0] == first[2]);
  CHECK(output[1] == second[0]);
  CHECK(output[2] == second[1]);
  CHECK(queue.counters().underrunCount == 0U);
}

TEST_CASE("pcm_buffer_queue clear for seek removes stale pcm and tracks cleared frames") {
  PcmBufferQueue queue(queueConfig(4));
  const std::array<StereoFrame, 3> stale{StereoFrame{7, 7, 7, 7}, StereoFrame{8, 8, 8, 8}, StereoFrame{9, 9, 9, 9}};
  const std::array<StereoFrame, 1> fresh{StereoFrame{42, 42, 42, 42}};
  std::array<StereoFrame, 2> output{StereoFrame{1, 1, 1, 1}, StereoFrame{1, 1, 1, 1}};

  CHECK(queue.write(stale.data(), static_cast<std::uint32_t>(stale.size())));
  queue.clearForSeek();
  CHECK(queue.availableFrames() == 0U);
  CHECK(queue.write(fresh.data(), static_cast<std::uint32_t>(fresh.size())));

  const auto result = queue.read(output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(result.copiedFrames == 1U);
  CHECK(result.silenceFrames == 1U);
  CHECK(output[0] == fresh[0]);
  CHECK(output[1] == StereoFrame{0, 0, 0, 0});

  const auto counters = queue.counters();
  CHECK(counters.clearCount == 1U);
  CHECK(counters.clearedFrames == 3U);
}

TEST_CASE("pcm_buffer_queue read discards pcm when seek generation changes during read") {
  PcmBufferQueue queue(queueConfig(4));
  const std::array<StereoFrame, 2> stale{StereoFrame{7, 7, 7, 7}, StereoFrame{8, 8, 8, 8}};
  std::array<StereoFrame, 2> output{StereoFrame{1, 1, 1, 1}, StereoFrame{1, 1, 1, 1}};

  CHECK(queue.write(stale.data(), static_cast<std::uint32_t>(stale.size())));
  const auto generation = queue.generation();
  queue.clearForSeek();

  const auto result = queue.readIfGeneration(output.data(), static_cast<std::uint32_t>(output.size()), generation);

  CHECK(result.requestedFrames == 2U);
  CHECK(result.copiedFrames == 0U);
  CHECK(result.silenceFrames == 2U);
  for (const auto& frame : output) {
    CHECK(frame == StereoFrame{0, 0, 0, 0});
  }
  CHECK(queue.counters().consumedFrames == 0U);
}

TEST_CASE("pcm_buffer_queue preserves frame order under concurrent producer consumer access") {
  constexpr std::uint32_t totalFrames = 4096U;
  PcmBufferQueue queue(queueConfig(64));
  std::vector<StereoFrame> input(totalFrames);
  std::vector<StereoFrame> output(totalFrames, StereoFrame{0, 0, 0, 0});
  for (std::uint32_t index = 0; index < totalFrames; ++index) {
    input[index] = StereoFrame{static_cast<std::uint8_t>(index & 0xFFU),
                               static_cast<std::uint8_t>((index >> 8U) & 0xFFU),
                               static_cast<std::uint8_t>((index >> 16U) & 0xFFU),
                               static_cast<std::uint8_t>((index >> 24U) & 0xFFU)};
  }

  std::atomic<std::uint32_t> written{0};
  std::atomic<std::uint32_t> read{0};
  std::thread producer([&] {
    while (written.load(std::memory_order_acquire) < totalFrames) {
      const auto index = written.load(std::memory_order_relaxed);
      if (queue.write(&input[index], 1U)) {
        written.fetch_add(1U, std::memory_order_release);
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    while (read.load(std::memory_order_acquire) < totalFrames) {
      StereoFrame frame{0, 0, 0, 0};
      const auto result = queue.read(&frame, 1U);
      const auto index = read.load(std::memory_order_relaxed);
      if (result.copiedFrames == 1U) {
        output[index] = frame;
        read.fetch_add(1U, std::memory_order_release);
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  CHECK(output == input);
}

}
