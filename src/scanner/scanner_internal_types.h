#pragma once

#include "seriona/scanner/cache/sqlite_cache.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace seriona::scanner {

enum class NodeType {
  Directory,
  Song,
  CueContainer,
  CueTrack,
};

enum class ScanItemOrigin {
  CacheHit,
  CueTrackCacheHit,
  RescannedChanged,
  ScannedNew,
  ScannedFull,
  CueTrackRescannedChanged,
  CueTrackScannedNew,
  VirtualContainer,
};

struct CueInfo {
  std::filesystem::path cueFilePath;
  std::filesystem::path audioFilePath;
  std::chrono::microseconds offset{0};
  std::chrono::microseconds duration{0};
  std::size_t trackIndex{0};
};

struct IndexedPublishedSong {
  std::size_t discoveryIndex{0};
  cache::CachedSong song;
  std::filesystem::path treeRelativePath;
  NodeType nodeType{NodeType::Song};
  std::optional<CueInfo> cueInfo;
  ScanItemOrigin origin{ScanItemOrigin::ScannedFull};
  std::optional<std::string> locationId;
  std::atomic<bool> filled{false};
  std::atomic<bool> needsScan{true};
  bool isVirtualFolder{false};

  IndexedPublishedSong() = default;

  IndexedPublishedSong(std::size_t index, cache::CachedSong s, std::filesystem::path path)
      : discoveryIndex(index), song(std::move(s)), treeRelativePath(std::move(path)) {}

  IndexedPublishedSong(IndexedPublishedSong&& other) noexcept
      : discoveryIndex(other.discoveryIndex),
        song(std::move(other.song)),
        treeRelativePath(std::move(other.treeRelativePath)),
        nodeType(other.nodeType),
        cueInfo(std::move(other.cueInfo)),
        origin(other.origin),
        locationId(std::move(other.locationId)),
        filled(other.filled.load()),
        needsScan(other.needsScan.load()),
        isVirtualFolder(other.isVirtualFolder) {}

  IndexedPublishedSong& operator=(IndexedPublishedSong&& other) noexcept {
    if (this != &other) {
      discoveryIndex = other.discoveryIndex;
      song = std::move(other.song);
      treeRelativePath = std::move(other.treeRelativePath);
      nodeType = other.nodeType;
      cueInfo = std::move(other.cueInfo);
      origin = other.origin;
      locationId = std::move(other.locationId);
      filled.store(other.filled.load());
      needsScan.store(other.needsScan.load());
      isVirtualFolder = other.isVirtualFolder;
    }
    return *this;
  }

  IndexedPublishedSong(const IndexedPublishedSong&) = delete;
  IndexedPublishedSong& operator=(const IndexedPublishedSong&) = delete;
};

}
