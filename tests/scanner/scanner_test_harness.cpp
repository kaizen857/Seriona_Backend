#include "scanner_test_harness.h"

#include <atomic>

namespace seriona::scanner::test {
namespace {

std::filesystem::path uniqueRootPath(const std::string& name) {
  static std::atomic_uint64_t sequence{0};
  const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
  return std::filesystem::temp_directory_path() / ("seriona-scanner-" + name + "-" + std::to_string(id));
}

std::filesystem::path databaseRootPath(const std::filesystem::path& rootPath) {
  return rootPath.parent_path() / (rootPath.filename().string() + "-db");
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.good()) {
    throw std::runtime_error("failed to open fixture file: " + path.string());
  }
  output << content;
  if (!output.good()) {
    throw std::runtime_error("failed to write fixture file: " + path.string());
  }
}

FakeWatcherEvent watcherEvent(FakeWatcherEvent::Type type, FakeWatcherEvent::PathKind pathKind, std::filesystem::path path) {
  return {type, pathKind, std::move(path), std::nullopt, {}};
}

FakeWatcherEvent renameEvent(FakeWatcherEvent::PathKind pathKind, std::filesystem::path oldPath,
                             std::filesystem::path newPath) {
  return {FakeWatcherEvent::Type::Renamed, pathKind, std::move(newPath), std::move(oldPath), {}};
}

FakeWatcherEvent warningEvent(FakeWatcherEvent::PathKind pathKind, std::filesystem::path path, std::string message) {
  return {FakeWatcherEvent::Type::Warning, pathKind, std::move(path), std::nullopt, std::move(message)};
}

}

TempScannerRoot::TempScannerRoot(std::string name) : path_(uniqueRootPath(name)) {
  const auto dbRoot = databaseRootPath(path_);
  std::error_code error;
  std::filesystem::remove_all(path_, error);
  std::filesystem::remove_all(dbRoot, error);
  std::filesystem::create_directories(path_);
  std::filesystem::create_directories(dbRoot);
  std::error_code canonicalError;
  const auto canonicalPath = std::filesystem::weakly_canonical(path_, canonicalError);
  path_ = canonicalError ? path_.lexically_normal() : canonicalPath;
}

TempScannerRoot::~TempScannerRoot() {
  cleanup();
}

TempScannerRoot::TempScannerRoot(TempScannerRoot&& other) noexcept
    : path_(std::move(other.path_)), removed_(other.removed_) {
  other.removed_ = true;
}

TempScannerRoot& TempScannerRoot::operator=(TempScannerRoot&& other) noexcept {
  if (this != &other) {
    cleanup();
    path_ = std::move(other.path_);
    removed_ = other.removed_;
    other.removed_ = true;
  }
  return *this;
}

const std::filesystem::path& TempScannerRoot::path() const noexcept {
  return path_;
}

std::filesystem::path TempScannerRoot::dbPath(std::string filename) const {
  return databaseRootPath(path_) / std::move(filename);
}

bool TempScannerRoot::removed() const noexcept {
  return removed_;
}

void TempScannerRoot::cleanup() noexcept {
  if (removed_ || path_.empty()) {
    return;
  }
  std::error_code error;
  const auto dbRoot = databaseRootPath(path_);
  std::filesystem::remove_all(path_, error);
  std::filesystem::remove_all(dbRoot, error);
  removed_ = !std::filesystem::exists(path_) && !std::filesystem::exists(dbRoot);
}

FakeClock::TimePoint FakeClock::now() const noexcept {
  return now_;
}

void FakeClock::advance(std::chrono::milliseconds delta) noexcept {
  now_ += delta;
}

void FakeEventSink::publish(ScannerEvent event) {
  events_.push_back(std::move(event));
}

const std::vector<ScannerEvent>& FakeEventSink::events() const noexcept {
  return events_;
}

std::vector<ScannerEvent> FakeEventSink::eventsOfType(ScannerEvent::Type type) const {
  std::vector<ScannerEvent> matches;
  for (const auto& event : events_) {
    if (event.type == type) {
      matches.push_back(event);
    }
  }
  return matches;
}

void FakeTagReader::setSuccess(FakeTrackTag tag) {
  std::lock_guard lock(mutex_);
  behavior_ = Behavior::Succeed;
  tag_ = std::move(tag);
  released_ = true;
}

void FakeTagReader::setThrow(std::string message) {
  std::lock_guard lock(mutex_);
  behavior_ = Behavior::Throw;
  throwMessage_ = std::move(message);
  released_ = true;
}

void FakeTagReader::setBlocked(FakeTrackTag tag) {
  std::lock_guard lock(mutex_);
  behavior_ = Behavior::BlockUntilReleased;
  tag_ = std::move(tag);
  released_ = false;
}

void FakeTagReader::releaseBlockedReads() {
  {
    std::lock_guard lock(mutex_);
    released_ = true;
  }
  releasedCv_.notify_all();
}

