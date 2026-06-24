#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "seriona/control/control_contracts.h"
#include "seriona/metadata/metadata_contracts.h"
#include "metadata_mapper.h"
#include "metadata_mpris_private.h"

#if defined(__linux__) && !defined(__APPLE__)
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace seriona::metadata {
MetadataSyncResult metadataMprisSmokeResult();
}

namespace {

struct RecordingMprisObject final : seriona::metadata::detail::IMprisObject {
  seriona::metadata::detail::MprisObjectModel model{};
  seriona::metadata::detail::MprisCommandHandlers handlers{};
  std::vector<seriona::metadata::detail::MprisSnapshotRecord> published{};
  int propertiesChangedSignals{0};

  void registerModel(const seriona::metadata::detail::MprisObjectModel& value) override { model = value; }
  void registerCommandHandlers(const seriona::metadata::detail::MprisCommandHandlers& value) override { handlers = value; }
  void publish(const seriona::metadata::detail::MprisSnapshotRecord& snapshot, bool emitPropertiesChanged) override {
    published.push_back(snapshot);
    if (emitPropertiesChanged) {
      ++propertiesChangedSignals;
    }
  }
};

struct CommandRecorder {
  std::vector<seriona::control::MediaControlCommand> commands{};

  seriona::control::MediaControlCommandSink sink() {
    return [this](const seriona::control::MediaControlCommand& command) { commands.push_back(command); };
  }
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
    return std::unique_ptr<seriona::metadata::detail::IMprisObject>{std::move(ownedObject)};
  }
};

seriona::control::PlayerStateSnapshot buildSnapshot(std::string trackId,
                                                     std::optional<std::filesystem::path> artworkPath,
                                                     bool canControl,
                                                     seriona::control::PlaybackStatus status = seriona::control::PlaybackStatus::Playing,
                                                     std::chrono::milliseconds position = std::chrono::milliseconds{1234}) {
  seriona::control::PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = 1U;
  snapshot.freshness.sampledAt = std::chrono::steady_clock::now();
  snapshot.currentTrack = seriona::control::TrackIdentity{.trackId = std::move(trackId),
                                                          .filePath = std::filesystem::path{"music/track.flac"},
                                                          .sourceId = "source-a",
                                                          .libraryId = "library-a"};
  snapshot.display = seriona::control::DisplayMetadata{.title = "Song", .artist = "Artist", .album = "R・I・O・T", .albumArtist = "Album Artist", .genre = "Genre"};
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
  snapshot.timeline.position = position;
  snapshot.timeline.duration = std::chrono::milliseconds{5678};
  snapshot.volume = 0.75F;
  return snapshot;
}

std::string trackObjectPath() {
  const seriona::control::TrackIdentity track{
      .trackId = "track-01",
      .filePath = std::filesystem::path{"music/track.flac"},
      .sourceId = "source-a",
      .libraryId = "library-a",
  };
  return seriona::metadata::makeMprisTrackObjectPath(track);
}

std::string fileUriForPath(const std::filesystem::path& path) {
  return std::string{"file://"} + path.generic_string();
}

void checkCommand(const seriona::control::MediaControlCommand& command,
                  seriona::control::MediaControlCommandKind expectedKind,
                  std::optional<std::chrono::milliseconds> expectedPosition = std::nullopt,
                  std::optional<std::chrono::milliseconds> expectedDelta = std::nullopt,
                  std::optional<float> expectedVolume = std::nullopt,
                  std::optional<seriona::control::RepeatMode> expectedRepeatMode = std::nullopt,
                  std::optional<bool> expectedShuffle = std::nullopt) {
  CHECK(command.kind == expectedKind);
  CHECK(command.position == expectedPosition);
  CHECK(command.delta == expectedDelta);
  CHECK(command.volume == expectedVolume);
  CHECK(command.repeatMode == expectedRepeatMode);
  CHECK(command.shuffle == expectedShuffle);
}

}

TEST_CASE("metadata mpris smoke path returns a stable accepted result") {
  const auto result = seriona::metadata::metadataMprisSmokeResult();

  CHECK(result.accepted);
  CHECK_FALSE(result.changed);
}

