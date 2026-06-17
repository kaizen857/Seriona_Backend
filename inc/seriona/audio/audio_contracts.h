#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace seriona::audio {

enum class AudioOutputMode {
  Direct,
  Mixed,
};

enum class AudioSampleFormat {
  Unknown,
  Int16,
  Int24,
  Int32,
  Float32,
};

enum class PlaybackState {
  Idle,
  Loading,
  Ready,
  Playing,
  Paused,
  Draining,
  Stopped,
  Error,
};

enum class PlaybackErrorCode {
  OpenFailed,
  UnsupportedFormat,
  DeviceUnavailable,
  FormatNegotiationFailed,
  DecodeFailed,
  BufferUnderrun,
  SeekFailed,
};

enum class BackendSourceModule {
  AudioPlayer,
  AudioPlaybackService,
};

enum class BackendEventType {
  PlaybackStateChanged,
  TrackChanged,
  PlaybackPositionUpdated,
  PositionDiscontinuity,
  PlaybackEnded,
  OutputFormatChanged,
  OutputModeFallback,
  PlaybackError,
};

struct TrackPlaybackRequest {
  std::string trackId;
  std::filesystem::path filePath;
  std::string title;
  std::string artist;
  std::optional<std::chrono::milliseconds> offset;
  std::optional<std::chrono::milliseconds> duration;
  std::optional<std::uint32_t> sampleRate;
  std::optional<std::uint16_t> bitDepth;
  std::optional<std::uint16_t> channels;
  std::optional<std::string> format;
};

struct AudioOutputConfig {
  AudioOutputMode outputMode{AudioOutputMode::Mixed};
  std::optional<std::uint32_t> targetSampleRate;
  std::optional<AudioSampleFormat> targetSampleFormat;
  std::optional<std::uint16_t> targetChannelCount;
  std::chrono::milliseconds bufferDuration{100};
  bool keepDeviceOpen{false};
  bool allowFallback{true};
  std::string preferredDeviceId;
};

struct AudioDeviceFormat {
  std::string deviceId;
  std::string deviceName;
  std::string backendName;
  std::uint32_t sampleRate{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::uint16_t channelCount{0};
  std::uint32_t bufferFrames{0};
  AudioOutputMode actualMode{AudioOutputMode::Mixed};
  bool fallbackApplied{false};
};

struct PlaybackClockSnapshot {
  std::string trackId;
  std::chrono::milliseconds position{0};
  std::chrono::steady_clock::time_point sampledAt{};
  std::uint64_t version{0};
  bool continuous{false};
};

struct PlaybackStateChanged {
  PlaybackState state{PlaybackState::Idle};
};

struct TrackChanged {
  TrackPlaybackRequest request{};
};

struct PlaybackPositionUpdated {
  PlaybackClockSnapshot clock{};
};

struct PositionDiscontinuity {
  PlaybackClockSnapshot before{};
  PlaybackClockSnapshot after{};
  std::string reason;
};

struct PlaybackEnded {
  TrackPlaybackRequest request{};
  PlaybackClockSnapshot finalClock{};
};

struct OutputFormatChanged {
  AudioOutputConfig requestedConfig{};
  AudioDeviceFormat deviceFormat{};
};

struct OutputModeFallback {
  AudioOutputConfig requestedConfig{};
  AudioOutputConfig effectiveConfig{};
  std::string reason;
};

struct PlaybackError {
  PlaybackErrorCode code{PlaybackErrorCode::OpenFailed};
  std::string message;
  std::string detail;
  std::optional<PlaybackClockSnapshot> clock;
};

using PlaybackEvent = std::variant<
    PlaybackStateChanged,
    TrackChanged,
    PlaybackPositionUpdated,
    PositionDiscontinuity,
    PlaybackEnded,
    OutputFormatChanged,
    OutputModeFallback,
    PlaybackError>;

struct BackendEvent {
  BackendEventType type{BackendEventType::PlaybackStateChanged};
  BackendSourceModule sourceModule{BackendSourceModule::AudioPlaybackService};
  std::uint64_t monotonicVersion{0};
  std::chrono::steady_clock::time_point timestamp{};
  PlaybackEvent payload{};
};

using BackendEventSink = std::function<void(BackendEvent)>;

class AudioPlaybackService {
public:
  virtual ~AudioPlaybackService() = default;

  virtual void setEventSink(BackendEventSink sink) = 0;
  virtual void configureOutput(const AudioOutputConfig& config) = 0;
  virtual void loadTrack(const TrackPlaybackRequest& request) = 0;
  virtual void prepareNext(const TrackPlaybackRequest& request) = 0;
  virtual void play() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;
  virtual void seek(std::chrono::milliseconds position) = 0;
  virtual void setVolume(float linearGain) = 0;
  virtual void setMuted(bool muted) = 0;
  virtual void selectOutputDevice(const std::string& deviceId) = 0;
  [[nodiscard]] virtual PlaybackClockSnapshot queryPlaybackClock() const = 0;
};

class AudioPlayer {
public:
  AudioPlayer() = default;
  explicit AudioPlayer(std::shared_ptr<AudioPlaybackService> service);

  void setPlaybackService(std::shared_ptr<AudioPlaybackService> service);
  void setEventSink(BackendEventSink sink);
  void configureOutput(const AudioOutputConfig& config);
  void loadTrack(const TrackPlaybackRequest& request);
  void prepareNext(const TrackPlaybackRequest& request);
  void play();
  void pause();
  void resume();
  void stop();
  void seek(std::chrono::milliseconds position);
  void setVolume(float linearGain);
  void setMuted(bool muted);
  void selectOutputDevice(const std::string& deviceId);
  [[nodiscard]] PlaybackClockSnapshot queryPlaybackClock() const;

private:
  std::shared_ptr<AudioPlaybackService> service_;
};

}
