#include "control_test_harness.h"

#include "../../src/control/media_controller_module.h"

#include "seriona/control/folder_sort_settings_store.h"
#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;
namespace control_test = seriona::control::test;

namespace {

scanner::SongMetadata song(std::string id, std::string path, std::chrono::milliseconds duration = std::chrono::milliseconds{3000}) {
  return scanner::SongMetadata{.trackId = std::move(id),
                               .filePath = std::filesystem::path{std::move(path)},
                               .title = {},
                               .artist = {},
                               .album = {},
                               .albumArtist = {},
                               .genre = {},
                               .trackNumber = std::nullopt,
                               .discNumber = std::nullopt,
                               .year = std::nullopt,
                               .sampleRate = std::nullopt,
                               .bitDepth = std::nullopt,
                               .channels = std::nullopt,
                               .fileSizeBytes = std::nullopt,
                               .fileMtime = std::nullopt,
                               .contentHash = {},
                               .effectiveLyricsSource = scanner::LyricsSource::None,
                               .effectiveLyrics = {},
                               .externalLyricsPath = std::nullopt,
                               .externalLyricsHash = std::nullopt,
                               .externalLyricsMtime = std::nullopt,
                               .sourceFilePath = {},
                               .offset = std::nullopt,
                               .duration = duration,
                               .logicalTrackId = {},
                               .artworkPath = std::nullopt,
                               .thumbnailPath = std::nullopt};
}

scanner::SongMetadata songWithArtwork(std::string id, std::string path, std::filesystem::path artworkPath) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.artworkPath = std::move(artworkPath);
  metadata.contentHash = "artwork-hash";
  return metadata;
}

scanner::SongMetadata songWithDisplayMetadata(std::string id,
                                              std::string path,
                                              std::string title,
                                              std::string artist,
                                              std::string album,
                                              std::string albumArtist,
                                              std::string genre) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.title = std::move(title);
  metadata.artist = std::move(artist);
  metadata.album = std::move(album);
  metadata.albumArtist = std::move(albumArtist);
  metadata.genre = std::move(genre);
  return metadata;
}

scanner::SongMetadata songWithTrackNumber(std::string id,
                                          std::string path,
                                          std::uint32_t trackNumber) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.trackNumber = trackNumber;
  return metadata;
}

scanner::PlaylistNode rootNode(std::vector<std::string> children) {
  return scanner::PlaylistNode{.nodeId = "root",
                               .parentNodeId = std::nullopt,
                               .kind = scanner::PlaylistNodeKind::Root,
                               .displayName = "Library",
                               .song = std::nullopt,
                               .childNodeIds = std::move(children)};
}

scanner::PlaylistNode trackNode(std::string nodeId, scanner::SongMetadata metadata) {
  return scanner::PlaylistNode{.nodeId = std::move(nodeId),
                               .parentNodeId = std::string{"root"},
                               .kind = scanner::PlaylistNodeKind::Track,
                               .displayName = metadata.trackId,
                               .song = std::move(metadata),
                               .childNodeIds = {}};
}

scanner::PlaylistNode directoryNode(std::string nodeId,
                                    std::string parentNodeId,
                                    std::string displayName,
                                    std::vector<std::string> children) {
  return scanner::PlaylistNode{.nodeId = std::move(nodeId),
                               .parentNodeId = std::move(parentNodeId),
                               .kind = scanner::PlaylistNodeKind::Directory,
                               .displayName = std::move(displayName),
                               .song = std::nullopt,
                               .childNodeIds = std::move(children)};
}

scanner::PlaylistNode trackNodeInParent(std::string nodeId, std::string parentNodeId, scanner::SongMetadata metadata) {
  return scanner::PlaylistNode{.nodeId = std::move(nodeId),
                               .parentNodeId = std::move(parentNodeId),
                               .kind = scanner::PlaylistNodeKind::Track,
                               .displayName = metadata.trackId,
                               .song = std::move(metadata),
                               .childNodeIds = {}};
}

scanner::PlaylistTreeSnapshot libraryTree(std::vector<scanner::SongMetadata> songs, std::uint64_t version) {
  scanner::PlaylistTreeSnapshot snapshot{};
  snapshot.version = version;
  snapshot.rootNodeId = "root";
  std::vector<std::string> children;
  children.reserve(songs.size());
  for (std::size_t index = 0; index < songs.size(); ++index) {
    children.push_back("track-node-" + std::to_string(index));
  }
  snapshot.nodes.push_back(rootNode(children));
  for (std::size_t index = 0; index < songs.size(); ++index) {
    snapshot.nodes.push_back(trackNode(children[index], std::move(songs[index])));
  }
  return snapshot;
}

scanner::PlaylistTreeSnapshot contextLibraryTree(std::uint64_t version = 50) {
  scanner::PlaylistTreeSnapshot snapshot{};
  snapshot.version = version;
  snapshot.rootNodeId = "root";
  snapshot.nodes = {rootNode({"dir:a", "dir:b", "dir:empty"}),
                    directoryNode("dir:a", "root", "Folder A", {"track:a-01", "track:a-02", "track:a-03"}),
                    directoryNode("dir:b", "root", "Folder B", {"track:b-01", "track:b-02"}),
                    directoryNode("dir:empty", "root", "Empty", {}),
                    trackNodeInParent("track:a-01", "dir:a", song("a-01", "music/folder-a/01.flac")),
                    trackNodeInParent("track:a-02", "dir:a", song("a-02", "music/folder-a/02.flac")),
                    trackNodeInParent("track:a-03", "dir:a", song("a-03", "music/folder-a/03.flac")),
                    trackNodeInParent("track:b-01", "dir:b", song("b-01", "music/folder-b/01.flac")),
                    trackNodeInParent("track:b-02", "dir:b", song("b-02", "music/folder-b/02.flac"))};
  return snapshot;
}

scanner::PlaylistTreeSnapshot contextLibraryTreeWithFolderA(std::vector<scanner::SongMetadata> folderASongs,
                                                            std::uint64_t version,
                                                            bool includeFolderA = true) {
  scanner::PlaylistTreeSnapshot snapshot{};
  snapshot.version = version;
  snapshot.rootNodeId = "root";

  std::vector<std::string> rootChildren;
  if (includeFolderA) {
    rootChildren.push_back("dir:a");
  }
  rootChildren.push_back("dir:b");
  snapshot.nodes.push_back(rootNode(std::move(rootChildren)));

  if (includeFolderA) {
    std::vector<std::string> folderAChildren;
    folderAChildren.reserve(folderASongs.size());
    for (const auto& metadata : folderASongs) {
      folderAChildren.push_back("track:" + metadata.trackId);
    }
    snapshot.nodes.push_back(directoryNode("dir:a", "root", "Folder A", std::move(folderAChildren)));
    for (auto& metadata : folderASongs) {
      const auto nodeId = "track:" + metadata.trackId;
      snapshot.nodes.push_back(trackNodeInParent(nodeId, "dir:a", std::move(metadata)));
    }
  }

  snapshot.nodes.push_back(directoryNode("dir:b", "root", "Folder B", {"track:b-01", "track:b-02"}));
  snapshot.nodes.push_back(trackNodeInParent("track:b-01", "dir:b", song("b-01", "music/folder-b/01.flac")));
  snapshot.nodes.push_back(trackNodeInParent("track:b-02", "dir:b", song("b-02", "music/folder-b/02.flac")));
  return snapshot;
}

scanner::PlaylistTreeSnapshot contextLibraryTreeWithSubfolder(std::uint64_t version = 60) {
  scanner::PlaylistTreeSnapshot snapshot{};
  snapshot.version = version;
  snapshot.rootNodeId = "root";
  snapshot.nodes = {rootNode({"dir:a", "dir:b"}),
                    directoryNode("dir:a", "root", "Folder A", {"dir:a-sub", "track:a-01", "track:a-02"}),
                    directoryNode("dir:a-sub", "dir:a", "Sub A", {"track:a-sub-01"}),
                    directoryNode("dir:b", "root", "Folder B", {"track:b-01"}),
                    trackNodeInParent("track:a-sub-01", "dir:a-sub", song("a-sub-01", "music/folder-a/sub/01.flac")),
                    trackNodeInParent("track:a-01", "dir:a", song("a-01", "music/folder-a/01.flac")),
                    trackNodeInParent("track:a-02", "dir:a", song("a-02", "music/folder-a/02.flac")),
                    trackNodeInParent("track:b-01", "dir:b", song("b-01", "music/folder-b/01.flac"))};
  return snapshot;
}

scanner::ScannerEvent scannerSnapshotEvent(scanner::PlaylistTreeSnapshot snapshot, std::uint64_t eventVersion) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::PlaylistSnapshotUpdated,
                               .monotonicVersion = eventVersion,
                               .timestamp = {},
                               .payload = std::move(snapshot)};
}

scanner::ScannerEvent scanStartedEvent(std::uint64_t version) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanStarted,
                               .monotonicVersion = version,
                               .timestamp = {},
                               .payload = scanner::ScanProgress{}};
}

scanner::ScannerEvent progressUpdatedEvent(scanner::ScanProgress progress, std::uint64_t version) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::ProgressUpdated,
                               .monotonicVersion = version,
                               .timestamp = {},
                               .payload = std::move(progress)};
}

scanner::ScannerEvent fileScannedEvent(scanner::SongMetadata metadata, std::uint64_t version) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::FileScanned,
                               .monotonicVersion = version,
                               .timestamp = {},
                               .payload = std::move(metadata)};
}

scanner::ScannerEvent scannerErrorEvent(std::string message, std::uint64_t version,
                                        scanner::ScannerErrorCode code = scanner::ScannerErrorCode::MetadataReadFailed) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanError,
                               .monotonicVersion = version,
                               .timestamp = {},
                               .payload = scanner::ScannerError{.code = code,
                                                                .message = std::move(message),
                                                                .detail = {},
                                                                .path = std::nullopt}};
}

audio::BackendEvent audioTrackChangedEvent(std::string id, std::string path, std::uint64_t version) {
  auto request = audio::TrackPlaybackRequest{.trackId = std::move(id),
                                             .filePath = std::filesystem::path{std::move(path)},
                                             .title = {},
                                             .artist = {},
                                             .offset = std::nullopt,
                                             .duration = std::chrono::milliseconds{3000},
                                             .sampleRate = std::nullopt,
                                             .bitDepth = std::nullopt,
                                             .channels = std::nullopt,
                                             .format = std::nullopt};
  return audio::BackendEvent{.type = audio::BackendEventType::TrackChanged,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::TrackChanged{.request = std::move(request)}};
}

audio::BackendEvent audioPositionUpdatedEvent(std::string id, std::chrono::milliseconds position, std::uint64_t version) {
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackPositionUpdated,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackPositionUpdated{.clock = audio::PlaybackClockSnapshot{.trackId = std::move(id),
                                                                                                               .position = position,
                                                                                                               .sampledAt = {},
                                                                                                               .version = version,
                                                                                                               .continuous = true}}};
}

audio::BackendEvent audioPlaybackStateChangedEvent(audio::PlaybackState state, std::uint64_t version) {
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackStateChanged,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackStateChanged{.state = state}};
}

audio::BackendEvent audioPlaybackEndedEvent(std::string id, std::string path, std::uint64_t version,
                                            std::chrono::milliseconds position = std::chrono::milliseconds{3000}) {
  auto request = audio::TrackPlaybackRequest{.trackId = std::move(id),
                                             .filePath = std::filesystem::path{std::move(path)},
                                             .title = {},
                                             .artist = {},
                                             .offset = std::nullopt,
                                             .duration = std::chrono::milliseconds{3000},
                                             .sampleRate = std::nullopt,
                                             .bitDepth = std::nullopt,
                                             .channels = std::nullopt,
                                             .format = std::nullopt};
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackEnded,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackEnded{.request = request,
                                                             .finalClock = audio::PlaybackClockSnapshot{.trackId = request.trackId,
                                                                                                         .position = position,
                                                                                                         .sampledAt = {},
                                                                                                         .version = version,
                                                                                                         .continuous = false}}};
}

