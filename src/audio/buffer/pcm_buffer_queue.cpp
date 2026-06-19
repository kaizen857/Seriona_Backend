#include "seriona/audio/buffer/pcm_buffer_queue.h"

#include <algorithm>
#include <cstring>

namespace seriona::audio {

PcmBufferQueue::PcmBufferQueue(PcmBufferQueueConfig config)
    : buffer_(static_cast<std::size_t>(config.capacityFrames) * config.bytesPerFrame),
      capacityFrames_(config.capacityFrames),
      bytesPerFrame_(config.bytesPerFrame) {}

bool PcmBufferQueue::write(const void* source, std::uint32_t frameCount) noexcept {
  const auto byteCount = static_cast<std::size_t>(frameCount) * bytesPerFrame_;
  if (frameCount == 0U) {
    return true;
  }

  if (source == nullptr || bytesPerFrame_ == 0U || byteCount > capacityBytes() - usedBytes()) {
    overflowCount_.fetch_add(1U, std::memory_order_relaxed);
    droppedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
    droppedBytes_.fetch_add(byteCount, std::memory_order_relaxed);
    return false;
  }

  copyIntoRing(static_cast<const std::uint8_t*>(source), byteCount);
  usedBytes_.fetch_add(byteCount, std::memory_order_release);
  submittedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
  return true;
}

PcmBufferReadResult PcmBufferQueue::read(void* destination, std::uint32_t frameCount) noexcept {
  PcmBufferReadResult result{};
  result.requestedFrames = frameCount;

  const auto requestedBytes = static_cast<std::size_t>(frameCount) * bytesPerFrame_;
  if (frameCount == 0U) {
    return result;
  }

  if (destination == nullptr || bytesPerFrame_ == 0U) {
    underrunCount_.fetch_add(1U, std::memory_order_relaxed);
    silenceFrames_.fetch_add(frameCount, std::memory_order_relaxed);
    result.silenceFrames = frameCount;
    return result;
  }

  const auto availableBytes = usedBytes();
  const auto copiedBytes = std::min(requestedBytes, availableBytes);
  auto* output = static_cast<std::uint8_t*>(destination);

  if (copiedBytes > 0U) {
    copyOutOfRing(output, copiedBytes);
    usedBytes_.fetch_sub(copiedBytes, std::memory_order_release);
  }

  const auto silenceBytes = requestedBytes - copiedBytes;
  if (silenceBytes > 0U) {
    std::memset(output + copiedBytes, 0, silenceBytes);
    underrunCount_.fetch_add(1U, std::memory_order_relaxed);
  }

  result.copiedFrames = static_cast<std::uint32_t>(copiedBytes / bytesPerFrame_);
  result.silenceFrames = frameCount - result.copiedFrames;
  consumedFrames_.fetch_add(result.copiedFrames, std::memory_order_relaxed);
  silenceFrames_.fetch_add(result.silenceFrames, std::memory_order_relaxed);
  return result;
}

void PcmBufferQueue::clearForSeek() noexcept {
  const auto clearedBytes = usedBytes_.exchange(0U, std::memory_order_acq_rel);
  readOffset_.store(0U, std::memory_order_release);
  writeOffset_.store(0U, std::memory_order_release);
  clearCount_.fetch_add(1U, std::memory_order_relaxed);
  if (bytesPerFrame_ > 0U) {
    clearedFrames_.fetch_add(clearedBytes / bytesPerFrame_, std::memory_order_relaxed);
  }
}

void PcmBufferQueue::resetCounters() noexcept {
  submittedFrames_.store(0U, std::memory_order_relaxed);
  consumedFrames_.store(0U, std::memory_order_relaxed);
  overflowCount_.store(0U, std::memory_order_relaxed);
  droppedFrames_.store(0U, std::memory_order_relaxed);
  droppedBytes_.store(0U, std::memory_order_relaxed);
  underrunCount_.store(0U, std::memory_order_relaxed);
  silenceFrames_.store(0U, std::memory_order_relaxed);
  clearedFrames_.store(0U, std::memory_order_relaxed);
  clearCount_.store(0U, std::memory_order_relaxed);
}

std::uint32_t PcmBufferQueue::capacityFrames() const noexcept { return capacityFrames_; }

std::uint32_t PcmBufferQueue::bytesPerFrame() const noexcept { return bytesPerFrame_; }

std::uint32_t PcmBufferQueue::availableFrames() const noexcept {
  if (bytesPerFrame_ == 0U) {
    return 0U;
  }

  return static_cast<std::uint32_t>(usedBytes() / bytesPerFrame_);
}

PcmBufferQueueCounters PcmBufferQueue::counters() const noexcept {
  return PcmBufferQueueCounters{submittedFrames_.load(std::memory_order_relaxed),
                                consumedFrames_.load(std::memory_order_relaxed),
                                overflowCount_.load(std::memory_order_relaxed),
                                droppedFrames_.load(std::memory_order_relaxed),
                                droppedBytes_.load(std::memory_order_relaxed),
                                underrunCount_.load(std::memory_order_relaxed),
                                silenceFrames_.load(std::memory_order_relaxed),
                                clearedFrames_.load(std::memory_order_relaxed),
                                clearCount_.load(std::memory_order_relaxed)};
}

std::size_t PcmBufferQueue::usedBytes() const noexcept {
  return usedBytes_.load(std::memory_order_acquire);
}

std::size_t PcmBufferQueue::capacityBytes() const noexcept { return buffer_.size(); }

void PcmBufferQueue::copyIntoRing(const std::uint8_t* source, std::size_t byteCount) noexcept {
  const auto capacity = capacityBytes();
  auto offset = writeOffset_.load(std::memory_order_relaxed);
  const auto firstCopy = std::min(byteCount, capacity - offset);
  std::memcpy(buffer_.data() + offset, source, firstCopy);
  if (firstCopy < byteCount) {
    std::memcpy(buffer_.data(), source + firstCopy, byteCount - firstCopy);
  }
  writeOffset_.store((offset + byteCount) % capacity, std::memory_order_release);
}

void PcmBufferQueue::copyOutOfRing(std::uint8_t* destination, std::size_t byteCount) noexcept {
  const auto capacity = capacityBytes();
  auto offset = readOffset_.load(std::memory_order_relaxed);
  const auto firstCopy = std::min(byteCount, capacity - offset);
  std::memcpy(destination, buffer_.data() + offset, firstCopy);
  if (firstCopy < byteCount) {
    std::memcpy(destination + firstCopy, buffer_.data(), byteCount - firstCopy);
  }
  readOffset_.store((offset + byteCount) % capacity, std::memory_order_release);
}

}
