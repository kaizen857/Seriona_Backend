#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace seriona::audio {

struct PcmBufferQueueConfig {
  std::uint32_t capacityFrames{0};
  std::uint32_t bytesPerFrame{0};
};

struct PcmBufferQueueCounters {
  std::uint64_t submittedFrames{0};
  std::uint64_t consumedFrames{0};
  std::uint64_t overflowCount{0};
  std::uint64_t droppedFrames{0};
  std::uint64_t droppedBytes{0};
  std::uint64_t underrunCount{0};
  std::uint64_t silenceFrames{0};
  std::uint64_t clearedFrames{0};
  std::uint64_t clearCount{0};
};

struct PcmBufferReadResult {
  std::uint32_t requestedFrames{0};
  std::uint32_t copiedFrames{0};
  std::uint32_t silenceFrames{0};
};

using PcmBufferQueueGeneration = std::uint64_t;

class PcmBufferQueue {
public:
  explicit PcmBufferQueue(PcmBufferQueueConfig config);

  PcmBufferQueue(const PcmBufferQueue&) = delete;
  PcmBufferQueue& operator=(const PcmBufferQueue&) = delete;
  PcmBufferQueue(PcmBufferQueue&&) = delete;
  PcmBufferQueue& operator=(PcmBufferQueue&&) = delete;

  [[nodiscard]] bool write(const void* source, std::uint32_t frameCount) noexcept;
  [[nodiscard]] PcmBufferReadResult read(void* destination, std::uint32_t frameCount) noexcept;
  [[nodiscard]] PcmBufferReadResult readIfGeneration(void* destination,
                                                     std::uint32_t frameCount,
                                                     PcmBufferQueueGeneration generation) noexcept;
  void clearForSeek() noexcept;
  void resetCounters() noexcept;

  [[nodiscard]] PcmBufferQueueGeneration generation() const noexcept;
  [[nodiscard]] std::uint32_t capacityFrames() const noexcept;
  [[nodiscard]] std::uint32_t bytesPerFrame() const noexcept;
  [[nodiscard]] std::uint32_t availableFrames() const noexcept;
  [[nodiscard]] PcmBufferQueueCounters counters() const noexcept;

private:
  [[nodiscard]] std::size_t usedBytes() const noexcept;
  [[nodiscard]] std::size_t capacityBytes() const noexcept;
  [[nodiscard]] PcmBufferReadResult readReserved(void* destination,
                                                 std::uint32_t frameCount,
                                                 std::size_t requestedBytes,
                                                 std::size_t copiedBytes) noexcept;
  void copyIntoRing(const std::uint8_t* source, std::size_t byteCount) noexcept;
  void copyOutOfRing(std::uint8_t* destination, std::size_t byteCount) noexcept;

  std::vector<std::uint8_t> buffer_;
  std::uint32_t capacityFrames_{0};
  std::uint32_t bytesPerFrame_{0};
  std::atomic<std::size_t> readOffset_{0};
  std::atomic<std::size_t> writeOffset_{0};
  std::atomic<std::size_t> usedBytes_{0};
  std::atomic<std::uint64_t> submittedFrames_{0};
  std::atomic<std::uint64_t> consumedFrames_{0};
  std::atomic<std::uint64_t> overflowCount_{0};
  std::atomic<std::uint64_t> droppedFrames_{0};
  std::atomic<std::uint64_t> droppedBytes_{0};
  std::atomic<std::uint64_t> underrunCount_{0};
  std::atomic<std::uint64_t> silenceFrames_{0};
  std::atomic<std::uint64_t> clearedFrames_{0};
  std::atomic<std::uint64_t> clearCount_{0};
  std::atomic<PcmBufferQueueGeneration> generation_{1};
};

}
