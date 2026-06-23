#pragma once

#include "seriona/audio/audio_contracts.h"
#include "seriona/control/control_contracts.h"
#include "seriona/metadata/metadata_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace seriona::control::test {

class DeterministicClock {
public:
  using TimePoint = std::chrono::steady_clock::time_point;

  [[nodiscard]] TimePoint now() const noexcept;
  void advance(std::chrono::milliseconds delta) noexcept;
  void set(TimePoint value) noexcept;

private:
  TimePoint now_{};
};

template <typename T>
class ValueCollector {
public:
  void push(T value);
  [[nodiscard]] const std::vector<T>& values() const noexcept;
  [[nodiscard]] std::size_t count() const noexcept;
  [[nodiscard]] const T& last() const;
  void clear() noexcept;

private:
  std::vector<T> values_{};
};

using PlayerStateSnapshotCollector = ValueCollector<PlayerStateSnapshot>;
using LibraryStateSnapshotCollector = ValueCollector<LibraryStateSnapshot>;
using MediaControlCommandCollector = ValueCollector<MediaControlCommand>;
using AudioBackendEventCollector = ValueCollector<audio::BackendEvent>;
using ScannerEventCollector = ValueCollector<scanner::ScannerEvent>;

[[nodiscard]] PlayerStateSnapshot makePlayerStateSnapshot(DeterministicClock& clock, std::uint64_t version,
                                                         PlaybackStatus status = PlaybackStatus::Stopped,
                                                         std::chrono::milliseconds position = std::chrono::milliseconds{0});
[[nodiscard]] LibraryStateSnapshot makeLibraryStateSnapshot(std::uint64_t version,
                                                           LibraryScanStatus status = LibraryScanStatus::Idle);
[[nodiscard]] MediaControlCommand makePlayCommand();
[[nodiscard]] MediaControlCommand makePauseCommand();

class FakeAudioPlaybackService final : public audio::AudioPlaybackService {
public:
  void setEventSink(audio::BackendEventSink sink) override;
  void configureOutput(const audio::AudioOutputConfig& config) override;
  void loadTrack(const audio::TrackPlaybackRequest& request) override;
  void prepareNext(const audio::TrackPlaybackRequest& request) override;
  void play() override;
  void pause() override;
  void resume() override;
  void stop() override;
  void seek(std::chrono::milliseconds position) override;
  void setVolume(float linearGain) override;
  void setMuted(bool muted) override;
  void selectOutputDevice(const std::string& deviceId) override;
  [[nodiscard]] audio::PlaybackClockSnapshot queryPlaybackClock() const override;

  [[nodiscard]] std::size_t setEventSinkCalls() const noexcept;
  [[nodiscard]] std::size_t configureOutputCalls() const noexcept;
  [[nodiscard]] std::size_t loadTrackCalls() const noexcept;
  [[nodiscard]] std::size_t prepareNextCalls() const noexcept;
  [[nodiscard]] std::size_t playCalls() const noexcept;
  [[nodiscard]] std::size_t pauseCalls() const noexcept;
  [[nodiscard]] std::size_t resumeCalls() const noexcept;
  [[nodiscard]] std::size_t stopCalls() const noexcept;
  [[nodiscard]] std::size_t seekCalls() const noexcept;
  [[nodiscard]] std::size_t setVolumeCalls() const noexcept;
  [[nodiscard]] std::size_t setMutedCalls() const noexcept;
  [[nodiscard]] std::size_t selectOutputDeviceCalls() const noexcept;
  [[nodiscard]] std::size_t emitEventCalls() const noexcept;

  [[nodiscard]] const std::optional<audio::AudioOutputConfig>& lastConfiguredOutput() const noexcept;
  [[nodiscard]] const std::optional<audio::TrackPlaybackRequest>& lastLoadedTrack() const noexcept;
  [[nodiscard]] const std::optional<audio::TrackPlaybackRequest>& lastPreparedTrack() const noexcept;
  [[nodiscard]] const std::optional<std::chrono::milliseconds>& lastSeekPosition() const noexcept;
  [[nodiscard]] const std::optional<float>& lastVolume() const noexcept;
  [[nodiscard]] const std::optional<bool>& lastMuted() const noexcept;
  [[nodiscard]] const std::optional<std::string>& lastSelectedOutputDevice() const noexcept;