MediaControlCommand command(MediaControlCommandKind kind) {
  MediaControlCommand value{};
  value.kind = kind;
  return value;
}

class FakeFolderSortSettingsStore final : public FolderSortSettingsStore {
public:
  void upsert(FolderSortSetting setting) override {
    ++upsertAttempts_;
    throwIfConfigured();
    ++upsertCalls_;
    settings_.push_back(std::move(setting));
  }

  [[nodiscard]] std::optional<FolderSortSetting> load(const std::filesystem::path& rootPath,
                                                      const std::string& folderNodeId) const override {
    throwIfConfigured();
    ++loadCalls_;
    throwIfLoadConfigured();
    for (const auto& setting : settings_) {
      if (setting.rootPath == rootPath && setting.folderNodeId == folderNodeId) {
        return setting;
      }
    }
    return std::nullopt;
  }

  void remove(const std::filesystem::path& rootPath, const std::string& folderNodeId) override {
    throwIfConfigured();
    ++removeCalls_;
    std::erase_if(settings_, [&](const FolderSortSetting& setting) {
      return setting.rootPath == rootPath && setting.folderNodeId == folderNodeId;
    });
  }

  [[nodiscard]] std::vector<FolderSortSetting> list(const std::filesystem::path& rootPath) const override {
    throwIfConfigured();
    ++listCalls_;
    std::vector<FolderSortSetting> matches;
    for (const auto& setting : settings_) {
      if (setting.rootPath == rootPath) {
        matches.push_back(setting);
      }
    }
    return matches;
  }

  void throwOnAccess(bool value = true) noexcept { throwOnAccess_ = value; }
  void throwOnLoad(FolderSortSettingsErrorCode code, std::string message) {
    loadFailureCode_ = code;
    loadFailureMessage_ = std::move(message);
  }
  void seed(FolderSortSetting setting) { settings_.push_back(std::move(setting)); }

  [[nodiscard]] std::size_t upsertAttempts() const noexcept { return upsertAttempts_; }
  [[nodiscard]] std::size_t upsertCalls() const noexcept { return upsertCalls_; }
  [[nodiscard]] std::size_t loadCalls() const noexcept { return loadCalls_; }
  [[nodiscard]] std::size_t listCalls() const noexcept { return listCalls_; }
  [[nodiscard]] std::optional<FolderSortSetting> lastUpsert() const {
    if (settings_.empty()) {
      return std::nullopt;
    }
    return settings_.back();
  }
  [[nodiscard]] std::size_t totalCalls() const noexcept { return upsertCalls_ + loadCalls_ + removeCalls_ + listCalls_; }

private:
  void throwIfConfigured() const {
    if (throwOnAccess_) {
      throw FolderSortSettingsError{FolderSortSettingsErrorCode::StorageError, "fake folder sort store failure"};
    }
  }

  void throwIfLoadConfigured() const {
    if (loadFailureCode_.has_value()) {
      throw FolderSortSettingsError{*loadFailureCode_, loadFailureMessage_};
    }
  }

  std::vector<FolderSortSetting> settings_{};
  mutable std::size_t upsertAttempts_{0};
  mutable std::size_t upsertCalls_{0};
  mutable std::size_t loadCalls_{0};
  mutable std::size_t removeCalls_{0};
  mutable std::size_t listCalls_{0};
  bool throwOnAccess_{false};
  std::optional<FolderSortSettingsErrorCode> loadFailureCode_{};
  std::string loadFailureMessage_{};
};

TrackIdentity track(std::string id, std::string path) {
  return TrackIdentity{.trackId = std::move(id), .filePath = std::filesystem::path{std::move(path)}, .sourceId = {}, .libraryId = {}};
}

class FakeArtworkResolveService final : public ArtworkResolveService {
public:
  void request(ArtworkResolveRequest request) noexcept override {
    std::lock_guard lock{mutex_};
    requests_.push_back(std::move(request));
  }

  void setResultCallback(ArtworkResolveCallback callback) noexcept override {
    std::lock_guard lock{mutex_};
    callback_ = std::move(callback);
  }

  void stop() noexcept override {
    std::lock_guard lock{mutex_};
    stopped_ = true;
  }

  void complete(ArtworkResolveResultView view) {
    ArtworkResolveCallback callback;
    {
      std::lock_guard lock{mutex_};
      callback = callback_;
    }
    if (callback) {
      callback(std::move(view));
    }
  }

  [[nodiscard]] std::size_t requestCount() const {
    std::lock_guard lock{mutex_};
    return requests_.size();
  }

  [[nodiscard]] ArtworkResolveRequest lastRequest() const {
    std::lock_guard lock{mutex_};
    return requests_.back();
  }

  [[nodiscard]] std::vector<ArtworkResolveRequest> requests() const {
    std::lock_guard lock{mutex_};
    return requests_;
  }

  [[nodiscard]] bool stopped() const {
    std::lock_guard lock{mutex_};
    return stopped_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<ArtworkResolveRequest> requests_{};
  ArtworkResolveCallback callback_{};
  bool stopped_{false};
};

PlaybackContextDescriptor folderContextForRoot(std::filesystem::path rootPath, std::string folderNodeId, TrackIdentity anchor) {
  return PlaybackContextDescriptor{.scope = PlaybackContextScope::Folder,
                                   .rootPath = std::move(rootPath),
                                   .folderNodeId = std::move(folderNodeId),
                                   .anchorTrack = std::move(anchor),
                                   .sortRules = {}};
}

PlaybackContextDescriptor folderContext(std::string folderNodeId, TrackIdentity anchor) {
  return folderContextForRoot("/library", std::move(folderNodeId), std::move(anchor));
}

std::vector<FolderSortRule> trackNumberDescendingRules() {
  return {FolderSortRule{.field = FolderSortField::TrackNumber,
                         .direction = FolderSortDirection::Descending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}};
}

FolderSortSetting savedTrackNumberDescendingSetting(std::filesystem::path rootPath, std::string folderNodeId = "dir:a") {
  return FolderSortSetting{.rootPath = std::move(rootPath),
                           .folderNodeId = std::move(folderNodeId),
                           .rules = trackNumberDescendingRules()};
}

MediaControlCommand startPlaybackFromContext(PlaybackContextDescriptor descriptor) {
  auto value = command(MediaControlCommandKind::StartPlaybackFromContext);
  value.playbackContext = std::move(descriptor);
  return value;
}

MediaControlCommand applyFolderSortRules(FolderSortSetting setting) {
  auto value = command(MediaControlCommandKind::ApplyFolderSortRules);
  value.folderSortSetting = std::move(setting);
  return value;
}

std::filesystem::path uniqueMediaControllerDatabasePath() {
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  return std::filesystem::temp_directory_path() / ("seriona-media-controller-sort-store-" + suffix + ".sqlite");
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

bool hasNotification(const std::vector<ControlDomainNotification>& notifications,
                     ControlDomainNotificationKind kind,
                     MediaControllerErrorCode errorCode) {
  return std::ranges::any_of(notifications, [kind, errorCode](const ControlDomainNotification& notification) {
    return notification.kind == kind && notification.errorCode == errorCode;
  });
}

bool hasNotificationKind(const std::vector<ControlDomainNotification>& notifications,
                         ControlDomainNotificationKind kind) {
  return std::ranges::any_of(notifications, [kind](const ControlDomainNotification& notification) {
    return notification.kind == kind;
  });
}

std::size_t notificationKindCount(const std::vector<ControlDomainNotification>& notifications,
                                  ControlDomainNotificationKind kind) {
  std::size_t count = 0;
  for (const auto& notification : notifications) {
    if (notification.kind == kind) {
      ++count;
    }
  }
  return count;
}

struct ControllerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  std::shared_ptr<FakeFolderSortSettingsStore> fakeFolderSortSettingsStore{std::make_shared<FakeFolderSortSettingsStore>()};
  control_test::FakeMetadataSharingService* fakeMetadata{nullptr};
  std::unique_ptr<MediaController> controller{};

  ControllerFixture() {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService),
                                                                 .folderSortSettingsStore = fakeFolderSortSettingsStore},
                                     MediaControllerOptions{.runInlineForTests = true});
  }

  explicit ControllerFixture(MediaControllerOptions options) {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService),
                                                                 .folderSortSettingsStore = fakeFolderSortSettingsStore},
                                     options);
  }
};

void installLibrary(ControllerFixture& fixture, std::uint64_t treeVersion = 20, std::uint64_t eventVersion = 1) {
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac")}, treeVersion),
                                                eventVersion));
  fixture.controller->drainForTests();
}

scanner::SongMetadata songWithThumbnail(std::string id, std::string path, std::filesystem::path thumbnailPath) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.thumbnailPath = std::move(thumbnailPath);
  return metadata;
}

scanner::SongMetadata cueSong(std::string id,
                              std::string cuePath,
                              std::string audioPath,
                              std::chrono::milliseconds offset,
                              std::chrono::milliseconds duration,
                              std::filesystem::path thumbnailPath) {
  auto metadata = song(std::move(id), std::move(cuePath), duration);
  metadata.sourceFilePath = std::move(audioPath);
  metadata.offset = offset;
  metadata.artist = "Cue Artist";
  metadata.album = "Cue Album";
  metadata.thumbnailPath = std::move(thumbnailPath);
  return metadata;
}

ArtworkResolveResultView artworkResult(std::uint64_t generation,
                                       TrackIdentity identity,
                                       ArtworkResolveOutcomeKind kind,
                                       std::filesystem::path fullPath = {}) {
  ArtworkResolveOutcomeView outcome{};
  outcome.kind = kind;
  if (kind == ArtworkResolveOutcomeKind::FullPath) {
    outcome.fullPath = std::move(fullPath);
  }
  return ArtworkResolveResultView{.generation = generation, .identity = std::move(identity), .outcome = std::move(outcome)};
}

audio::BackendEvent cueTrackChangedEvent(std::string id,
                                         std::string audioPath,
                                         std::chrono::milliseconds offset,
                                         std::chrono::milliseconds duration,
                                         std::uint64_t version) {
  auto event = audioTrackChangedEvent(std::move(id), std::move(audioPath), version);
  auto& changed = std::get<audio::TrackChanged>(event.payload);
  changed.request.offset = offset;
  changed.request.duration = duration;
  changed.request.boundedSegment = true;
  return event;
}

struct ArtworkControllerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  std::shared_ptr<FakeFolderSortSettingsStore> fakeFolderSortSettingsStore{std::make_shared<FakeFolderSortSettingsStore>()};
  std::shared_ptr<FakeArtworkResolveService> fakeArtwork{std::make_shared<FakeArtworkResolveService>()};
  control_test::FakeMetadataSharingService* fakeMetadata{nullptr};
  std::unique_ptr<MediaController> controller{};

  ArtworkControllerFixture() {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService),
                                                                 .folderSortSettingsStore = fakeFolderSortSettingsStore,
                                                                 .artworkResolver = fakeArtwork},
                                     MediaControllerOptions{.runInlineForTests = true});
  }
};

}