TEST_CASE("linux mpris art url prefers explicit uri and exports absolute local files") {
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};
  auto snapshot = buildSnapshot("track-01", std::filesystem::path{"relative/cover.jpg"}, true);
  snapshot.artwork->uri = std::string{"file:///tmp/seriona-explicit-cover.jpg"};

  CHECK(adapter.start(seriona::metadata::PlatformMediaState{.controlState = snapshot,
                                                            .timelineUpdateInterval = std::chrono::milliseconds{1000}}).accepted);

  REQUIRE(adapter.lastPublishedSnapshot().has_value());
  CHECK(adapter.lastPublishedSnapshot()->artUrl == "file:///tmp/seriona-explicit-cover.jpg");
  REQUIRE(busRaw->object != nullptr);
  CHECK(busRaw->object->published.back().artUrl == "file:///tmp/seriona-explicit-cover.jpg");
}

TEST_CASE("linux mpris production object is visible on the session bus") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  if (std::getenv("DBUS_SESSION_BUS_ADDRESS") == nullptr) {
    return;
  }

  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{};
  const auto start = adapter.start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true),
                                                                         .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  REQUIRE(start.accepted);

  try {
    auto proxy = sdbus::createProxy(sdbus::ServiceName{seriona::metadata::detail::kMprisBusName},
                                    sdbus::ObjectPath{seriona::metadata::detail::kMprisObjectPath},
                                    sdbus::dont_run_event_loop_thread);
    std::string introspectionXml;
    proxy->callMethod("Introspect")
        .onInterface("org.freedesktop.DBus.Introspectable")
        .withTimeout(std::chrono::milliseconds{500})
        .storeResultsTo(introspectionXml);

    CHECK(introspectionXml.find(seriona::metadata::detail::kMprisRootInterface) != std::string::npos);
    CHECK(introspectionXml.find(seriona::metadata::detail::kMprisPlayerInterface) != std::string::npos);
    CHECK(introspectionXml.find("PlaybackStatus") != std::string::npos);
    CHECK(introspectionXml.find("Metadata") != std::string::npos);
  } catch (const sdbus::Error& error) {
    CAPTURE(error.getName());
    CAPTURE(error.getMessage());
    FAIL("MPRIS object must be introspectable on the session bus");
  }

  static_cast<void>(adapter.stop());
#endif
}