  void emit(audio::BackendEvent event);
  void setPlaybackClock(audio::PlaybackClockSnapshot clock);
  void loadTrackThrows(std::exception_ptr exception) noexcept;
  void loadTrackThrows(std::runtime_error exception);

private:
  audio::BackendEventSink eventSink_{};
  audio::PlaybackClockSnapshot clock_{};
  std::size_t setEventSinkCalls_{0};
  std::size_t configureOutputCalls_{0};
  std::size_t loadTrackCalls_{0};
  std::size_t prepareNextCalls_{0};
  std::size_t playCalls_{0};
  std::size_t pauseCalls_{0};
  std::size_t resumeCalls_{0};
  std::size_t stopCalls_{0};
  std::size_t seekCalls_{0};
  std::size_t setVolumeCalls_{0};
  std::size_t setMutedCalls_{0};
  std::size_t selectOutputDeviceCalls_{0};
  std::size_t emitEventCalls_{0};
  std::optional<audio::AudioOutputConfig> lastConfiguredOutput_{};
  std::optional<audio::TrackPlaybackRequest> lastLoadedTrack_{};
  std::optional<audio::TrackPlaybackRequest> lastPreparedTrack_{};
  std::optional<std::chrono::milliseconds> lastSeekPosition_{};
  std::optional<float> lastVolume_{};
  std::optional<bool> lastMuted_{};
  std::optional<std::string> lastSelectedOutputDevice_{};
  std::exception_ptr loadTrackException_{};
};

class FakeFileScannerService final : public scanner::FileScannerService {
public:
  void setEventSink(scanner::ScannerEventSink sink) override;
  void configure(const scanner::ScannerConfig& config) override;
  void scan(const std::vector<scanner::ScannerRoot>& roots, scanner::ScanMode mode) override;
  void startWatching(const std::vector<scanner::ScannerRoot>& roots) override;
  void stopWatching() override;
  void stop() override;
  [[nodiscard]] scanner::PlaylistTreeSnapshot snapshot() const override;

  [[nodiscard]] std::size_t setEventSinkCalls() const noexcept;
  [[nodiscard]] std::size_t configureCalls() const noexcept;
  [[nodiscard]] std::size_t scanCalls() const noexcept;
  [[nodiscard]] std::size_t startWatchingCalls() const noexcept;
  [[nodiscard]] std::size_t stopWatchingCalls() const noexcept;
  [[nodiscard]] std::size_t stopCalls() const noexcept;
  [[nodiscard]] std::size_t emitEventCalls() const noexcept;

  [[nodiscard]] const std::optional<scanner::ScannerConfig>& lastConfigured() const noexcept;
  [[nodiscard]] const std::optional<std::vector<scanner::ScannerRoot>>& lastScannedRoots() const noexcept;
  [[nodiscard]] const std::optional<scanner::ScanMode>& lastScanMode() const noexcept;
  [[nodiscard]] const std::optional<std::vector<scanner::ScannerRoot>>& lastWatchingRoots() const noexcept;

  void emit(scanner::ScannerEvent event);
  void setSnapshot(scanner::PlaylistTreeSnapshot snapshot);

private:
  scanner::ScannerEventSink eventSink_{};
  scanner::PlaylistTreeSnapshot snapshot_{};
  std::size_t setEventSinkCalls_{0};
  std::size_t configureCalls_{0};
  std::size_t scanCalls_{0};
  std::size_t startWatchingCalls_{0};
  std::size_t stopWatchingCalls_{0};
  std::size_t stopCalls_{0};
  std::size_t emitEventCalls_{0};
  std::optional<scanner::ScannerConfig> lastConfigured_{};
  std::optional<std::vector<scanner::ScannerRoot>> lastScannedRoots_{};
  std::optional<scanner::ScanMode> lastScanMode_{};
  std::optional<std::vector<scanner::ScannerRoot>> lastWatchingRoots_{};
};