TEST_CASE("media controller dependencies default a missing folder sort settings store during construction") {
  auto fakeAudio = std::make_shared<control_test::FakeAudioPlaybackService>();
  auto fakeScanner = std::make_shared<control_test::FakeFileScannerService>();
  auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();

  {
    auto controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                      .scanner = fakeScanner,
                                                                      .metadata = std::move(metadataService),
                                                                      .folderSortSettingsStore = {}},
                                         MediaControllerOptions{.runInlineForTests = true});
    CHECK(fakeAudio->setEventSinkCalls() == 1U);
    CHECK(fakeScanner->setEventSinkCalls() == 1U);
  }

  CHECK(fakeAudio->setEventSinkCalls() == 2U);
  CHECK(fakeScanner->setEventSinkCalls() == 2U);
}

TEST_CASE("media controller dependencies carry an explicit fake folder sort settings store") {
  ControllerFixture fixture{};

  fixture.controller->start();
  installLibrary(fixture);
  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(playResult.accepted);
  CHECK(fixture.fakeFolderSortSettingsStore->totalCalls() == 0U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
}

TEST_CASE("media controller preserves zero-offset normal tracks without marking them as bounded segments") {
  ControllerFixture fixture{};
  fixture.controller->start();

  auto plainSong = song("mp3-track", "music/song.mp3", std::chrono::milliseconds{293760});
  plainSong.sourceFilePath = plainSong.filePath;
  plainSong.offset = std::chrono::milliseconds{0};

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({std::move(plainSong)}, 20), 1));
  fixture.controller->drainForTests();

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(playResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  REQUIRE(fixture.fakeAudio->lastLoadedTrack()->offset.has_value());
  CHECK(*fixture.fakeAudio->lastLoadedTrack()->offset == std::chrono::milliseconds{0});
  CHECK(fixture.fakeAudio->lastLoadedTrack()->duration == std::chrono::milliseconds{293760});
  CHECK_FALSE(fixture.fakeAudio->lastLoadedTrack()->boundedSegment);
}

TEST_CASE("media controller keeps state stable when an injected fake folder sort store would fail") {
  ControllerFixture fixture{};
  fixture.fakeFolderSortSettingsStore->throwOnAccess();
  fixture.controller->start();
  installLibrary(fixture);
  const auto before = fixture.controller->playerStateSnapshot();

  auto invalidSeek = command(MediaControlCommandKind::SeekTo);
  const auto result = fixture.controller->submitCommand(invalidSeek);

  CHECK_FALSE(result.accepted);
  CHECK(result.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(fixture.fakeFolderSortSettingsStore->totalCalls() == 0U);
  const auto after = fixture.controller->playerStateSnapshot();
  CHECK(after.playback.state == before.playback.state);
  REQUIRE(before.currentTrack.has_value());
  REQUIRE(after.currentTrack.has_value());
  CHECK(after.currentTrack->trackId == before.currentTrack->trackId);
}

TEST_CASE("media controller applies folder sort rules through the injected store and publishes a state signal") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  const auto result = fixture.controller->submitCommand(applyFolderSortRules(FolderSortSetting{
      .rootPath = "/library/../library",
      .folderNodeId = "dir:a",
      .rules = {FolderSortRule{.field = FolderSortField::Artist,
                               .direction = FolderSortDirection::Descending,
                               .missingValuePolicy = FolderSortMissingValuePolicy::First}}}));

  REQUIRE(result.accepted);
  CHECK(result.code == MediaControllerErrorCode::None);
  CHECK(fixture.fakeFolderSortSettingsStore->upsertAttempts() == 1U);
  CHECK(fixture.fakeFolderSortSettingsStore->upsertCalls() == 1U);
  REQUIRE(fixture.fakeFolderSortSettingsStore->lastUpsert().has_value());
  const auto stored = *fixture.fakeFolderSortSettingsStore->lastUpsert();
  CHECK(stored.rootPath == std::filesystem::path{"/library"});
  CHECK(stored.folderNodeId == "dir:a");
  REQUIRE(stored.rules.size() == 1U);
  CHECK(stored.rules.front().field == FolderSortField::Artist);
  CHECK(stored.rules.front().direction == FolderSortDirection::Descending);
  CHECK(stored.rules.front().missingValuePolicy == FolderSortMissingValuePolicy::First);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotificationKind(notifications, ControlDomainNotificationKind::FolderSortRulesApplied);
  }));
  {
    std::lock_guard lock{notificationMutex};
    const auto applied = std::ranges::find_if(notifications, [](const ControlDomainNotification& notification) {
      return notification.kind == ControlDomainNotificationKind::FolderSortRulesApplied;
    });
    REQUIRE(applied != notifications.end());
    CHECK(applied->errorCode == MediaControllerErrorCode::None);
    REQUIRE(applied->folderSortSetting.has_value());
    CHECK(applied->folderSortSetting->rootPath == std::filesystem::path{"/library"});
    CHECK(applied->folderSortSetting->folderNodeId == "dir:a");
    REQUIRE(applied->folderSortSetting->rules.size() == 1U);
    CHECK(applied->folderSortSetting->rules.front().field == FolderSortField::Artist);
    CHECK_FALSE(hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::InvalidCommand));
  }
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller scanLibrary replays saved folder sort rules for scanned roots") {
  ControllerFixture fixture{};
  fixture.fakeFolderSortSettingsStore->seed(savedTrackNumberDescendingSetting("/library"));
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  scanner::ScannerRoot root{};
  root.path = "/library";
  root.recursive = true;
  const auto result = fixture.controller->scanLibrary({root}, scanner::ScanMode::Full);

  REQUIRE(result.accepted);
  CHECK(fixture.fakeFolderSortSettingsStore->listCalls() == 1U);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotificationKind(notifications, ControlDomainNotificationKind::FolderSortRulesApplied);
  }));
  {
    std::lock_guard lock{notificationMutex};
    const auto applied = std::ranges::find_if(notifications, [](const ControlDomainNotification& notification) {
      return notification.kind == ControlDomainNotificationKind::FolderSortRulesApplied;
    });
    REQUIRE(applied != notifications.end());
    REQUIRE(applied->folderSortSetting.has_value());
    CHECK(applied->folderSortSetting->rootPath == std::filesystem::path{"/library"});
    CHECK(applied->folderSortSetting->folderNodeId == "dir:a");
    REQUIRE(applied->folderSortSetting->rules.size() == 1U);
    CHECK(applied->folderSortSetting->rules.front().field == FolderSortField::TrackNumber);
    CHECK(applied->folderSortSetting->rules.front().direction == FolderSortDirection::Descending);
  }
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller rejects malformed folder sort commands before store upsert") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  auto setting = FolderSortSetting{.rootPath = "/library",
                                   .folderNodeId = "dir:a",
                                   .rules = {FolderSortRule{.field = FolderSortField::Title,
                                                            .direction = FolderSortDirection::Ascending,
                                                            .missingValuePolicy = FolderSortMissingValuePolicy::Last}}};

  SUBCASE("missing root path") {
    setting.rootPath.clear();
  }
  SUBCASE("empty folder node id") {
    setting.folderNodeId.clear();
  }
  SUBCASE("empty rules") {
    setting.rules.clear();
  }
  SUBCASE("unsupported rule enum") {
    setting.rules.front().field = static_cast<FolderSortField>(999);
  }

  const auto before = fixture.controller->playerStateSnapshot();
  const auto result = fixture.controller->submitCommand(applyFolderSortRules(std::move(setting)));

  CHECK_FALSE(result.accepted);
  CHECK(result.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(fixture.fakeFolderSortSettingsStore->upsertAttempts() == 0U);
  CHECK(fixture.fakeFolderSortSettingsStore->upsertCalls() == 0U);
  CHECK_FALSE(fixture.fakeFolderSortSettingsStore->lastUpsert().has_value());
  const auto after = fixture.controller->playerStateSnapshot();
  CHECK(after.playback.state == before.playback.state);
  CHECK(after.currentTrack.has_value() == before.currentTrack.has_value());
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::InvalidCommand);
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK_FALSE(hasNotificationKind(notifications, ControlDomainNotificationKind::FolderSortRulesApplied));
  }
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller reports folder sort store failures without changing playback context") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 64));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);
  const auto before = fixture.controller->playerStateSnapshot();
  const auto loadCallsBeforeFailure = fixture.fakeAudio->loadTrackCalls();
  const auto playCallsBeforeFailure = fixture.fakeAudio->playCalls();
  const auto stopCallsBeforeFailure = fixture.fakeAudio->stopCalls();
  fixture.fakeFolderSortSettingsStore->throwOnAccess();

  const auto result = fixture.controller->submitCommand(applyFolderSortRules(FolderSortSetting{
      .rootPath = "/library",
      .folderNodeId = "dir:a",
      .rules = {FolderSortRule{.field = FolderSortField::TrackNumber,
                               .direction = FolderSortDirection::Ascending,
                               .missingValuePolicy = FolderSortMissingValuePolicy::Last}}}));

  CHECK_FALSE(result.accepted);
  CHECK(result.code == MediaControllerErrorCode::BackendRejected);
  CHECK(fixture.fakeFolderSortSettingsStore->upsertAttempts() == 1U);
  CHECK(fixture.fakeFolderSortSettingsStore->upsertCalls() == 0U);
  CHECK_FALSE(fixture.fakeFolderSortSettingsStore->lastUpsert().has_value());
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeFailure);
  CHECK(fixture.fakeAudio->playCalls() == playCallsBeforeFailure);
  CHECK(fixture.fakeAudio->stopCalls() == stopCallsBeforeFailure);
  const auto after = fixture.controller->playerStateSnapshot();
  CHECK(after.playback.state == before.playback.state);
  REQUIRE(before.currentTrack.has_value());
  REQUIRE(after.currentTrack.has_value());
  CHECK(after.currentTrack->trackId == before.currentTrack->trackId);
  CHECK(after.currentTrack->filePath == before.currentTrack->filePath);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::BackendRejected);
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK_FALSE(hasNotificationKind(notifications, ControlDomainNotificationKind::FolderSortRulesApplied));
  }

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  notificationSubscription.unsubscribe();
}

TEST_CASE("production media controller dependencies own a sqlite folder sort settings store using database path") {
  const auto databasePath = uniqueMediaControllerDatabasePath();
  std::error_code cleanupError{};
  std::filesystem::remove(databasePath, cleanupError);

  auto dependencies = makeProductionMediaControllerDependencies(databasePath, std::filesystem::temp_directory_path());

  REQUIRE(dependencies.folderSortSettingsStore != nullptr);
  CHECK(std::filesystem::exists(databasePath));
  dependencies.folderSortSettingsStore->upsert(FolderSortSetting{.rootPath = "/music/root",
                                                                 .folderNodeId = "folder-a",
                                                                 .rules = {FolderSortRule{.field = FolderSortField::Album,
                                                                                          .direction = FolderSortDirection::Descending,
                                                                                          .missingValuePolicy = FolderSortMissingValuePolicy::First}}});

  auto reopenedStore = makeSQLiteFolderSortSettingsStore(FolderSortSettingsStoreConfig{.databasePath = databasePath});
  const auto loaded = reopenedStore->load("/music/root", "folder-a");

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->rules.size() == 1U);
  CHECK(loaded->rules.front().field == FolderSortField::Album);
  CHECK(loaded->rules.front().direction == FolderSortDirection::Descending);
  CHECK(loaded->rules.front().missingValuePolicy == FolderSortMissingValuePolicy::First);
  reopenedStore.reset();
  dependencies.folderSortSettingsStore.reset();
  std::filesystem::remove(databasePath, cleanupError);
}

TEST_CASE("media controller facade drives fake audio load before play for play and select") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(playResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.fakeAudio->playCalls() == 1U);

  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("b", "music/b.flac");
  const auto selectResult = fixture.controller->submitCommand(select);

  CHECK(selectResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 2U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b");
  CHECK(fixture.fakeAudio->playCalls() == 2U);
}

