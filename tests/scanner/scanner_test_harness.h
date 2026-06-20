#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner::test {

class TempScannerRoot {
public:
  explicit TempScannerRoot(std::string name);
  ~TempScannerRoot();

  TempScannerRoot(const TempScannerRoot&) = delete;
  TempScannerRoot& operator=(const TempScannerRoot&) = delete;

  TempScannerRoot(TempScannerRoot&& other) noexcept;
  TempScannerRoot& operator=(TempScannerRoot&& other) noexcept;

  [[nodiscard]] const std::filesystem::path& path() const noexcept;
  [[nodiscard]] std::filesystem::path dbPath(std::string filename = "scanner-cache.sqlite") const;
  [[nodiscard]] bool removed() const noexcept;

private:
  void cleanup() noexcept;

  std::filesystem::path path_;
  bool removed_{false};
};

class FakeClock {
public:
  using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

  [[nodiscard]] TimePoint now() const noexcept;
  void advance(std::chrono::milliseconds delta) noexcept;

private:
  TimePoint now_{};
};

struct ScannerEvent {
  enum class Type { ScanStarted, ScanFinished, TrackAccepted, TrackRejected, WatcherWarning };

  Type type{Type::ScanStarted};
  std::filesystem::path path;
  std::string message;
};

class FakeEventSink {
public:
  void publish(ScannerEvent event);
  [[nodiscard]] const std::vector<ScannerEvent>& events() const noexcept;
  [[nodiscard]] std::vector<ScannerEvent> eventsOfType(ScannerEvent::Type type) const;

private:
  std::vector<ScannerEvent> events_;
};

struct FakeTrackTag {
  std::filesystem::path filePath;
  std::string title;
  std::string artist;
  std::chrono::milliseconds duration{0};
};

class FakeTagReader {
public:
  enum class Behavior { Succeed, Throw, BlockUntilReleased };

  void setSuccess(FakeTrackTag tag);
  void setThrow(std::string message);
  void setBlocked(FakeTrackTag tag);
  void releaseBlockedReads();

  [[nodiscard]] FakeTrackTag read(const std::filesystem::path& path);
  [[nodiscard]] std::size_t readCount() const noexcept;
  [[nodiscard]] const std::vector<std::filesystem::path>& requestedPaths() const noexcept;

private:
  Behavior behavior_{Behavior::Succeed};
  FakeTrackTag tag_{};
  std::string throwMessage_{"fake tag reader failure"};
  bool released_{false};
  std::size_t readCount_{0};
  std::vector<std::filesystem::path> requestedPaths_;
  std::mutex mutex_;
  std::condition_variable releasedCv_;
};

struct FakeWatcherEvent {
  enum class Type { Created, Modified, Destroyed, Renamed, Warning };
  enum class PathKind { Audio, Lrc };

  Type type{Type::Created};
  PathKind pathKind{PathKind::Audio};
  std::filesystem::path path;
  std::optional<std::filesystem::path> oldPath;
  std::string warning;
};

class FakeWatcher {
public:
  void audioCreated(std::filesystem::path path);
  void audioModified(std::filesystem::path path);
  void audioDestroyed(std::filesystem::path path);
  void audioRenamed(std::filesystem::path oldPath, std::filesystem::path newPath);
  void lrcCreated(std::filesystem::path path);
  void lrcModified(std::filesystem::path path);
  void lrcDestroyed(std::filesystem::path path);
  void lrcRenamed(std::filesystem::path oldPath, std::filesystem::path newPath);
  void warning(std::filesystem::path path, std::string message);

  [[nodiscard]] const std::deque<FakeWatcherEvent>& events() const noexcept;
  [[nodiscard]] FakeWatcherEvent popNext();

private:
  void push(FakeWatcherEvent event);

  std::deque<FakeWatcherEvent> events_;
};

[[nodiscard]] std::filesystem::path writeAudioFixture(const std::filesystem::path& root, std::string filename);
[[nodiscard]] std::filesystem::path writeValidLrcFixture(const std::filesystem::path& root, std::string filename);
[[nodiscard]] std::filesystem::path writeInvalidLrcFixture(const std::filesystem::path& root, std::string filename);
void writeDbMarker(const std::filesystem::path& dbPath);
void requireEventCount(const FakeEventSink& sink, ScannerEvent::Type type, std::size_t expectedCount);
void requireWatcherEvent(const FakeWatcherEvent& event, FakeWatcherEvent::Type type, FakeWatcherEvent::PathKind pathKind,
                         const std::filesystem::path& path);

}
