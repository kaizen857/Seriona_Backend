#include "control_test_harness.h"

namespace seriona::control::test {

DeterministicClock::TimePoint DeterministicClock::now() const noexcept {
  return now_;
}

void DeterministicClock::advance(std::chrono::milliseconds delta) noexcept {
  now_ += delta;
}

void DeterministicClock::set(TimePoint value) noexcept {
  now_ = value;
}

PlayerStateSnapshot makePlayerStateSnapshot(DeterministicClock& clock, std::uint64_t version, PlaybackStatus status,
                                           std::chrono::milliseconds position) {
  PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = version;
  snapshot.freshness.sampledAt = clock.now();
  snapshot.playback.state = status;
  snapshot.timeline.position = position;
  return snapshot;
}

LibraryStateSnapshot makeLibraryStateSnapshot(std::uint64_t version, LibraryScanStatus status) {
  LibraryStateSnapshot snapshot{};
  snapshot.version = version;
  snapshot.scanStatus = status;
  return snapshot;
}

MediaControlCommand makePlayCommand() {
  MediaControlCommand command{};
  command.kind = MediaControlCommandKind::Play;
  return command;
}

MediaControlCommand makePauseCommand() {
  MediaControlCommand command{};
  command.kind = MediaControlCommandKind::Pause;
  return command;
}

void FakeAudioPlaybackService::setEventSink(audio::BackendEventSink sink) {
  ++setEventSinkCalls_;
  callLog_.push_back("setEventSink");
  eventSink_ = std::move(sink);
}

void FakeAudioPlaybackService::configureOutput(const audio::AudioOutputConfig& config) {
  ++configureOutputCalls_;
  callLog_.push_back("configureOutput");
  lastConfiguredOutput_ = config;
}

void FakeAudioPlaybackService::configureTransition(const audio::TransitionConfig& config) {
  ++configureTransitionCalls_;
  callLog_.push_back("configureTransition");
  lastConfiguredTransition_ = config;
}

void FakeAudioPlaybackService::loadTrack(const audio::TrackPlaybackRequest& request) {
  if (loadTrackException_) {
    std::rethrow_exception(loadTrackException_);
  }
  ++loadTrackCalls_;
  callLog_.push_back("loadTrack");
  lastLoadedTrack_ = request;
}

void FakeAudioPlaybackService::abortTransition() {
  ++abortTransitionCalls_;
  callLog_.push_back("abortTransition");
}

void FakeAudioPlaybackService::prepareNext(const audio::TrackPlaybackRequest& request) {
  prepareNext(request, audio::PrepareNextMeta{});
}

void FakeAudioPlaybackService::prepareNext(const audio::TrackPlaybackRequest& request,
                                           const audio::PrepareNextMeta& meta) {
  ++prepareNextCalls_;
  callLog_.push_back("prepareNext");
  lastPreparedTrack_ = request;
  lastPrepareNextMeta_ = meta;
}

void FakeAudioPlaybackService::play() {
  ++playCalls_;
  callLog_.push_back("play");
}

void FakeAudioPlaybackService::pause() {
  ++pauseCalls_;
  callLog_.push_back("pause");
}

void FakeAudioPlaybackService::resume() {
  ++resumeCalls_;
  callLog_.push_back("resume");
}

void FakeAudioPlaybackService::stop() {
  ++stopCalls_;
  callLog_.push_back("stop");
}

void FakeAudioPlaybackService::seek(std::chrono::milliseconds position) {
  ++seekCalls_;
  callLog_.push_back("seek");
  lastSeekPosition_ = position;
}

void FakeAudioPlaybackService::setVolume(float linearGain) {
  ++setVolumeCalls_;
  callLog_.push_back("setVolume");
  lastVolume_ = linearGain;
}

void FakeAudioPlaybackService::setMuted(bool muted) {
  ++setMutedCalls_;
  callLog_.push_back("setMuted");
  lastMuted_ = muted;
}

void FakeAudioPlaybackService::selectOutputDevice(const std::string& deviceId) {
  ++selectOutputDeviceCalls_;
  callLog_.push_back("selectOutputDevice");
  lastSelectedOutputDevice_ = deviceId;
}

audio::PlaybackClockSnapshot FakeAudioPlaybackService::queryPlaybackClock() const {
  return clock_;
}

std::vector<audio::AudioDeviceFormat> FakeAudioPlaybackService::enumeratePlaybackDevices() const {
  return playbackDevices_;
}

std::size_t FakeAudioPlaybackService::setEventSinkCalls() const noexcept {
  return setEventSinkCalls_;
}

std::size_t FakeAudioPlaybackService::configureOutputCalls() const noexcept {
  return configureOutputCalls_;
}

std::size_t FakeAudioPlaybackService::configureTransitionCalls() const noexcept {
  return configureTransitionCalls_;
}

std::size_t FakeAudioPlaybackService::loadTrackCalls() const noexcept {
  return loadTrackCalls_;
}

std::size_t FakeAudioPlaybackService::abortTransitionCalls() const noexcept {
  return abortTransitionCalls_;
}

std::size_t FakeAudioPlaybackService::prepareNextCalls() const noexcept {
  return prepareNextCalls_;
}

std::size_t FakeAudioPlaybackService::playCalls() const noexcept {
  return playCalls_;
}

std::size_t FakeAudioPlaybackService::pauseCalls() const noexcept {
  return pauseCalls_;
}

std::size_t FakeAudioPlaybackService::resumeCalls() const noexcept {
  return resumeCalls_;
}

std::size_t FakeAudioPlaybackService::stopCalls() const noexcept {
  return stopCalls_;
}

std::size_t FakeAudioPlaybackService::seekCalls() const noexcept {
  return seekCalls_;
}

std::size_t FakeAudioPlaybackService::setVolumeCalls() const noexcept {
  return setVolumeCalls_;
}

std::size_t FakeAudioPlaybackService::setMutedCalls() const noexcept {
  return setMutedCalls_;
}

std::size_t FakeAudioPlaybackService::selectOutputDeviceCalls() const noexcept {
  return selectOutputDeviceCalls_;
}

std::size_t FakeAudioPlaybackService::emitEventCalls() const noexcept {
  return emitEventCalls_;
}

const std::optional<audio::AudioOutputConfig>& FakeAudioPlaybackService::lastConfiguredOutput() const noexcept {
  return lastConfiguredOutput_;
}

const std::optional<audio::TransitionConfig>& FakeAudioPlaybackService::lastConfiguredTransition() const noexcept {
  return lastConfiguredTransition_;
}

const std::optional<audio::TrackPlaybackRequest>& FakeAudioPlaybackService::lastLoadedTrack() const noexcept {
  return lastLoadedTrack_;
}

const std::optional<audio::TrackPlaybackRequest>& FakeAudioPlaybackService::lastPreparedTrack() const noexcept {
  return lastPreparedTrack_;
}

const std::optional<audio::PrepareNextMeta>& FakeAudioPlaybackService::lastPrepareNextMeta() const noexcept {
  return lastPrepareNextMeta_;
}

const std::optional<std::chrono::milliseconds>& FakeAudioPlaybackService::lastSeekPosition() const noexcept {
  return lastSeekPosition_;
}

const std::optional<float>& FakeAudioPlaybackService::lastVolume() const noexcept {
  return lastVolume_;
}

const std::optional<bool>& FakeAudioPlaybackService::lastMuted() const noexcept {
  return lastMuted_;
}

const std::optional<std::string>& FakeAudioPlaybackService::lastSelectedOutputDevice() const noexcept {
  return lastSelectedOutputDevice_;
}

const std::vector<std::string>& FakeAudioPlaybackService::callLog() const noexcept {
  return callLog_;
}

void FakeAudioPlaybackService::setPlaybackDevices(std::vector<audio::AudioDeviceFormat> devices) noexcept {
  playbackDevices_ = std::move(devices);
}

const std::vector<audio::AudioDeviceFormat>& FakeAudioPlaybackService::playbackDevices() const noexcept {
  return playbackDevices_;
}

void FakeAudioPlaybackService::emit(audio::BackendEvent event) {
  ++emitEventCalls_;
  if (eventSink_) {
    eventSink_(std::move(event));
  }
}

void FakeAudioPlaybackService::setPlaybackClock(audio::PlaybackClockSnapshot clock) {
  clock_ = std::move(clock);
}

void FakeAudioPlaybackService::loadTrackThrows(std::exception_ptr exception) noexcept {
  loadTrackException_ = std::move(exception);
}

void FakeAudioPlaybackService::loadTrackThrows(std::runtime_error exception) {
  loadTrackException_ = std::make_exception_ptr(std::move(exception));
}

void FakeFileScannerService::setEventSink(scanner::ScannerEventSink sink) {
  ++setEventSinkCalls_;
  eventSink_ = std::move(sink);
}

void FakeFileScannerService::configure(const scanner::ScannerConfig& config) {
  ++configureCalls_;
  lastConfigured_ = config;
}

void FakeFileScannerService::scan(const std::vector<scanner::ScannerRoot>& roots, scanner::ScanMode mode) {
  ++scanCalls_;
  lastScannedRoots_ = roots;
  lastScanMode_ = mode;
  waitIfScanBlocked();
}

void FakeFileScannerService::startWatching(const std::vector<scanner::ScannerRoot>& roots) {
  ++startWatchingCalls_;
  if (startWatchingException_) {
    lastWatchingRoots_.reset();
    std::rethrow_exception(startWatchingException_);
  }
  lastWatchingRoots_ = roots;
}

void FakeFileScannerService::stopWatching() {
  ++stopWatchingCalls_;
  lastWatchingRoots_.reset();
}

void FakeFileScannerService::stop() {
  ++stopCalls_;
}

scanner::PlaylistTreeSnapshot FakeFileScannerService::snapshot() const {
  return snapshot_;
}

bool FakeFileScannerService::removeLocation(const std::filesystem::path& path) {
  ++removeLocationCalls_;
  removeLocationPaths_.push_back(path);
  if (removeLocationException_) {
    std::rethrow_exception(removeLocationException_);
  }
  return removeLocationResult_;
}

std::size_t FakeFileScannerService::setEventSinkCalls() const noexcept {
  return setEventSinkCalls_;
}

std::size_t FakeFileScannerService::configureCalls() const noexcept {
  return configureCalls_;
}

std::size_t FakeFileScannerService::scanCalls() const noexcept {
  return scanCalls_;
}

std::size_t FakeFileScannerService::startWatchingCalls() const noexcept {
  return startWatchingCalls_;
}

std::size_t FakeFileScannerService::stopWatchingCalls() const noexcept {
  return stopWatchingCalls_;
}

std::size_t FakeFileScannerService::stopCalls() const noexcept {
  return stopCalls_;
}

std::size_t FakeFileScannerService::emitEventCalls() const noexcept {
  return emitEventCalls_;
}

std::size_t FakeFileScannerService::removeLocationCalls() const noexcept {
  return removeLocationCalls_;
}

const std::vector<std::filesystem::path>& FakeFileScannerService::removeLocationPaths() const noexcept {
  return removeLocationPaths_;
}

void FakeFileScannerService::setRemoveLocationResult(bool result) noexcept {
  removeLocationResult_ = result;
}

void FakeFileScannerService::setRemoveLocationThrows(std::exception_ptr exception) noexcept {
  removeLocationException_ = std::move(exception);
}

void FakeFileScannerService::setRemoveLocationThrows(std::runtime_error exception) {
  setRemoveLocationThrows(std::make_exception_ptr(std::move(exception)));
}

const std::optional<scanner::ScannerConfig>& FakeFileScannerService::lastConfigured() const noexcept {
  return lastConfigured_;
}

const std::optional<std::vector<scanner::ScannerRoot>>& FakeFileScannerService::lastScannedRoots() const noexcept {
  return lastScannedRoots_;
}

const std::optional<scanner::ScanMode>& FakeFileScannerService::lastScanMode() const noexcept {
  return lastScanMode_;
}

const std::optional<std::vector<scanner::ScannerRoot>>& FakeFileScannerService::lastWatchingRoots() const noexcept {
  return lastWatchingRoots_;
}

void FakeFileScannerService::emit(scanner::ScannerEvent event) {
  ++emitEventCalls_;
  if (eventSink_) {
    eventSink_(std::move(event));
  }
}

void FakeFileScannerService::setSnapshot(scanner::PlaylistTreeSnapshot snapshot) {
  snapshot_ = std::move(snapshot);
}

void FakeFileScannerService::startWatchingThrows(std::exception_ptr exception) noexcept {
  startWatchingException_ = std::move(exception);
}

void FakeFileScannerService::startWatchingThrows(std::runtime_error exception) {
  startWatchingException_ = std::make_exception_ptr(std::move(exception));
}

void FakeFileScannerService::blockScansUntilReleased() noexcept {
  blockScans_ = true;
}

void FakeFileScannerService::releaseBlockedScans() {
  {
    std::lock_guard lock{scanBlockMutex_};
    releaseScans_ = true;
  }
  scanBlockChanged_.notify_all();
}

bool FakeFileScannerService::waitForBlockedScan(std::chrono::milliseconds timeout) {
  std::unique_lock lock{scanBlockMutex_};
  return scanBlockChanged_.wait_for(lock, timeout, [this] { return scanBlocked_; });
}

void FakeFileScannerService::waitIfScanBlocked() {
  if (!blockScans_) {
    return;
  }

  std::unique_lock lock{scanBlockMutex_};
  scanBlocked_ = true;
  scanBlockChanged_.notify_all();
  scanBlockChanged_.wait(lock, [this] { return releaseScans_; });
}

metadata::MetadataBackendKind FakeMetadataSharingService::backendKind() const {
  return backendKind_;
}

metadata::MetadataBackendCapabilities FakeMetadataSharingService::capabilities() const {
  return capabilities_;
}

SubscriptionHandle FakeMetadataSharingService::registerCommandCallback(MediaControlCommandSink callback) {
  ++registerCommandCallbackCalls_;
  ++commandRegistrations_;

  auto registration = std::make_shared<CommandRegistration>();
  registration->id = nextSubscriptionId_++;
  registration->callback = std::move(callback);
  commandRegistration_ = registration;

  SubscriptionHandle handle{};
  handle.subscriptionId = registration->id;
  handle.unsubscribe = [this, weakRegistration = std::weak_ptr<CommandRegistration>{registration}] {
    if (auto shared = weakRegistration.lock()) {
      if (shared->active) {
        shared->active = false;
        shared->callback = {};
        ++commandUnregistrations_;
        if (commandRegistration_ && commandRegistration_->id == shared->id) {
          commandRegistration_.reset();
        }
      }
    }
  };
  return handle;
}

metadata::MetadataSyncResult FakeMetadataSharingService::start(const metadata::PlatformMediaState& state) {
  ++startCalls_;
  lastStartedState_ = state;
  lastStartResult_ = startResult_;
  return startResult_;
}

metadata::MetadataSyncResult FakeMetadataSharingService::update(const metadata::PlatformMediaState& state) {
  ++updateCalls_;
  lastUpdatedState_ = state;
  lastUpdateResult_ = updateResult_;
  return updateResult_;
}

metadata::MetadataSyncResult FakeMetadataSharingService::stop() {
  ++stopCalls_;
  lastStopResult_ = stopResult_;
  return stopResult_;
}

std::size_t FakeMetadataSharingService::registerCommandCallbackCalls() const noexcept {
  return registerCommandCallbackCalls_;
}

std::size_t FakeMetadataSharingService::commandRegistrations() const noexcept {
  return commandRegistrations_;
}

std::size_t FakeMetadataSharingService::commandUnregistrations() const noexcept {
  return commandUnregistrations_;
}

std::size_t FakeMetadataSharingService::startCalls() const noexcept {
  return startCalls_;
}

std::size_t FakeMetadataSharingService::updateCalls() const noexcept {
  return updateCalls_;
}

std::size_t FakeMetadataSharingService::stopCalls() const noexcept {
  return stopCalls_;
}

std::size_t FakeMetadataSharingService::emitCommandCalls() const noexcept {
  return emitCommandCalls_;
}

bool FakeMetadataSharingService::hasCommandCallback() const noexcept {
  return commandRegistration_ && commandRegistration_->active;
}

const std::optional<metadata::PlatformMediaState>& FakeMetadataSharingService::lastStartedState() const noexcept {
  return lastStartedState_;
}

const std::optional<metadata::PlatformMediaState>& FakeMetadataSharingService::lastUpdatedState() const noexcept {
  return lastUpdatedState_;
}

const std::optional<metadata::MetadataSyncResult>& FakeMetadataSharingService::lastStartResult() const noexcept {
  return lastStartResult_;
}

const std::optional<metadata::MetadataSyncResult>& FakeMetadataSharingService::lastUpdateResult() const noexcept {
  return lastUpdateResult_;
}

const std::optional<metadata::MetadataSyncResult>& FakeMetadataSharingService::lastStopResult() const noexcept {
  return lastStopResult_;
}

void FakeMetadataSharingService::emitCommand(const MediaControlCommand& command) {
  ++emitCommandCalls_;
  if (commandRegistration_ && commandRegistration_->active && commandRegistration_->callback) {
    commandRegistration_->callback(command);
  }
}

void FakeMetadataSharingService::setBackendKind(metadata::MetadataBackendKind kind) noexcept {
  backendKind_ = kind;
}

void FakeMetadataSharingService::setCapabilities(metadata::MetadataBackendCapabilities capabilities) noexcept {
  capabilities_ = capabilities;
}

void FakeMetadataSharingService::setStartResult(metadata::MetadataSyncResult result) noexcept {
  startResult_ = std::move(result);
}

void FakeMetadataSharingService::setUpdateResult(metadata::MetadataSyncResult result) noexcept {
  updateResult_ = std::move(result);
}

void FakeMetadataSharingService::setStopResult(metadata::MetadataSyncResult result) noexcept {
  stopResult_ = std::move(result);
}

}