TEST_CASE("media controller facade rejects unplayable commands and publishes command notifications") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationsSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK_FALSE(playResult.accepted);
  CHECK(playResult.code == MediaControllerErrorCode::NoPlayableTrack);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::NoPlayableTrack);
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK(hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::NoPlayableTrack));
  }

  installLibrary(fixture);
  auto invalidSelect = command(MediaControlCommandKind::SelectTrack);
  invalidSelect.track = track("missing", "music/missing.flac");
  const auto selectResult = fixture.controller->submitCommand(invalidSelect);

  CHECK_FALSE(selectResult.accepted);
  CHECK(selectResult.code == MediaControllerErrorCode::TrackNotInLibrary);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::TrackNotInLibrary);
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK(hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::TrackNotInLibrary));
  }
  notificationsSubscription.unsubscribe();
}

TEST_CASE("media controller facade forwards seek volume mute and toggle commands through fake audio") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  auto seekPastDuration = command(MediaControlCommandKind::SeekTo);
  seekPastDuration.position = std::chrono::milliseconds{4500};
  const auto seekPastResult = fixture.controller->submitCommand(seekPastDuration);
  CHECK(seekPastResult.accepted);
  CHECK(fixture.fakeAudio->seekCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastSeekPosition().has_value());
  CHECK(*fixture.fakeAudio->lastSeekPosition() == std::chrono::milliseconds{3000});
  CHECK(fixture.controller->playerStateSnapshot().timeline.position == std::chrono::milliseconds{3000});

  auto seekBeforeStart = command(MediaControlCommandKind::SeekBy);
  seekBeforeStart.delta = std::chrono::milliseconds{-5000};
  const auto seekBeforeResult = fixture.controller->submitCommand(seekBeforeStart);
  CHECK(seekBeforeResult.accepted);
  CHECK(fixture.fakeAudio->seekCalls() == 2U);
  REQUIRE(fixture.fakeAudio->lastSeekPosition().has_value());
  CHECK(*fixture.fakeAudio->lastSeekPosition() == std::chrono::milliseconds{0});

  auto setVolume = command(MediaControlCommandKind::SetVolume);
  setVolume.volume = 1.75F;
  const auto volumeResult = fixture.controller->submitCommand(setVolume);
  CHECK(volumeResult.accepted);
  CHECK(fixture.fakeAudio->setVolumeCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastVolume().has_value());
  CHECK(*fixture.fakeAudio->lastVolume() == 1.0F);
  CHECK(fixture.controller->playerStateSnapshot().volume == 1.0F);

  auto setMuted = command(MediaControlCommandKind::SetMuted);
  setMuted.muted = true;
  const auto mutedResult = fixture.controller->submitCommand(setMuted);
  CHECK(mutedResult.accepted);
  CHECK(fixture.fakeAudio->setMutedCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastMuted().has_value());
  CHECK(*fixture.fakeAudio->lastMuted());
  CHECK(fixture.controller->playerStateSnapshot().muted);

  const auto toggleResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));
  CHECK(toggleResult.accepted);
  CHECK(fixture.fakeAudio->pauseCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);

  const auto resumeResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));
  CHECK(resumeResult.accepted);
  CHECK(fixture.fakeAudio->resumeCalls() == 1U);
  CHECK(fixture.fakeAudio->playCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller keeps visible playback state stable during seek while playing") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  auto seek = command(MediaControlCommandKind::SeekTo);
  seek.position = std::chrono::milliseconds{1500};
  REQUIRE(fixture.controller->submitCommand(seek).accepted);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);

  fixture.fakeAudio->emit(audioPlaybackStateChangedEvent(audio::PlaybackState::Loading, 40));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);

  fixture.fakeAudio->emit(audioPlaybackStateChangedEvent(audio::PlaybackState::Playing, 41));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller facade applies skip repeat and playback-ended policies through fake audio") {
  ControllerFixture fixture{};
  std::atomic_size_t playbackEndedNotifications{0};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    if (notification.kind == ControlDomainNotificationKind::PlaybackEnded) {
      playbackEndedNotifications.fetch_add(1U);
    }
  });
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  const auto skipNextResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  CHECK(skipNextResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b");

  auto repeatAll = command(MediaControlCommandKind::SetRepeatMode);
  repeatAll.repeatMode = RepeatMode::All;
  CHECK(fixture.controller->submitCommand(repeatAll).accepted);
  const auto wrapNextResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  CHECK(wrapNextResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");

  auto repeatOne = command(MediaControlCommandKind::SetRepeatMode);
  repeatOne.repeatMode = RepeatMode::One;
  CHECK(fixture.controller->submitCommand(repeatOne).accepted);
  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a", "music/a.flac", 10));
  fixture.controller->drainForTests();

  CHECK(waitUntil([&] { return fixture.fakeAudio->loadTrackCalls() > 0U; }));
  CHECK(playbackEndedNotifications.load() == 0U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller SelectTrack fallback still loads the requested track from a folder tree") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 50));
  fixture.controller->drainForTests();

  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("a-02", "music/folder-a/02.flac");
  const auto selectResult = fixture.controller->submitCommand(select);

  CHECK(selectResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");
  const auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");
  CHECK(player.playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller keeps idle and SelectTrack default fallback compatibility explicit") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 56));
  fixture.controller->drainForTests();

  auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-01");
  CHECK(player.playback.state == PlaybackStatus::Stopped);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);

  REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-01");
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("b-01", "music/folder-b/01.flac");
  REQUIRE(fixture.controller->submitCommand(select).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b-01");
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b-02");
}

TEST_CASE("media controller folder playback context keeps next and previous inside the transient order") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 51));
  fixture.controller->drainForTests();

  const auto startResult = fixture.controller->submitCommand(
      startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))));
  CHECK(startResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  const auto previousResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious));
  CHECK(previousResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-01");

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");

  const auto loadCallsBeforeEnd = fixture.fakeAudio->loadTrackCalls();
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeEnd);
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller StartPlaybackFromContext owns sorted next and previous navigation") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                               91),
                                                91));
  fixture.controller->drainForTests();
  auto descriptor = folderContext("dir:a", track("a-02", "music/folder-a/02.flac"));
  descriptor.sortRules = trackNumberDescendingRules();

  const auto startResult = fixture.controller->submitCommand(startPlaybackFromContext(std::move(descriptor)));
  CHECK(startResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-01");
}

TEST_CASE("media controller SelectTrack default context fallback does not skip into sibling folders") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 52));
  fixture.controller->drainForTests();

  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("a-02", "music/folder-a/02.flac");
  REQUIRE(fixture.controller->submitCommand(select).accepted);

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");

  const auto loadCallsBeforeEnd = fixture.fakeAudio->loadTrackCalls();
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeEnd);
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller repeat-one skip does not re-enter full-tree lookup after active context drift") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 57));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);
  auto repeatOne = command(MediaControlCommandKind::SetRepeatMode);
  repeatOne.repeatMode = RepeatMode::One;
  REQUIRE(fixture.controller->submitCommand(repeatOne).accepted);
  const auto loadCallsBeforeDrift = fixture.fakeAudio->loadTrackCalls();

  fixture.fakeAudio->emit(audioTrackChangedEvent("b-01", "music/folder-b/01.flac", 91));
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeDrift);
  const auto loadCallsBeforeSkip = fixture.fakeAudio->loadTrackCalls();

  const auto skipResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext));

  CHECK(skipResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSkip);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller shuffle candidates are limited to the current playback context order") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 53));
  fixture.controller->drainForTests();

  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);
  auto enableShuffle = command(MediaControlCommandKind::SetShuffle);
  enableShuffle.shuffle = true;
  REQUIRE(fixture.controller->submitCommand(enableShuffle).accepted);

  std::set<std::string> playedTrackIds{"a-01"};
  for (int index = 0; index < 2; ++index) {
    CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
    REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
    const auto& trackId = fixture.fakeAudio->lastLoadedTrack()->trackId;
    CHECK(trackId != "b-01");
    CHECK(trackId != "b-02");
    playedTrackIds.insert(trackId);
  }

  CHECK(playedTrackIds == std::set<std::string>{"a-01", "a-02", "a-03"});
  const auto loadCallsBeforeExhaustedShuffle = fixture.fakeAudio->loadTrackCalls();
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeExhaustedShuffle);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller playback-ended advances only within the current playback context") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 54));
  fixture.controller->drainForTests();

  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-02", "music/folder-a/02.flac", 80));
  fixture.controller->drainForTests();
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);

  const auto loadCallsBeforeEnd = fixture.fakeAudio->loadTrackCalls();
  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-03", "music/folder-a/03.flac", 81));
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeEnd);
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller sequential playback notifies only when the list ends") {
  ControllerFixture fixture{};
  std::atomic_size_t playbackEndedNotifications{0};
  auto subscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    if (notification.kind == ControlDomainNotificationKind::PlaybackEnded) {
      playbackEndedNotifications.fetch_add(1U);
    }
  });
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 55));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-01", "music/folder-a/01.flac", 80));
  fixture.controller->drainForTests();
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  CHECK(playbackEndedNotifications.load() == 0U);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-02", "music/folder-a/02.flac", 81));
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  CHECK(playbackEndedNotifications.load() == 0U);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-03", "music/folder-a/03.flac", 82));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
  REQUIRE(waitUntil([&] { return playbackEndedNotifications.load() == 1U; }));
  CHECK(playbackEndedNotifications.load() == 1U);
  subscription.unsubscribe();
}

TEST_CASE("media controller list-loop playback wraps to the folder's first direct track on end") {
  ControllerFixture fixture{};
  std::atomic_size_t playbackEndedNotifications{0};
  auto subscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    if (notification.kind == ControlDomainNotificationKind::PlaybackEnded) {
      playbackEndedNotifications.fetch_add(1U);
    }
  });
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithSubfolder(), 56));
  fixture.controller->drainForTests();

  auto repeatAll = command(MediaControlCommandKind::SetRepeatMode);
  repeatAll.repeatMode = RepeatMode::All;
  REQUIRE(fixture.controller->submitCommand(repeatAll).accepted);

  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-01", "music/folder-a/01.flac", 80));
  fixture.controller->drainForTests();
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");
  CHECK(playbackEndedNotifications.load() == 0U);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-02", "music/folder-a/02.flac", 81));
  fixture.controller->drainForTests();
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-01");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  REQUIRE(waitUntil([&] { return playbackEndedNotifications.load() == 1U; }));
  CHECK(playbackEndedNotifications.load() == 1U);
  subscription.unsubscribe();
}

TEST_CASE("media controller shuffle playback always picks a random next track on end") {
  ControllerFixture fixture{};
  std::atomic_size_t playbackEndedNotifications{0};
  auto subscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    if (notification.kind == ControlDomainNotificationKind::PlaybackEnded) {
      playbackEndedNotifications.fetch_add(1U);
    }
  });
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 57));
  fixture.controller->drainForTests();

  auto enableShuffle = command(MediaControlCommandKind::SetShuffle);
  enableShuffle.shuffle = true;
  REQUIRE(fixture.controller->submitCommand(enableShuffle).accepted);
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);

  std::set<std::string> played{"a-01"};
  for (int index = 0; index < 6; ++index) {
    REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
    const auto current = fixture.fakeAudio->lastLoadedTrack()->trackId;
    fixture.fakeAudio->emit(audioPlaybackEndedEvent(current, "music/folder-a/" + current.substr(2) + ".flac", 90 + index));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
    played.insert(fixture.fakeAudio->lastLoadedTrack()->trackId);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  }
  CHECK(played == std::set<std::string>{"a-01", "a-02", "a-03"});
  CHECK(playbackEndedNotifications.load() == 0U);
  subscription.unsubscribe();
}