class FakeMetadataSharingService final : public metadata::MetadataSharingService {
public:
  [[nodiscard]] metadata::MetadataBackendKind backendKind() const override;
  [[nodiscard]] metadata::MetadataBackendCapabilities capabilities() const override;
  [[nodiscard]] SubscriptionHandle registerCommandCallback(MediaControlCommandSink callback) override;
  [[nodiscard]] metadata::MetadataSyncResult start(const metadata::PlatformMediaState& state) override;
  [[nodiscard]] metadata::MetadataSyncResult update(const metadata::PlatformMediaState& state) override;
  [[nodiscard]] metadata::MetadataSyncResult stop() override;

  [[nodiscard]] std::size_t registerCommandCallbackCalls() const noexcept;
  [[nodiscard]] std::size_t commandRegistrations() const noexcept;
  [[nodiscard]] std::size_t commandUnregistrations() const noexcept;
  [[nodiscard]] std::size_t startCalls() const noexcept;
  [[nodiscard]] std::size_t updateCalls() const noexcept;
  [[nodiscard]] std::size_t stopCalls() const noexcept;
  [[nodiscard]] std::size_t emitCommandCalls() const noexcept;
  [[nodiscard]] bool hasCommandCallback() const noexcept;

  [[nodiscard]] const std::optional<metadata::PlatformMediaState>& lastStartedState() const noexcept;
  [[nodiscard]] const std::optional<metadata::PlatformMediaState>& lastUpdatedState() const noexcept;
  [[nodiscard]] const std::optional<metadata::MetadataSyncResult>& lastStartResult() const noexcept;
  [[nodiscard]] const std::optional<metadata::MetadataSyncResult>& lastUpdateResult() const noexcept;
  [[nodiscard]] const std::optional<metadata::MetadataSyncResult>& lastStopResult() const noexcept;

  void emitCommand(const MediaControlCommand& command);
  void setBackendKind(metadata::MetadataBackendKind kind) noexcept;
  void setCapabilities(metadata::MetadataBackendCapabilities capabilities) noexcept;
  void setStartResult(metadata::MetadataSyncResult result) noexcept;
  void setUpdateResult(metadata::MetadataSyncResult result) noexcept;
  void setStopResult(metadata::MetadataSyncResult result) noexcept;

private:
  struct CommandRegistration {
    std::size_t id{0};
    MediaControlCommandSink callback{};
    bool active{true};
  };

  metadata::MetadataBackendKind backendKind_{metadata::MetadataBackendKind::Noop};
  metadata::MetadataBackendCapabilities capabilities_{};
  std::size_t registerCommandCallbackCalls_{0};
  std::size_t commandRegistrations_{0};
  std::size_t commandUnregistrations_{0};
  std::size_t startCalls_{0};
  std::size_t updateCalls_{0};
  std::size_t stopCalls_{0};
  std::size_t emitCommandCalls_{0};
  std::size_t nextSubscriptionId_{1};
  std::optional<metadata::PlatformMediaState> lastStartedState_{};
  std::optional<metadata::PlatformMediaState> lastUpdatedState_{};
  std::optional<metadata::MetadataSyncResult> lastStartResult_{};
  std::optional<metadata::MetadataSyncResult> lastUpdateResult_{};
  std::optional<metadata::MetadataSyncResult> lastStopResult_{};
  metadata::MetadataSyncResult startResult_{};
  metadata::MetadataSyncResult updateResult_{};
  metadata::MetadataSyncResult stopResult_{};
  std::shared_ptr<CommandRegistration> commandRegistration_{};
};

template <typename T>
void ValueCollector<T>::push(T value) {
  values_.push_back(std::move(value));
}

template <typename T>
const std::vector<T>& ValueCollector<T>::values() const noexcept {
  return values_;
}

template <typename T>
std::size_t ValueCollector<T>::count() const noexcept {
  return values_.size();
}

template <typename T>
const T& ValueCollector<T>::last() const {
  return values_.back();
}

template <typename T>
void ValueCollector<T>::clear() noexcept {
  values_.clear();
}

}
