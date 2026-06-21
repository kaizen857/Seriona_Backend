#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seriona/control/control_contracts.h"
#include "seriona/metadata/metadata_contracts.h"
#include "metadata_mapper.h"
#include "metadata_mpris_private.h"

namespace seriona::metadata {
MetadataSyncResult metadataMprisSmokeResult();
}

namespace {

struct RecordingMprisObject final : seriona::metadata::detail::IMprisObject {
  seriona::metadata::detail::MprisObjectModel model{};
  std::vector<seriona::metadata::detail::MprisSnapshotRecord> published{};

  void registerModel(const seriona::metadata::detail::MprisObjectModel& value) override { model = value; }
  void publish(const seriona::metadata::detail::MprisSnapshotRecord& snapshot) override { published.push_back(snapshot); }
};

struct RecordingMprisBus final : seriona::metadata::detail::IMprisBus {
  std::string requestedName{};
  std::string objectManagerPath{};
  RecordingMprisObject* object{nullptr};
  std::unique_ptr<RecordingMprisObject> ownedObject{std::make_unique<RecordingMprisObject>()};

  void requestName(std::string_view name) override { requestedName = std::string{name}; }
  void addObjectManager(std::string_view objectPath) override { objectManagerPath = std::string{objectPath}; }
  [[nodiscard]] std::unique_ptr<seriona::metadata::detail::IMprisObject> createObject(std::string_view) override {
    object = ownedObject.get();
    return std::unique_ptr<seriona::metadata::detail::IMprisObject>{ownedObject.release()};
  }
};

seriona::control::PlayerStateSnapshot buildSnapshot(std::string trackId,
                                                    std::optional<std::filesystem::path> artworkPath,
                                                    bool canControl,
                                                    seriona::control::PlaybackStatus status = seriona::control::PlaybackStatus::Playing) {
  seriona::control::PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = 1U;
  snapshot.freshness.sampledAt = std::chrono::steady_clock::now();
  snapshot.currentTrack = seriona::control::TrackIdentity{.trackId = std::move(trackId),
                                                          .filePath = std::filesystem::path{"music/track.flac"},
                                                          .sourceId = "source-a",
                                                          .libraryId = "library-a"};
  snapshot.display = seriona::control::DisplayMetadata{.title = "Song", .artist = "Artist", .album = "Album", .albumArtist = "Album Artist", .genre = "Genre"};
  snapshot.artwork = seriona::control::ArtworkRef{.localPath = std::move(artworkPath), .uri = std::nullopt, .contentHash = std::string{"hash-01"}};
  snapshot.playback.state = status;
  snapshot.repeatMode = seriona::control::RepeatMode::All;
  snapshot.capabilities = seriona::control::PlaybackCapabilities{.canPlay = canControl,
                                                                 .canPause = canControl,
                                                                 .canStop = canControl,
                                                                 .canSeek = canControl,
                                                                 .canSkipNext = canControl,
                                                                 .canSkipPrevious = canControl,
                                                                 .canSetRepeat = canControl,
                                                                 .canSetShuffle = canControl,
                                                                 .canSetVolume = canControl,
                                                                 .canSelectTrack = canControl};
  snapshot.timeline.position = std::chrono::milliseconds{1234};
  snapshot.timeline.duration = std::chrono::milliseconds{5678};
  snapshot.volume = 0.75F;
  return snapshot;
}

}

TEST_CASE("metadata mpris smoke path returns a stable accepted result") {
  const auto result = seriona::metadata::metadataMprisSmokeResult();

  CHECK(result.accepted);
  CHECK_FALSE(result.changed);
}

TEST_CASE("linux mpris adapter exposes the real object model through a fake bus seam") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};
  adapter.setCommandSink([](const seriona::control::MediaControlCommand&) {});

  const auto start = adapter.start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true),
                                                                         .timelineUpdateInterval = std::chrono::milliseconds{1000}});

  CHECK(start.accepted);
  CHECK(busRaw->requestedName == "org.mpris.MediaPlayer2.seriona");
  CHECK(busRaw->objectManagerPath == "/org/mpris/MediaPlayer2");
  REQUIRE(busRaw->object != nullptr);
  CHECK(busRaw->object->model.objectPath == "/org/mpris/MediaPlayer2");
  CHECK(busRaw->object->model.rootInterface == "org.mpris.MediaPlayer2");
  CHECK(busRaw->object->model.playerInterface == "org.mpris.MediaPlayer2.Player");
  CHECK(busRaw->object->model.playerMethods == std::vector<std::string>{"Next", "Previous", "Pause", "PlayPause", "Stop", "Play", "Seek", "SetPosition", "OpenUri"});
  CHECK(busRaw->object->model.playerProperties == std::vector<std::string>{"PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume", "Position", "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl"});
  REQUIRE_FALSE(busRaw->object->published.empty());

  const auto& published = busRaw->object->published.back();
  CHECK(published.trackObjectPath.rfind("/org/mpris", 0) != 0);
  CHECK(published.artUrl == "file://covers/track.jpg");
  CHECK(published.loopStatus == "Playlist");
  CHECK(published.canControl);

  const auto update = adapter.update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, false),
                                                                            .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(update.accepted);
  CHECK_FALSE(busRaw->object->published.empty());
  CHECK_FALSE(busRaw->object->published.back().canControl);

  CHECK_FALSE(adapter.setPosition("/org/mpris/MediaPlayer2/TrackList/NoTrack", std::chrono::microseconds{1'000'000}));
#endif
}

TEST_CASE("metadata mpris adapter rejects stale track ids for SetPosition") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};
  const auto start = adapter.start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true),
                                                                         .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(start.accepted);

  CHECK_FALSE(adapter.setPosition("/com/seriona/metadata/track/track-02", std::chrono::microseconds{2'000'000}));
#endif
}

TEST_CASE("metadata mpris adapter gates CanControl off when command support is absent") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};
  const auto start = adapter.start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, false),
                                                                         .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(start.accepted);

  REQUIRE_FALSE(busRaw->object->published.empty());
  CHECK_FALSE(busRaw->object->published.back().canControl);
#endif
}