TEST_CASE("media controller ignores playback-ended events for a stale previous track") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({song("a-01", "music/folder-a/01.flac"),
                                                                                song("a-02", "music/folder-a/02.flac"),
                                                                                song("a-03", "music/folder-a/03.flac"),
                                                                                song("a-04", "music/folder-a/04.flac")},
                                                                               95),
                                                95));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-02", "music/folder-a/02.flac", 120));
  fixture.controller->drainForTests();
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  const auto loadCallsAfterValidEnd = fixture.fakeAudio->loadTrackCalls();

  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a-02", "music/folder-a/02.flac", 121));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsAfterValidEnd);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  const auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-03");
  CHECK(player.playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller StartPlaybackFromContext baseline uses tree order unless explicit command rules are supplied") {
  ControllerFixture withoutRules{};
  withoutRules.controller->start();
  withoutRules.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                     songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                     songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                                    86),
                                                     86));
  withoutRules.controller->drainForTests();

  REQUIRE(withoutRules.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);
  CHECK(withoutRules.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(withoutRules.fakeAudio->lastLoadedTrack().has_value());
  CHECK(withoutRules.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  ControllerFixture withExplicitRules{};
  withExplicitRules.controller->start();
  withExplicitRules.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                          songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                          songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                                         87),
                                                          87));
  withExplicitRules.controller->drainForTests();
  auto explicitContext = folderContext("dir:a", track("a-01", "music/folder-a/01.flac"));
  explicitContext.sortRules = trackNumberDescendingRules();

  REQUIRE(withExplicitRules.controller->submitCommand(startPlaybackFromContext(std::move(explicitContext))).accepted);
  CHECK(withExplicitRules.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious)).accepted);
  REQUIRE(withExplicitRules.fakeAudio->lastLoadedTrack().has_value());
  CHECK(withExplicitRules.fakeAudio->lastLoadedTrack()->trackId == "a-02");
  CHECK(withExplicitRules.fakeFolderSortSettingsStore->loadCalls() == 0U);
}

TEST_CASE("media controller loads saved folder rules for matching root and folder when context omits rules") {
  ControllerFixture fixture{};
  fixture.fakeFolderSortSettingsStore->seed(savedTrackNumberDescendingSetting("/library"));
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                               88),
                                                88));
  fixture.controller->drainForTests();

  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);

  CHECK(fixture.fakeFolderSortSettingsStore->loadCalls() == 1U);
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");
}

TEST_CASE("media controller saved folder rules are keyed by root path and folder node id") {
  ControllerFixture fixture{};
  fixture.fakeFolderSortSettingsStore->seed(savedTrackNumberDescendingSetting("/other-library"));
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                               89),
                                                89));
  fixture.controller->drainForTests();

  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContextForRoot("/library", "dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);

  CHECK(fixture.fakeFolderSortSettingsStore->loadCalls() == 1U);
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  ControllerFixture matchingRoot{};
  matchingRoot.fakeFolderSortSettingsStore->seed(savedTrackNumberDescendingSetting("/other-library"));
  matchingRoot.controller->start();
  matchingRoot.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                     songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                     songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                                    90),
                                                     90));
  matchingRoot.controller->drainForTests();

  REQUIRE(matchingRoot.controller->submitCommand(
              startPlaybackFromContext(folderContextForRoot("/other-library", "dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);
  CHECK(matchingRoot.fakeFolderSortSettingsStore->loadCalls() == 1U);
  CHECK(matchingRoot.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious)).accepted);
  REQUIRE(matchingRoot.fakeAudio->lastLoadedTrack().has_value());
  CHECK(matchingRoot.fakeAudio->lastLoadedTrack()->trackId == "a-02");
}

TEST_CASE("media controller snapshot reconcile reuses saved rules loaded into the active context") {
  ControllerFixture fixture{};
  fixture.fakeFolderSortSettingsStore->seed(savedTrackNumberDescendingSetting("/library"));
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                               91),
                                                91));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);
  const auto loadCallsAfterStart = fixture.fakeFolderSortSettingsStore->loadCalls();
  const auto loadCallsBeforeSnapshot = fixture.fakeAudio->loadTrackCalls();

  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3),
                                                                                songWithTrackNumber("a-04", "music/folder-a/04.flac", 4)},
                                                                               92),
                                                92));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  CHECK(fixture.fakeFolderSortSettingsStore->loadCalls() == loadCallsAfterStart);
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-02");
}

TEST_CASE("media controller falls back to tree order when saved folder rules are missing or corrupt") {
  ControllerFixture missing{};
  missing.controller->start();
  missing.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                               93),
                                                93));
  missing.controller->drainForTests();

  REQUIRE(missing.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);
  CHECK(missing.fakeFolderSortSettingsStore->loadCalls() == 1U);
  CHECK(missing.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(missing.fakeAudio->lastLoadedTrack().has_value());
  CHECK(missing.fakeAudio->lastLoadedTrack()->trackId == "a-02");

  ControllerFixture corrupt{};
  corrupt.fakeFolderSortSettingsStore->throwOnLoad(FolderSortSettingsErrorCode::InvalidRulesJson,
                                                   "corrupt saved folder sort rules");
  corrupt.controller->start();
  corrupt.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({songWithTrackNumber("a-01", "music/folder-a/01.flac", 1),
                                                                                songWithTrackNumber("a-02", "music/folder-a/02.flac", 2),
                                                                                songWithTrackNumber("a-03", "music/folder-a/03.flac", 3)},
                                                                               94),
                                                94));
  corrupt.controller->drainForTests();

  REQUIRE(corrupt.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-01", "music/folder-a/01.flac"))))
              .accepted);
  CHECK(corrupt.fakeFolderSortSettingsStore->loadCalls() == 1U);
  CHECK(corrupt.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(corrupt.fakeAudio->lastLoadedTrack().has_value());
  CHECK(corrupt.fakeAudio->lastLoadedTrack()->trackId == "a-02");
}

TEST_CASE("media controller snapshot reconcile keeps current track and rebuilds its context index") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 56));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);
  const auto loadCallsBeforeSnapshot = fixture.fakeAudio->loadTrackCalls();

  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({song("a-02", "music/folder-a/02.flac"),
                                                                                song("a-03", "music/folder-a/03.flac")},
                                                                               57),
                                                57));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");
  CHECK(player.playback.state == PlaybackStatus::Playing);

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipPrevious)).accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
}

TEST_CASE("media controller snapshot reconcile advances to successor when current context anchor is removed") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 58));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);
  const auto loadCallsBeforeSnapshot = fixture.fakeAudio->loadTrackCalls();

  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({song("a-01", "music/folder-a/01.flac"),
                                                                                song("a-03", "music/folder-a/03.flac")},
                                                                               59),
                                                59));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot + 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a-03");
  const auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-03");
  CHECK(player.playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller snapshot reconcile stops and clears context when folder becomes empty") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 60));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);
  const auto loadCallsBeforeSnapshot = fixture.fakeAudio->loadTrackCalls();
  const auto stopCallsBeforeSnapshot = fixture.fakeAudio->stopCalls();

  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({}, 61), 61));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  CHECK(fixture.fakeAudio->stopCalls() == stopCallsBeforeSnapshot + 1U);
  auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");
  CHECK(player.playback.state == PlaybackStatus::Stopped);

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");
  CHECK(player.playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller snapshot reconcile stops and clears context when folder node is missing") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 62));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(
              startPlaybackFromContext(folderContext("dir:a", track("a-02", "music/folder-a/02.flac"))))
              .accepted);
  const auto loadCallsBeforeSnapshot = fixture.fakeAudio->loadTrackCalls();
  const auto stopCallsBeforeSnapshot = fixture.fakeAudio->stopCalls();

  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTreeWithFolderA({}, 63, false), 63));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  CHECK(fixture.fakeAudio->stopCalls() == stopCallsBeforeSnapshot + 1U);
  auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");
  CHECK(player.playback.state == PlaybackStatus::Stopped);

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext)).accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeSnapshot);
  player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a-02");
  CHECK(player.playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("media controller handles missing or invalid playback context order without rebuilding the full tree") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(contextLibraryTree(), 55));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "a-01");

  const auto skipResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  CHECK(skipResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "a-01");

  auto invalidDescriptor = folderContext("dir:a", track("a-01", "music/folder-a/01.flac"));
  invalidDescriptor.rootPath.clear();
  const auto invalidResult = fixture.controller->submitCommand(startPlaybackFromContext(std::move(invalidDescriptor)));
  CHECK_FALSE(invalidResult.accepted);
  CHECK(invalidResult.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);

  const auto deletedAnchorResult = fixture.controller->submitCommand(
      startPlaybackFromContext(folderContext("dir:a", track("deleted", "music/folder-a/deleted.flac"))));
  CHECK_FALSE(deletedAnchorResult.accepted);
  CHECK(deletedAnchorResult.code == MediaControllerErrorCode::TrackNotInLibrary);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
}

TEST_CASE("media controller propagates scanner artwork to player snapshot metadata") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithArtwork("a", "music/a.flac", "/tmp/seriona-cover-a.png")}, 21), 21));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto snapshot = fixture.controller->playerStateSnapshot();

  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/tmp/seriona-cover-a.png"});
  CHECK(snapshot.artwork->contentHash == std::string{"artwork-hash"});
}

TEST_CASE("media controller preserves scanner display metadata for platform snapshots") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithDisplayMetadata("riot",
                                                                                      "music/R・I・O・T.flac",
                                                                                      "R·I·O·T",
                                                                                      "RAISE A SUILEN",
                                                                                      "R・I・O・T",
                                                                                      "RAISE A SUILEN",
                                                                                      "Rock")},
                                                       22),
                                               22));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto snapshot = fixture.controller->playerStateSnapshot();

  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->title == "R·I·O·T");
  CHECK(snapshot.display->artist == "RAISE A SUILEN");
  CHECK(snapshot.display->album == "R・I・O・T");
  CHECK(snapshot.display->albumArtist == "RAISE A SUILEN");
  CHECK(snapshot.display->genre == "Rock");

  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  const auto& publishedSnapshot = fixture.fakeMetadata->lastUpdatedState()->controlState;
  REQUIRE(publishedSnapshot.display.has_value());
  CHECK(publishedSnapshot.display->album == "R・I・O・T");
}

TEST_CASE("media controller does not replace scanner album with parent directory name") {
  ControllerFixture fixture{};
  fixture.controller->start();
  constexpr auto kFolderAlbum = "[M3-44] ARForest - The Unfinished [FLAC]";
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithDisplayMetadata("arforest-01",
                                                                                      std::string{"music/"} + kFolderAlbum + "/01 - Abandoned Creation.flac",
                                                                                      "Abandoned Creation",
                                                                                      "ARForest",
                                                                                      "The Unfinished",
                                                                                      "ARForest",
                                                                                      "Soundtrack")},
                                                       23),
                                               23));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto snapshot = fixture.controller->playerStateSnapshot();

  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->album == "The Unfinished");
  CHECK(snapshot.display->album != kFolderAlbum);

  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  const auto& publishedSnapshot = fixture.fakeMetadata->lastUpdatedState()->controlState;
  REQUIRE(publishedSnapshot.display.has_value());
  CHECK(publishedSnapshot.display->album == "The Unfinished");
  CHECK(publishedSnapshot.display->album != kFolderAlbum);
}