FakeTrackTag FakeTagReader::read(const std::filesystem::path& path) {
  std::unique_lock lock(mutex_);
  ++readCount_;
  requestedPaths_.push_back(path);
  if (behavior_ == Behavior::Throw) {
    throw std::runtime_error(throwMessage_);
  }
  if (behavior_ == Behavior::BlockUntilReleased) {
    releasedCv_.wait(lock, [this] { return released_; });
  }
  auto tag = tag_;
  tag.filePath = path;
  return tag;
}

std::size_t FakeTagReader::readCount() const noexcept {
  return readCount_;
}

const std::vector<std::filesystem::path>& FakeTagReader::requestedPaths() const noexcept {
  return requestedPaths_;
}

void FakeWatcher::audioCreated(std::filesystem::path path) {
  push(watcherEvent(FakeWatcherEvent::Type::Created, FakeWatcherEvent::PathKind::Audio, std::move(path)));
}

void FakeWatcher::audioModified(std::filesystem::path path) {
  push(watcherEvent(FakeWatcherEvent::Type::Modified, FakeWatcherEvent::PathKind::Audio, std::move(path)));
}

void FakeWatcher::audioDestroyed(std::filesystem::path path) {
  push(watcherEvent(FakeWatcherEvent::Type::Destroyed, FakeWatcherEvent::PathKind::Audio, std::move(path)));
}

void FakeWatcher::audioRenamed(std::filesystem::path oldPath, std::filesystem::path newPath) {
  push(renameEvent(FakeWatcherEvent::PathKind::Audio, std::move(oldPath), std::move(newPath)));
}

void FakeWatcher::lrcCreated(std::filesystem::path path) {
  push(watcherEvent(FakeWatcherEvent::Type::Created, FakeWatcherEvent::PathKind::Lrc, std::move(path)));
}

void FakeWatcher::lrcModified(std::filesystem::path path) {
  push(watcherEvent(FakeWatcherEvent::Type::Modified, FakeWatcherEvent::PathKind::Lrc, std::move(path)));
}

void FakeWatcher::lrcDestroyed(std::filesystem::path path) {
  push(watcherEvent(FakeWatcherEvent::Type::Destroyed, FakeWatcherEvent::PathKind::Lrc, std::move(path)));
}

void FakeWatcher::lrcRenamed(std::filesystem::path oldPath, std::filesystem::path newPath) {
  push(renameEvent(FakeWatcherEvent::PathKind::Lrc, std::move(oldPath), std::move(newPath)));
}

void FakeWatcher::audioWarning(std::filesystem::path path, std::string message) {
  push(warningEvent(FakeWatcherEvent::PathKind::Audio, std::move(path), std::move(message)));
}

void FakeWatcher::lrcWarning(std::filesystem::path path, std::string message) {
  push(warningEvent(FakeWatcherEvent::PathKind::Lrc, std::move(path), std::move(message)));
}

const std::deque<FakeWatcherEvent>& FakeWatcher::events() const noexcept {
  return events_;
}

FakeWatcherEvent FakeWatcher::popNext() {
  if (events_.empty()) {
    throw std::runtime_error("fake watcher has no queued events");
  }
  auto event = std::move(events_.front());
  events_.pop_front();
  return event;
}

void FakeWatcher::push(FakeWatcherEvent event) {
  events_.push_back(std::move(event));
}

std::filesystem::path writeAudioFixture(const std::filesystem::path& root, std::string filename) {
  auto path = root / std::move(filename);
  writeTextFile(path, "SERIONA_TEST_AUDIO\nframes=1024\nsample_rate=48000\n");
  return path;
}

std::filesystem::path writeValidLrcFixture(const std::filesystem::path& root, std::string filename) {
  auto path = root / std::move(filename);
  writeTextFile(path, "[00:00.00]Seriona fixture lyric\n[00:01.25]second deterministic line\n");
  return path;
}

std::filesystem::path writeInvalidLrcFixture(const std::filesystem::path& root, std::string filename) {
  auto path = root / std::move(filename);
  writeTextFile(path, "not a timed lyric line\n[invalid]still deterministic\n");
  return path;
}

void writeDbMarker(const std::filesystem::path& dbPath) {
  writeTextFile(dbPath, "SERIONA_SCANNER_TEST_DB\n");
}

void requireEventCount(const FakeEventSink& sink, ScannerEvent::Type type, std::size_t expectedCount) {
  if (sink.eventsOfType(type).size() != expectedCount) {
    throw std::runtime_error("scanner event count mismatch");
  }
}

void requireWatcherEvent(const FakeWatcherEvent& event, FakeWatcherEvent::Type type, FakeWatcherEvent::PathKind pathKind,
                         const std::filesystem::path& path) {
  if (event.type != type || event.pathKind != pathKind || event.path != path) {
    throw std::runtime_error("watcher event mismatch");
  }
}

}