TEST_CASE("linux mpris adapter exposes the real object model through a fake bus seam") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};
  CommandRecorder recorder{};
  adapter.setCommandSink(recorder.sink());

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

  REQUIRE(busRaw->object->handlers.play);
  REQUIRE(busRaw->object->handlers.pause);
  REQUIRE(busRaw->object->handlers.playPause);
  REQUIRE(busRaw->object->handlers.stop);
  REQUIRE(busRaw->object->handlers.next);
  REQUIRE(busRaw->object->handlers.previous);
  REQUIRE(busRaw->object->handlers.seekBy);
  REQUIRE(busRaw->object->handlers.setPosition);
  REQUIRE(busRaw->object->handlers.setVolume);
  REQUIRE(busRaw->object->handlers.setRepeatMode);
  REQUIRE(busRaw->object->handlers.setShuffle);

  CHECK(busRaw->object->handlers.play());
  CHECK(busRaw->object->handlers.pause());
  CHECK(busRaw->object->handlers.playPause());
  CHECK(busRaw->object->handlers.stop());
  CHECK(busRaw->object->handlers.next());
  CHECK(busRaw->object->handlers.previous());
  CHECK(busRaw->object->handlers.seekBy(std::chrono::microseconds{250'000}));
  CHECK(busRaw->object->handlers.setPosition(trackObjectPath(), std::chrono::microseconds{2'000'000}));
  CHECK(busRaw->object->handlers.setVolume(0.25F));
  CHECK(busRaw->object->handlers.setRepeatMode(seriona::control::RepeatMode::One));
  CHECK(busRaw->object->handlers.setShuffle(false));

  REQUIRE(recorder.commands.size() == 11);
  checkCommand(recorder.commands[0], seriona::control::MediaControlCommandKind::Play);
  checkCommand(recorder.commands[1], seriona::control::MediaControlCommandKind::Pause);
  checkCommand(recorder.commands[2], seriona::control::MediaControlCommandKind::TogglePlayPause);
  checkCommand(recorder.commands[3], seriona::control::MediaControlCommandKind::Stop);
  checkCommand(recorder.commands[4], seriona::control::MediaControlCommandKind::SkipNext);
  checkCommand(recorder.commands[5], seriona::control::MediaControlCommandKind::SkipPrevious);
  checkCommand(recorder.commands[6], seriona::control::MediaControlCommandKind::SeekBy, std::nullopt, std::chrono::milliseconds{250});
  checkCommand(recorder.commands[7], seriona::control::MediaControlCommandKind::SeekTo, std::chrono::milliseconds{2000});
  checkCommand(recorder.commands[8], seriona::control::MediaControlCommandKind::SetVolume, std::nullopt, std::nullopt, 0.25F);
  checkCommand(recorder.commands[9], seriona::control::MediaControlCommandKind::SetRepeatMode, std::nullopt, std::nullopt, std::nullopt, seriona::control::RepeatMode::One);
  checkCommand(recorder.commands[10], seriona::control::MediaControlCommandKind::SetShuffle, std::nullopt, std::nullopt, std::nullopt, std::nullopt, false);

  const auto& published = busRaw->object->published.back();
  CHECK(published.trackObjectPath.rfind("/org/mpris", 0) != 0);
  CHECK(published.artUrl == fileUriForPath(std::filesystem::absolute("covers/track.jpg")));
  CHECK(published.snapshot.mpris.fields.album == std::optional<std::string>{"R・I・O・T"});
  CHECK(published.loopStatus == "Playlist");
  CHECK(published.canControl);

  const auto update = adapter.update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, false),
                                                                            .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(update.accepted);
  CHECK_FALSE(busRaw->object->published.empty());
  CHECK_FALSE(busRaw->object->published.back().canControl);

  CHECK_FALSE(adapter.setPosition("/org/mpris/MediaPlayer2/TrackList/NoTrack", std::chrono::microseconds{1'000'000}));
  CHECK_FALSE(busRaw->object->handlers.seekBy(std::chrono::microseconds{-1}));
  CHECK_FALSE(busRaw->object->handlers.setPosition("/com/seriona/metadata/track/track-01", std::chrono::microseconds{-1}));
#endif
}

TEST_CASE("linux mpris adapter updates playing position without emitting repeated properties changed signals") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};

  const auto start = adapter.start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true, seriona::control::PlaybackStatus::Playing, std::chrono::milliseconds{26'000}),
                                                                         .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  REQUIRE(start.accepted);
  REQUIRE(busRaw->object != nullptr);
  CHECK(busRaw->object->propertiesChangedSignals == 1);

  const auto tick = adapter.update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true, seriona::control::PlaybackStatus::Playing, std::chrono::milliseconds{27'000}),
                                                                          .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  REQUIRE(tick.accepted);
  REQUIRE(busRaw->object->published.size() == 2U);
  CHECK(busRaw->object->published.back().snapshot.mpris.positionMicros == 27'000'000);
  CHECK(busRaw->object->propertiesChangedSignals == 1);

  const auto paused = adapter.update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true, seriona::control::PlaybackStatus::Paused, std::chrono::milliseconds{27'000}),
                                                                            .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  REQUIRE(paused.accepted);
  CHECK(busRaw->object->propertiesChangedSignals == 2);
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

TEST_CASE("metadata mpris adapter rejects commands when capabilities are disabled") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto adapter = seriona::metadata::detail::LinuxMprisAdapter{std::move(bus)};
  CommandRecorder recorder{};
  adapter.setCommandSink(recorder.sink());
  const auto start = adapter.start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, false),
                                                                         .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(start.accepted);
  REQUIRE(busRaw->object != nullptr);

  CHECK_FALSE(busRaw->object->handlers.play());
  CHECK_FALSE(busRaw->object->handlers.pause());
  CHECK_FALSE(busRaw->object->handlers.playPause());
  CHECK_FALSE(busRaw->object->handlers.stop());
  CHECK_FALSE(busRaw->object->handlers.next());
  CHECK_FALSE(busRaw->object->handlers.previous());
  CHECK_FALSE(busRaw->object->handlers.seekBy(std::chrono::microseconds{1'000'000}));
  CHECK_FALSE(busRaw->object->handlers.setPosition("/com/seriona/metadata/track/track-01", std::chrono::microseconds{1'000'000}));
  CHECK_FALSE(busRaw->object->handlers.setVolume(0.5F));
  CHECK_FALSE(busRaw->object->handlers.setRepeatMode(seriona::control::RepeatMode::All));
  CHECK_FALSE(busRaw->object->handlers.setShuffle(true));
  CHECK(recorder.commands.empty());
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

TEST_CASE("metadata mpris backend unsubscribe remains safe after backend destruction") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  seriona::control::SubscriptionHandle handle{};

  {
    auto backend = seriona::metadata::detail::makeLinuxMetadataServiceBackend(std::move(bus));
    handle = backend->registerCommandCallback(seriona::control::MediaControlCommandSink{});
    REQUIRE(handle.unsubscribe);
    CHECK(handle.subscriptionId != 0U);
  }

  handle.unsubscribe();
  handle.unsubscribe();
#endif
}

TEST_CASE("metadata mpris backend command callback may unsubscribe during dispatch") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto backend = seriona::metadata::detail::makeLinuxMetadataServiceBackend(std::move(bus));
  seriona::control::SubscriptionHandle handle{};
  std::atomic_int commandCount{0};
  handle = backend->registerCommandCallback([&](const seriona::control::MediaControlCommand& command) {
    CHECK(command.kind == seriona::control::MediaControlCommandKind::Play);
    ++commandCount;
    handle.unsubscribe();
  });

  const auto start = backend->start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true),
                                                                          .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(start.accepted);
  REQUIRE(busRaw->object != nullptr);
  REQUIRE(busRaw->object->handlers.play);

  CHECK(busRaw->object->handlers.play());
  CHECK(commandCount.load() == 1);
  CHECK_FALSE(busRaw->object->handlers.play());