TEST_CASE("media controller preserves scanner display metadata after audio track changed event") {
  ControllerFixture fixture{};
  fixture.controller->start();
  constexpr auto kFolderAlbum = "[M3-44] ARForest - The Unfinished [FLAC]";
  const auto path = std::string{"music/"} + kFolderAlbum + "/01 - Abandoned Creation.flac";
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithDisplayMetadata("arforest-01",
                                                                                      path,
                                                                                      "Abandoned Creation",
                                                                                      "ARForest",
                                                                                      "The Unfinished",
                                                                                      "ARForest",
                                                                                      "Soundtrack")},
                                                       24),
                                               24));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  fixture.fakeAudio->emit(audioTrackChangedEvent("arforest-01", path, 25));
  fixture.controller->drainForTests();

  const auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->title == "Abandoned Creation");
  CHECK(snapshot.display->artist == "ARForest");
  CHECK(snapshot.display->album == "The Unfinished");
  CHECK(snapshot.display->album != kFolderAlbum);

  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  const auto& publishedSnapshot = fixture.fakeMetadata->lastUpdatedState()->controlState;
  REQUIRE(publishedSnapshot.display.has_value());
  CHECK(publishedSnapshot.display->album == "The Unfinished");
  CHECK(publishedSnapshot.display->album != kFolderAlbum);
}

TEST_CASE("media controller facade shuffle produces deterministic selected tracks") {
  ControllerFixture first{};
  ControllerFixture second{};
  first.controller->start();
  second.controller->start();
  installLibrary(first, 20, 1);
  installLibrary(second, 20, 1);
  first.controller->submitCommand(command(MediaControlCommandKind::Play));
  second.controller->submitCommand(command(MediaControlCommandKind::Play));

  auto enableShuffle = command(MediaControlCommandKind::SetShuffle);
  enableShuffle.shuffle = true;
  CHECK(first.controller->submitCommand(enableShuffle).accepted);
  CHECK(second.controller->submitCommand(enableShuffle).accepted);
  const auto firstSkip = first.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  const auto secondSkip = second.controller->submitCommand(command(MediaControlCommandKind::SkipNext));

  CHECK(firstSkip.accepted);
  CHECK(secondSkip.accepted);
  REQUIRE(first.fakeAudio->lastLoadedTrack().has_value());
  REQUIRE(second.fakeAudio->lastLoadedTrack().has_value());
  CHECK(first.fakeAudio->lastLoadedTrack()->trackId == second.fakeAudio->lastLoadedTrack()->trackId);
}

TEST_CASE("media controller facade scans library, starts watching, and publishes committed library snapshots") {
  ControllerFixture fixture{};
  std::mutex librarySnapshotMutex{};
  std::vector<LibraryStateSnapshot> librarySnapshots{};
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](LibraryStateSnapshot snapshot) {
    std::lock_guard lock{librarySnapshotMutex};
    librarySnapshots.push_back(std::move(snapshot));
  });
  fixture.controller->start();

  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  const auto scanResult = fixture.controller->scanLibrary(roots, scanner::ScanMode::Full);

  CHECK(scanResult.accepted);
  CHECK(fixture.fakeScanner->scanCalls() == 1U);
  REQUIRE(fixture.fakeScanner->lastScannedRoots().has_value());
  CHECK(fixture.fakeScanner->lastScannedRoots()->front().path == std::filesystem::path{"music"});
  REQUIRE(fixture.fakeScanner->lastScanMode().has_value());
  CHECK(*fixture.fakeScanner->lastScanMode() == scanner::ScanMode::Full);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 1U);
  REQUIRE(fixture.fakeScanner->lastWatchingRoots().has_value());
  REQUIRE(fixture.fakeScanner->lastWatchingRoots()->size() == 1U);
  CHECK(fixture.fakeScanner->lastWatchingRoots()->front().path == std::filesystem::path{"music"});
  CHECK(fixture.fakeScanner->lastWatchingRoots()->front().recursive);

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac")}, 33), 2));
  fixture.controller->drainForTests();

  REQUIRE(waitUntil([&] {
    std::lock_guard lock{librarySnapshotMutex};
    return librarySnapshots.size() >= 2U;
  }));
  {
    std::lock_guard lock{librarySnapshotMutex};
    CHECK(librarySnapshots.back().version == 33U);
    REQUIRE(librarySnapshots.back().libraryTree.has_value());
    CHECK(librarySnapshots.back().libraryTree->version == 33U);
  }
  librarySubscription.unsubscribe();
}

TEST_CASE("media controller scan progress uses aggregate skipped progress events instead of FileScanned") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scanStartedEvent(1));
  fixture.controller->drainForTests();
  fixture.fakeScanner->emit(fileScannedEvent(song("cache-hit", "music/cache-hit.flac"), 2));
  fixture.controller->drainForTests();

  {
    std::lock_guard lock{notificationMutex};
    CHECK(notificationKindCount(notifications, ControlDomainNotificationKind::LibraryScanProgressUpdated) == 0U);
  }

  scanner::ScanProgress skippedOnly{};
  skippedOnly.filesDiscovered = 5;
  skippedOnly.filesScanned = 0;
  skippedOnly.filesSkipped = 5;
  fixture.fakeScanner->emit(progressUpdatedEvent(skippedOnly, 3));
  fixture.controller->drainForTests();

  const auto progressSnapshot = fixture.controller->libraryStateSnapshot();
  REQUIRE(progressSnapshot.scanProgress.has_value());
  CHECK(progressSnapshot.scanProgress->filesDiscovered == 5U);
  CHECK(progressSnapshot.scanProgress->filesScanned == 0U);
  CHECK(progressSnapshot.scanProgress->filesSkipped == 5U);
	  CHECK(progressSnapshot.scanProgress->filesScanned + progressSnapshot.scanProgress->filesSkipped ==
	        progressSnapshot.scanProgress->filesDiscovered);
	  CHECK(waitUntil([&] {
	    std::lock_guard lock{notificationMutex};
	    return notificationKindCount(notifications, ControlDomainNotificationKind::LibraryScanProgressUpdated) == 1U;
	  }));

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("restored", "music/restored.flac")}, 44), 4));
  fixture.controller->drainForTests();

  const auto snapshot = fixture.controller->libraryStateSnapshot();
  REQUIRE(snapshot.libraryTree.has_value());
  CHECK(snapshot.libraryTree->version == 44U);

  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller repeated full scans restart watching through the scanner facade") {
  ControllerFixture fixture{};
  fixture.controller->start();

  const std::vector<scanner::ScannerRoot> initialRoots{{.path = std::filesystem::path{"music-a"}, .recursive = true}};
  const auto firstScan = fixture.controller->scanLibrary(initialRoots, scanner::ScanMode::Full);

  REQUIRE(firstScan.accepted);
  CHECK(fixture.fakeScanner->scanCalls() == 1U);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 1U);
  REQUIRE(fixture.fakeScanner->lastWatchingRoots().has_value());
  CHECK(fixture.fakeScanner->lastWatchingRoots()->front().path == std::filesystem::path{"music-a"});

  const std::vector<scanner::ScannerRoot> replacementRoots{{.path = std::filesystem::path{"music-b"}, .recursive = false},
                                                           {.path = std::filesystem::path{"music-c"}, .recursive = true}};
  const auto secondScan = fixture.controller->scanLibrary(replacementRoots, scanner::ScanMode::Full);

  REQUIRE(secondScan.accepted);
  CHECK(fixture.fakeScanner->scanCalls() == 2U);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 2U);
  REQUIRE(fixture.fakeScanner->lastWatchingRoots().has_value());
  REQUIRE(fixture.fakeScanner->lastWatchingRoots()->size() == 2U);
  CHECK(fixture.fakeScanner->lastWatchingRoots()->at(0).path == std::filesystem::path{"music-b"});
  CHECK_FALSE(fixture.fakeScanner->lastWatchingRoots()->at(0).recursive);
  CHECK(fixture.fakeScanner->lastWatchingRoots()->at(1).path == std::filesystem::path{"music-c"});
  CHECK(fixture.fakeScanner->lastWatchingRoots()->at(1).recursive);
}

TEST_CASE("media controller incremental scans also start watching through the scanner facade") {
  ControllerFixture fixture{};
  fixture.controller->start();

  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  const auto result = fixture.controller->scanLibrary(roots, scanner::ScanMode::Incremental);

  REQUIRE(result.accepted);
  CHECK(fixture.fakeScanner->scanCalls() == 1U);
  REQUIRE(fixture.fakeScanner->lastScanMode().has_value());
  CHECK(*fixture.fakeScanner->lastScanMode() == scanner::ScanMode::Incremental);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 1U);
  REQUIRE(fixture.fakeScanner->lastWatchingRoots().has_value());
  REQUIRE(fixture.fakeScanner->lastWatchingRoots()->size() == 1U);
  CHECK(fixture.fakeScanner->lastWatchingRoots()->front().path == std::filesystem::path{"music"});
}

TEST_CASE("media controller shutdown stops scanner watching") {
  ControllerFixture fixture{};
  fixture.controller->start();
  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  REQUIRE(fixture.controller->scanLibrary(roots, scanner::ScanMode::Full).accepted);
  REQUIRE(fixture.fakeScanner->lastWatchingRoots().has_value());

  fixture.controller->shutdown();

  CHECK(fixture.fakeScanner->stopWatchingCalls() == 1U);
  CHECK_FALSE(fixture.fakeScanner->lastWatchingRoots().has_value());
}

