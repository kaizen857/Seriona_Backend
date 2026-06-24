#pragma once

#include "metadata_mapper.h"
#include "metadata_service_backend.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::metadata::detail {

constexpr const char* kMprisBusName = "org.mpris.MediaPlayer2.seriona";
constexpr const char* kMprisObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kMprisRootInterface = "org.mpris.MediaPlayer2";
constexpr const char* kMprisPlayerInterface = "org.mpris.MediaPlayer2.Player";

struct MprisObjectModel {
  std::string objectPath{kMprisObjectPath};
  std::string rootInterface{kMprisRootInterface};
  std::string playerInterface{kMprisPlayerInterface};
  std::vector<std::string> rootProperties{"CanQuit", "CanRaise", "HasTrackList", "Identity", "DesktopEntry", "SupportedUriSchemes", "SupportedMimeTypes"};
  std::vector<std::string> playerMethods{"Next", "Previous", "Pause", "PlayPause", "Stop", "Play", "Seek", "SetPosition", "OpenUri"};
  std::vector<std::string> playerProperties{"PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume", "Position", "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl"};
};

struct MprisCommandHandlers {
  std::function<bool()> play;
  std::function<bool()> pause;
  std::function<bool()> playPause;
  std::function<bool()> stop;
  std::function<bool()> next;
  std::function<bool()> previous;
  std::function<bool(std::chrono::microseconds)> seekBy;
  std::function<bool(const std::string&, std::chrono::microseconds)> setPosition;
  std::function<bool(float)> setVolume;
  std::function<bool(control::RepeatMode)> setRepeatMode;
  std::function<bool(bool)> setShuffle;
};

struct MprisSnapshotRecord {
  MetadataPlatformSnapshotDto snapshot{};
  std::string trackObjectPath{kMprisNoTrackObjectPath};
  std::string artUrl;
  std::string loopStatus{"None"};
  bool canControl{false};
};

struct CommandSinkState {
  void set(control::MediaControlCommandSink sink);
  void clear();
  [[nodiscard]] std::optional<control::MediaControlCommandSink> snapshot() const;

private:
  mutable std::mutex mutex_{};
  std::optional<control::MediaControlCommandSink> sink_{};
};

class IMprisObject {
public:
  virtual ~IMprisObject() = default;
  virtual void registerModel(const MprisObjectModel& model) = 0;
  virtual void registerCommandHandlers(const MprisCommandHandlers& handlers) = 0;
  virtual void publish(const MprisSnapshotRecord& snapshot, bool emitPropertiesChanged) = 0;
};

class IMprisBus {
public:
  virtual ~IMprisBus() = default;
  virtual void requestName(std::string_view name) = 0;
  virtual void addObjectManager(std::string_view objectPath) = 0;
  [[nodiscard]] virtual std::unique_ptr<IMprisObject> createObject(std::string_view objectPath) = 0;
};

class LinuxMprisAdapter {
public:
  explicit LinuxMprisAdapter(std::unique_ptr<IMprisBus> bus = {});

  void setCommandSink(control::MediaControlCommandSink sink);
  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state);
  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state);
  [[nodiscard]] MetadataSyncResult stop();
  [[nodiscard]] bool setPosition(const std::string& trackObjectPath, std::chrono::microseconds position);
  [[nodiscard]] std::shared_ptr<CommandSinkState> commandSinkState() const noexcept { return commandSinkState_; }

  [[nodiscard]] const MprisObjectModel& model() const noexcept { return model_; }
  [[nodiscard]] const std::optional<MprisSnapshotRecord>& lastPublishedSnapshot() const noexcept { return lastPublishedSnapshot_; }

private:
  [[nodiscard]] static MetadataSyncResult makeFailureResult(std::string code, std::string message);
  [[nodiscard]] static MetadataSyncResult makeAcceptedResult(const PlatformMediaState& state, bool changed);
  [[nodiscard]] static std::string loopStatusFromRepeatMode(control::RepeatMode repeatMode);
  [[nodiscard]] static std::string artUrlFromArtwork(const std::optional<control::ArtworkRef>& artwork);
  [[nodiscard]] static bool canControlFromCapabilities(const control::PlaybackCapabilities& capabilities);
  [[nodiscard]] static MprisSnapshotRecord toSnapshotRecord(const MetadataPlatformSnapshotDto& snapshot);

  void ensureStarted();
  void configureCommandModel();
  void publishCurrentSnapshot(const PlatformMediaState& state);
  [[nodiscard]] std::optional<PlatformMediaState> currentStateSnapshot() const;
  [[nodiscard]] bool dispatchCommand(control::MediaControlCommandKind kind, std::optional<std::chrono::milliseconds> position = std::nullopt);
  [[nodiscard]] bool dispatchSeekBy(std::chrono::microseconds delta);
  [[nodiscard]] bool dispatchSetVolume(float volume);
  [[nodiscard]] bool dispatchSetRepeat(control::RepeatMode repeatMode);
  [[nodiscard]] bool dispatchSetShuffle(bool shuffle);

  std::unique_ptr<IMprisBus> bus_{};
  std::unique_ptr<IMprisObject> object_{};
  std::shared_ptr<CommandSinkState> commandSinkState_{std::make_shared<CommandSinkState>()};
  mutable std::mutex stateMutex_{};
  std::optional<PlatformMediaState> currentState_{};
  std::optional<MprisSnapshotRecord> lastPublishedSnapshot_{};
  bool started_{false};
  MprisObjectModel model_{};
  MprisCommandHandlers commandHandlers_{};
};

[[nodiscard]] std::unique_ptr<MetadataServiceBackend> makeLinuxMetadataServiceBackend(std::unique_ptr<IMprisBus> bus = {});

}