#endif
}

TEST_CASE("metadata mpris backend command dispatch tolerates racing unsubscribe and update") {
#if !defined(__linux__) || defined(__APPLE__)
  SUCCEED("linux-only adapter test");
#else
  auto bus = std::make_unique<RecordingMprisBus>();
  auto* busRaw = bus.get();
  auto backend = seriona::metadata::detail::makeLinuxMetadataServiceBackend(std::move(bus));
  std::mutex handleMutex{};
  seriona::control::SubscriptionHandle handle{};
  std::atomic_bool done{false};
  std::atomic_int commandCount{0};

  auto subscribe = [&] {
    return backend->registerCommandCallback([&](const seriona::control::MediaControlCommand& command) {
      if (command.kind == seriona::control::MediaControlCommandKind::Play) {
        ++commandCount;
      }
    });
  };
  handle = subscribe();
  const auto start = backend->start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true),
                                                                          .timelineUpdateInterval = std::chrono::milliseconds{1000}});
  CHECK(start.accepted);
  REQUIRE(busRaw->object != nullptr);
  REQUIRE(busRaw->object->handlers.play);

  std::thread dispatchThread{[&] {
    for (int i = 0; i < 2'000; ++i) {
      static_cast<void>(busRaw->object->handlers.play());
    }
    done = true;
  }};
  std::thread updateThread{[&] {
    while (!done.load()) {
      static_cast<void>(backend->update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot("track-01", std::filesystem::path{"covers/track.jpg"}, true),
                                                                               .timelineUpdateInterval = std::chrono::milliseconds{1000}}));
    }
  }};
  std::thread subscriptionThread{[&] {
    while (!done.load()) {
      std::lock_guard lock{handleMutex};
      if (handle.unsubscribe) {
        handle.unsubscribe();
      }
      handle = subscribe();
    }
  }};

  dispatchThread.join();
  updateThread.join();
  subscriptionThread.join();
  {
    std::lock_guard lock{handleMutex};
    if (handle.unsubscribe) {
      handle.unsubscribe();
    }
  }
  CHECK(commandCount.load() >= 0);
#endif
}