TEST_CASE("media controller reports watcher start failure without stale watched roots or playback mutation") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music-a/a.flac")}, 70), 70));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto before = fixture.controller->playerStateSnapshot();
  const auto loadCallsBeforeFailure = fixture.fakeAudio->loadTrackCalls();
  const auto playCallsBeforeFailure = fixture.fakeAudio->playCalls();
  const auto stopCallsBeforeFailure = fixture.fakeAudio->stopCalls();

  const std::vector<scanner::ScannerRoot> initialRoots{{.path = std::filesystem::path{"music-a"}, .recursive = true}};
  REQUIRE(fixture.controller->scanLibrary(initialRoots, scanner::ScanMode::Full).accepted);
  REQUIRE(fixture.fakeScanner->lastWatchingRoots().has_value());
  fixture.fakeScanner->startWatchingThrows(std::runtime_error{"fake watcher start failed"});

  const std::vector<scanner::ScannerRoot> replacementRoots{{.path = std::filesystem::path{"music-b"}, .recursive = true}};
  const auto result = fixture.controller->scanLibrary(replacementRoots, scanner::ScanMode::Full);

  CHECK_FALSE(result.accepted);
  CHECK(result.code == MediaControllerErrorCode::BackendRejected);
  CHECK(result.message.find("start scanner watcher") != std::string::npos);
  CHECK(fixture.fakeScanner->scanCalls() == 2U);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 2U);
  CHECK_FALSE(fixture.fakeScanner->lastWatchingRoots().has_value());
  CHECK(fixture.fakeAudio->loadTrackCalls() == loadCallsBeforeFailure);
  CHECK(fixture.fakeAudio->playCalls() == playCallsBeforeFailure);
  CHECK(fixture.fakeAudio->stopCalls() == stopCallsBeforeFailure);
  const auto after = fixture.controller->playerStateSnapshot();
  CHECK(after.playback.state == before.playback.state);
  REQUIRE(before.currentTrack.has_value());
  REQUIRE(after.currentTrack.has_value());
  CHECK(after.currentTrack->trackId == before.currentTrack->trackId);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::BackendRejected);
  }));
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller facade does not run scanner work on the control executor") {
  ControllerFixture fixture{MediaControllerOptions{.runInlineForTests = false}};
  fixture.fakeScanner->blockScansUntilReleased();
  fixture.controller->start();
  installLibrary(fixture);
  for (auto attempts = 0; attempts < 100 && !fixture.controller->libraryStateSnapshot().libraryTree.has_value(); ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  for (auto attempts = 0; attempts < 100 && fixture.fakeAudio->playCalls() == 0U; ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.fakeAudio->playCalls() == 1U);

  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  auto scanResult = std::async(std::launch::async, [&] {
    return fixture.controller->scanLibrary(roots, scanner::ScanMode::Full);
  });
  REQUIRE(fixture.fakeScanner->waitForBlockedScan(std::chrono::seconds{1}));

  fixture.fakeAudio->emit(audioPositionUpdatedEvent("a", std::chrono::milliseconds{1250}, 8));
  auto pauseResult = std::async(std::launch::async, [&] {
    return fixture.controller->submitCommand(command(MediaControlCommandKind::Pause));
  });

  REQUIRE(scanResult.wait_for(std::chrono::seconds{1}) == std::future_status::timeout);
  REQUIRE(pauseResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(pauseResult.get().accepted);
  for (auto attempts = 0; attempts < 100 && fixture.controller->playerStateSnapshot().timeline.position != std::chrono::milliseconds{1250}; ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  CHECK(fixture.fakeAudio->pauseCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);
  CHECK(fixture.controller->playerStateSnapshot().timeline.position == std::chrono::milliseconds{1250});

  fixture.fakeScanner->releaseBlockedScans();
  REQUIRE(scanResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(scanResult.get().accepted);
}

TEST_CASE("media controller facade exposes first scanned track while stopped") {
  ControllerFixture fixture{};
  std::promise<PlayerStateSnapshot> publishedTrackSnapshot{};
  auto trackSnapshot = publishedTrackSnapshot.get_future();
  std::atomic_bool trackSnapshotCaptured{false};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot snapshot) {
    if (snapshot.currentTrack.has_value() && !trackSnapshotCaptured.exchange(true)) {
      publishedTrackSnapshot.set_value(std::move(snapshot));
    }
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac")}, 33), 2));
  fixture.controller->drainForTests();

  const auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a");
  CHECK(player.playback.state == PlaybackStatus::Stopped);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  REQUIRE(trackSnapshot.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  const auto publishedSnapshot = trackSnapshot.get();
  REQUIRE(publishedSnapshot.currentTrack.has_value());
  CHECK(publishedSnapshot.currentTrack->trackId == "a");

  const auto toggleResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));

  CHECK(toggleResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.fakeAudio->playCalls() == 1U);
  playerSubscription.unsubscribe();
}

TEST_CASE("media controller facade ignores stale audio and scanner events") {
  ControllerFixture fixture{};
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("new", "music/new.flac")}, 30), 30));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  CHECK(fixture.controller->libraryStateSnapshot().libraryTree->nodes.size() == 2U);
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("old", "music/old.flac")}, 20), 20));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  CHECK(fixture.controller->libraryStateSnapshot().libraryTree->version == 30U);

  fixture.fakeAudio->emit(audioTrackChangedEvent("fresh", "music/fresh.flac", 8));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "fresh");
  fixture.fakeAudio->emit(audioTrackChangedEvent("stale", "music/stale.flac", 7));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "fresh");
}

TEST_CASE("media controller facade publishes fatal scanner errors as library state and domain notifications") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerErrorEvent("root unavailable", 5, scanner::ScannerErrorCode::RootUnavailable));
  fixture.controller->drainForTests();

  const auto librarySnapshot = fixture.controller->libraryStateSnapshot();
  CHECK(librarySnapshot.version == 5U);
  CHECK(librarySnapshot.scanStatus == LibraryScanStatus::Error);
  REQUIRE(librarySnapshot.lastError.has_value());
  CHECK(librarySnapshot.lastError->message == "root unavailable");
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return !notifications.empty();
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK(notifications.back().kind == ControlDomainNotificationKind::LibraryScanError);
    CHECK(notifications.back().errorCode == MediaControllerErrorCode::BackendRejected);
    CHECK(notifications.back().scanStatus == LibraryScanStatus::Error);
  }
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller facade keeps scanning state for non-fatal file errors") {
  ControllerFixture fixture{};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerErrorEvent("metadata read failed", 5));
  fixture.controller->drainForTests();

  const auto librarySnapshot = fixture.controller->libraryStateSnapshot();
  CHECK(librarySnapshot.version == 5U);
  CHECK(librarySnapshot.scanStatus == LibraryScanStatus::Idle);
  REQUIRE(librarySnapshot.lastError.has_value());
  CHECK(librarySnapshot.lastError->message == "metadata read failed");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  {
    std::lock_guard lock{notificationMutex};
    CHECK_FALSE(hasNotification(notifications, ControlDomainNotificationKind::LibraryScanError,
                                MediaControllerErrorCode::BackendRejected));
  }
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller facade starts metadata and updates after committed player snapshot") {
  ControllerFixture fixture{};
  fixture.controller->start();

  CHECK(fixture.fakeMetadata->registerCommandCallbackCalls() == 1U);
  CHECK(fixture.fakeMetadata->startCalls() == 1U);
  REQUIRE(fixture.fakeMetadata->lastStartedState().has_value());
  CHECK(fixture.fakeMetadata->lastStartedState()->controlState.playback.state == PlaybackStatus::Stopped);

  installLibrary(fixture, 44, 1);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(fixture.fakeMetadata->updateCalls() >= 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState()->controlState.currentTrack.has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.currentTrack->trackId == "a");
}

TEST_CASE("media controller facade posts metadata commands onto the control executor") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);

  fixture.fakeMetadata->emitCommand(command(MediaControlCommandKind::Play));

  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  CHECK(fixture.fakeAudio->playCalls() == 1U);
}

TEST_CASE("media controller facade routes metadata pause like direct queued commands") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  fixture.fakeMetadata->emitCommand(command(MediaControlCommandKind::Pause));

  CHECK(fixture.fakeAudio->pauseCalls() == 0U);
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->pauseCalls() == 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Paused);
}

TEST_CASE("media controller facade shutdown unregisters callbacks and drops late events") {
  ControllerFixture fixture{};
  control_test::PlayerStateSnapshotCollector playerSnapshots{};
  control_test::LibraryStateSnapshotCollector librarySnapshots{};
  std::size_t notificationDeliveries{0};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](const PlayerStateSnapshot& snapshot) { playerSnapshots.push(snapshot); });
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](const LibraryStateSnapshot& snapshot) { librarySnapshots.push(snapshot); });
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](const ControlDomainNotification&) { ++notificationDeliveries; });
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  const auto playerSnapshotBeforeShutdown = fixture.controller->playerStateSnapshot();
  const auto librarySnapshotBeforeShutdown = fixture.controller->libraryStateSnapshot();
  const auto updateCallsBeforeShutdown = fixture.fakeMetadata->updateCalls();

  playerSubscription.unsubscribe();
  librarySubscription.unsubscribe();
  notificationSubscription.unsubscribe();
  // Subscription delivery callbacks are asynchronous. unsubscribe() waits for
  // already-started pre-shutdown deliveries, so the no-late-event baseline must
  // be captured after all subscriptions are fully removed.
  const auto playerDeliveriesBeforeLateEvents = playerSnapshots.count();
  const auto libraryDeliveriesBeforeLateEvents = librarySnapshots.count();
  const auto notificationDeliveriesBeforeLateEvents = notificationDeliveries;
  fixture.controller->shutdown();

  CHECK(fixture.fakeAudio->setEventSinkCalls() == 2U);
  CHECK(fixture.fakeScanner->setEventSinkCalls() == 2U);
  CHECK(fixture.fakeMetadata->commandUnregistrations() == 1U);
  CHECK_FALSE(fixture.fakeMetadata->hasCommandCallback());
  CHECK(fixture.fakeMetadata->stopCalls() == 1U);

  fixture.fakeMetadata->emitCommand(command(MediaControlCommandKind::Pause));
  fixture.fakeAudio->emit(audioTrackChangedEvent("late-audio", "music/late.flac", 99));
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("late", "music/late.flac")}, 99), 99));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->pauseCalls() == 0U);
  CHECK(fixture.fakeMetadata->updateCalls() == updateCallsBeforeShutdown);
  CHECK(playerSnapshots.count() == playerDeliveriesBeforeLateEvents);
  CHECK(librarySnapshots.count() == libraryDeliveriesBeforeLateEvents);
  CHECK(notificationDeliveries == notificationDeliveriesBeforeLateEvents);
  CHECK(fixture.controller->playerStateSnapshot().freshness.version == playerSnapshotBeforeShutdown.freshness.version);
  CHECK(fixture.controller->libraryStateSnapshot().version == librarySnapshotBeforeShutdown.version);
}

TEST_CASE("media controller facade clears constructor-installed sinks when destroyed before start") {
  auto fakeAudio = std::make_shared<control_test::FakeAudioPlaybackService>();
  auto fakeScanner = std::make_shared<control_test::FakeFileScannerService>();
  auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
  {
    auto controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                      .scanner = fakeScanner,
                                                                      .metadata = std::move(metadataService),
                                                                      .folderSortSettingsStore = {}},
                                         MediaControllerOptions{.runInlineForTests = true});
    CHECK(fakeAudio->setEventSinkCalls() == 1U);
    CHECK(fakeScanner->setEventSinkCalls() == 1U);
  }

  CHECK(fakeAudio->setEventSinkCalls() == 2U);
  CHECK(fakeScanner->setEventSinkCalls() == 2U);

  fakeAudio->emit(audioTrackChangedEvent("late-audio", "music/late.flac", 1));
  fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("late", "music/late.flac")}, 1), 1));
  CHECK(fakeAudio->emitEventCalls() == 1U);
  CHECK(fakeScanner->emitEventCalls() == 1U);
}

