#include "seriona/audio/audio_contracts.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <variant>

namespace {

enum class PlaybackResult {
  Running,
  Completed,
  Failed,
};

struct PlaybackStatus {
  PlaybackResult result{PlaybackResult::Running};
  std::string message{};
};

void printUsage(const char* programName) {
  std::cerr << "Usage: " << programName << " /path/to/musicFile\n";
}

seriona::audio::TrackPlaybackRequest makeTrackRequest(const std::filesystem::path& filePath) {
  seriona::audio::TrackPlaybackRequest request{};
  request.trackId = filePath.string();
  request.filePath = filePath;
  request.title = filePath.filename().string();
  return request;
}

std::string playbackErrorMessage(const seriona::audio::PlaybackError& error) {
  if (error.detail.empty()) {
    return error.message;
  }

  return error.message + ": " + error.detail;
}

}

int main(int argc, char** argv) {
  if (argc != 2) {
    printUsage(argv[0]);
    return 2;
  }

  const std::filesystem::path filePath{argv[1]};

  seriona::audio::AudioPlayer player;
  std::mutex mutex;
  std::condition_variable statusChanged;
  PlaybackStatus status{};

  player.setEventSink([&](seriona::audio::BackendEvent event) {
    std::lock_guard lock{mutex};
    if (std::holds_alternative<seriona::audio::PlaybackEnded>(event.payload)) {
      status.result = PlaybackResult::Completed;
      statusChanged.notify_one();
      return;
    }

    if (const auto* error = std::get_if<seriona::audio::PlaybackError>(&event.payload); error != nullptr) {
      status.result = PlaybackResult::Failed;
      status.message = playbackErrorMessage(*error);
      statusChanged.notify_one();
    }
  });

  player.loadTrack(makeTrackRequest(filePath));
  {
    std::lock_guard lock{mutex};
    if (status.result == PlaybackResult::Failed) {
      std::cerr << "seriona: " << status.message << '\n';
      return 1;
    }
  }

  std::cout << "seriona: playing " << filePath << '\n';
  player.play();

  std::unique_lock lock{mutex};
  while (status.result == PlaybackResult::Running) {
    lock.unlock();
    const auto clock = player.queryPlaybackClock();
    std::cout << "\rseriona: position " << std::setw(6) << clock.position.count() << " ms" << std::flush;
    lock.lock();
    statusChanged.wait_for(lock, std::chrono::milliseconds{50});
  }
  std::cout << '\n';

  if (status.result == PlaybackResult::Failed) {
    std::cerr << "seriona: " << status.message << '\n';
    return 1;
  }

  std::cout << "seriona: playback finished\n";
  return 0;
}