TEST_CASE("media controller facade contains subscriber exceptions and updates metadata") {
  ControllerFixture fixture{};
  std::atomic_size_t laterPlayerSnapshots{0};
  std::atomic_size_t laterLibrarySnapshots{0};
  std::atomic_size_t laterNotifications{0};
  auto throwingPlayerSubscription = fixture.controller->subscribePlayerState([](PlayerStateSnapshot) { throw std::runtime_error{"player subscriber"}; });
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot) { laterPlayerSnapshots.fetch_add(1U); });
  auto throwingLibrarySubscription = fixture.controller->subscribeLibraryState([](LibraryStateSnapshot) { throw std::runtime_error{"library subscriber"}; });
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](LibraryStateSnapshot) { laterLibrarySnapshots.fetch_add(1U); });
  auto throwingNotificationSubscription = fixture.controller->subscribeDomainNotifications([](ControlDomainNotification) { throw std::runtime_error{"notification subscriber"}; });
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification) { laterNotifications.fetch_add(1U); });
  fixture.controller->start();

  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(waitUntil([&] { return laterLibrarySnapshots.load() >= 2U; }));
  CHECK(waitUntil([&] { return laterPlayerSnapshots.load() >= 2U; }));
  CHECK(waitUntil([&] { return laterNotifications.load() >= 1U; }));
  CHECK(fixture.fakeMetadata->updateCalls() >= 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
  throwingPlayerSubscription.unsubscribe();
  playerSubscription.unsubscribe();
  throwingLibrarySubscription.unsubscribe();
  librarySubscription.unsubscribe();
  throwingNotificationSubscription.unsubscribe();
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller facade completes dispatch future when queued work throws") {
  ControllerFixture fixture{MediaControllerOptions{.runInlineForTests = false}};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac")}, 20), 1));
  for (auto attempts = 0; attempts < 100 && !fixture.controller->libraryStateSnapshot().libraryTree.has_value(); ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  fixture.fakeAudio->loadTrackThrows(std::runtime_error{"load failed"});

  auto commandResult = std::async(std::launch::async, [&] { return fixture.controller->submitCommand(command(MediaControlCommandKind::Play)); });

  REQUIRE(commandResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK_THROWS_WITH_AS(static_cast<void>(commandResult.get()), "load failed", std::runtime_error);
}

TEST_CASE("media controller facade slow snapshot subscribers do not starve control work") {
  ControllerFixture fixture{MediaControllerOptions{.runInlineForTests = false}};
  std::promise<void> subscriberEntered{};
  auto subscriberIsBlocked = subscriberEntered.get_future();
  std::promise<void> releaseSubscriber{};
  auto releaseSignal = releaseSubscriber.get_future().share();
  std::promise<void> subscriberExited{};
  auto subscriberIsReleased = subscriberExited.get_future();
  std::atomic_bool enteredOnce{false};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot) mutable {
    if (enteredOnce.exchange(true)) {
      return;
    }
    subscriberEntered.set_value();
    releaseSignal.wait();
    subscriberExited.set_value();
  });
  fixture.controller->start();

  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  REQUIRE(subscriberIsBlocked.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

  auto pauseResult = std::async(std::launch::async, [&] {
    return fixture.controller->submitCommand(command(MediaControlCommandKind::Pause));
  });

  REQUIRE(pauseResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(pauseResult.get().accepted);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);

  releaseSubscriber.set_value();
  REQUIRE(subscriberIsReleased.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  playerSubscription.unsubscribe();
}

TEST_CASE("media controller facade subscribers receive committed snapshots not raw sink payloads") {
  ControllerFixture fixture{};
  std::promise<PlayerStateSnapshot> committedPlayerSnapshot{};
  auto committedPlayer = committedPlayerSnapshot.get_future();
  std::atomic_bool playerSnapshotCaptured{false};
  std::promise<LibraryStateSnapshot> committedLibrarySnapshot{};
  auto committedLibrary = committedLibrarySnapshot.get_future();
  std::atomic_bool librarySnapshotCaptured{false};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot snapshot) {
    if (snapshot.currentTrack.has_value() && !playerSnapshotCaptured.exchange(true)) {
      committedPlayerSnapshot.set_value(std::move(snapshot));
    }
  });
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](LibraryStateSnapshot snapshot) {
    if (snapshot.scanStatus == LibraryScanStatus::Scanning && !librarySnapshotCaptured.exchange(true)) {
      committedLibrarySnapshot.set_value(std::move(snapshot));
    }
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scanStartedEvent(1));
  fixture.controller->drainForTests();

  fixture.fakeAudio->emit(audioTrackChangedEvent("sink-track", "music/sink.flac", 1));
  fixture.controller->drainForTests();

  REQUIRE(committedLibrary.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  const auto librarySnapshot = committedLibrary.get();
  CHECK(librarySnapshot.scanStatus == LibraryScanStatus::Scanning);
  CHECK_FALSE(librarySnapshot.libraryTree.has_value());
  REQUIRE(committedPlayer.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  const auto playerSnapshot = committedPlayer.get();
  REQUIRE(playerSnapshot.currentTrack.has_value());
  CHECK(playerSnapshot.currentTrack->trackId == "sink-track");
  CHECK(playerSnapshot.timeline.duration == std::chrono::milliseconds{3000});
  playerSubscription.unsubscribe();
  librarySubscription.unsubscribe();
}

TEST_CASE("media controller schedules an artwork intent and shows the thumbnail-first snapshot") {
  ArtworkControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(
      libraryTree({songWithThumbnail("a", "music/a.flac", "/thumbs/a.png")}, 21), 21));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

  REQUIRE(fixture.fakeArtwork->requestCount() == 1U);
  const auto request = fixture.fakeArtwork->lastRequest();
  CHECK(request.generation == 1U);
  CHECK(request.identity.trackId == "a");
  CHECK(request.identity.filePath == std::filesystem::path{"music/a.flac"});
  CHECK(request.artworkSourcePath == std::filesystem::path{"music/a.flac"});
  CHECK(request.fallbackThumbnailPath == std::filesystem::path{"/thumbs/a.png"});

  const auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/a.png"});
  CHECK(snapshot.artwork->thumbnailPath == std::filesystem::path{"/thumbs/a.png"});
}

TEST_CASE("media controller applies a resolved full path and retains the thumbnail fallback") {
  ArtworkControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(
      libraryTree({songWithThumbnail("a", "music/a.flac", "/thumbs/a.png")}, 21), 21));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

  const auto before = fixture.controller->playerStateSnapshot();
  REQUIRE(before.artwork.has_value());
  CHECK(before.artwork->localPath == std::filesystem::path{"/thumbs/a.png"});

  fixture.fakeArtwork->complete(artworkResult(1U, track("a", "music/a.flac"), ArtworkResolveOutcomeKind::FullPath, "/covers/full-a.png"));
  fixture.controller->drainForTests();

  const auto after = fixture.controller->playerStateSnapshot();
  REQUIRE(after.artwork.has_value());
  CHECK(after.artwork->localPath == std::filesystem::path{"/covers/full-a.png"});
  CHECK(after.artwork->thumbnailPath == std::filesystem::path{"/thumbs/a.png"});
  CHECK(after.freshness.version > before.freshness.version);
}

TEST_CASE("media controller keeps the thumbnail and player state on no-art cover error and resolver failure") {
  ArtworkControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(
      libraryTree({songWithThumbnail("a", "music/a.flac", "/thumbs/a.png")}, 21), 21));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

  const auto identity = track("a", "music/a.flac");
  const auto before = fixture.controller->playerStateSnapshot();

  const std::vector<ArtworkResolveResultView> results = {
      artworkResult(1U, identity, ArtworkResolveOutcomeKind::NoArt),
      artworkResult(1U, identity, ArtworkResolveOutcomeKind::CoverError),
      artworkResult(1U, identity, ArtworkResolveOutcomeKind::ResolverFailure),
  };
  for (const auto& result : results) {
    fixture.fakeArtwork->complete(result);
    fixture.controller->drainForTests();
    const auto snapshot = fixture.controller->playerStateSnapshot();
    REQUIRE(snapshot.artwork.has_value());
    CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/a.png"});
    CHECK(snapshot.artwork->thumbnailPath == std::filesystem::path{"/thumbs/a.png"});
    CHECK(snapshot.freshness.version == before.freshness.version);
  }
}

TEST_CASE("media controller drops stale and identity-mismatched artwork resolutions") {
  ArtworkControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(
      libraryTree({songWithThumbnail("a", "music/a.flac", "/thumbs/a.png"),
                   songWithThumbnail("b", "music/b.flac", "/thumbs/b.png")},
                  21),
      21));
  fixture.controller->drainForTests();

  auto selectCommand = command(MediaControlCommandKind::SelectTrack);
  selectCommand.track = track("a", "music/a.flac");
  CHECK(fixture.controller->submitCommand(selectCommand).accepted);
  selectCommand.track = track("b", "music/b.flac");
  CHECK(fixture.controller->submitCommand(selectCommand).accepted);

  REQUIRE(fixture.fakeArtwork->requestCount() == 2U);
  CHECK(fixture.fakeArtwork->lastRequest().generation == 2U);
  const auto afterSelectB = fixture.controller->playerStateSnapshot();
  REQUIRE(afterSelectB.artwork.has_value());
  CHECK(afterSelectB.artwork->localPath == std::filesystem::path{"/thumbs/b.png"});

  // Old generation for A: dropped entirely.
  fixture.fakeArtwork->complete(artworkResult(1U, track("a", "music/a.flac"), ArtworkResolveOutcomeKind::FullPath, "/covers/full-a.png"));
  fixture.controller->drainForTests();
  auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/b.png"});
  CHECK(snapshot.freshness.version == afterSelectB.freshness.version);

  // Current generation but wrong logical identity: dropped.
  fixture.fakeArtwork->complete(artworkResult(2U, track("a", "music/a.flac"), ArtworkResolveOutcomeKind::FullPath, "/covers/full-a.png"));
  fixture.controller->drainForTests();
  snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/b.png"});
  CHECK(snapshot.freshness.version == afterSelectB.freshness.version);

  // Matching generation and identity: applied.
  fixture.fakeArtwork->complete(artworkResult(2U, track("b", "music/b.flac"), ArtworkResolveOutcomeKind::FullPath, "/covers/full-b.png"));
  fixture.controller->drainForTests();
  snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/covers/full-b.png"});
  CHECK(snapshot.artwork->thumbnailPath == std::filesystem::path{"/thumbs/b.png"});
}

TEST_CASE("media controller preserves logical CUE identity and artwork across track changed") {
  ArtworkControllerFixture fixture{};
  fixture.controller->start();
  const auto cue = cueSong("cue-01", "music/cue/disc.cue", "music/cue/disc.flac",
                           std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000}, "/thumbs/cue-01.png");
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({cue}, 21), 21));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

  // The artwork intent resolves from the referenced audio file while keeping
  // the logical .cue identity.
  REQUIRE(fixture.fakeArtwork->requestCount() == 1U);
  const auto request = fixture.fakeArtwork->lastRequest();
  CHECK(request.identity.trackId == "cue-01");
  CHECK(request.identity.filePath == std::filesystem::path{"music/cue/disc.cue"});
  CHECK(request.artworkSourcePath == std::filesystem::path{"music/cue/disc.flac"});
  CHECK(request.fallbackThumbnailPath == std::filesystem::path{"/thumbs/cue-01.png"});

  auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.currentTrack.has_value());
  CHECK(snapshot.currentTrack->filePath == std::filesystem::path{"music/cue/disc.cue"});
  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->album == "Cue Album");

  // The audio backend confirms the segment request (audio source + offset/duration).
  fixture.fakeAudio->emit(cueTrackChangedEvent("cue-01", "music/cue/disc.flac",
                                               std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000}, 25));
  fixture.controller->drainForTests();

  snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.currentTrack.has_value());
  CHECK(snapshot.currentTrack->trackId == "cue-01");
  CHECK(snapshot.currentTrack->filePath == std::filesystem::path{"music/cue/disc.cue"});
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/cue-01.png"});
  CHECK(snapshot.artwork->thumbnailPath == std::filesystem::path{"/thumbs/cue-01.png"});
  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->album == "Cue Album");
  CHECK(snapshot.timeline.position == std::chrono::milliseconds{1000});
}

TEST_CASE("media controller keeps logical CUE identity and artwork after a cache-hit re-scan") {
  ArtworkControllerFixture fixture{};
  fixture.controller->start();
  const auto cue = cueSong("cue-01", "music/cue/disc.cue", "music/cue/disc.flac",
                           std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000}, "/thumbs/cue-01.png");
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({cue}, 21), 21));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

  // Cache hit: the same tree is published again; reconcile keeps the current track.
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({cue}, 22), 22));
  fixture.controller->drainForTests();

  fixture.fakeAudio->emit(cueTrackChangedEvent("cue-01", "music/cue/disc.flac",
                                               std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000}, 25));
  fixture.controller->drainForTests();

  const auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.currentTrack.has_value());
  CHECK(snapshot.currentTrack->filePath == std::filesystem::path{"music/cue/disc.cue"});
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/cue-01.png"});
  CHECK(snapshot.artwork->thumbnailPath == std::filesystem::path{"/thumbs/cue-01.png"});
  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->album == "Cue Album");
}
