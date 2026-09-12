#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

struct PlaylistTreeStats {
  std::uint64_t songCount{0};
  std::chrono::milliseconds totalDuration{0};
};

struct PlaylistTreeDirectory {
  std::filesystem::path relativePath;
  std::string displayName;
};

struct PlaylistTreeSong {
  std::filesystem::path relativePath;
  SongMetadata metadata;
};

class PlaylistTreeBuilder {
public:
  explicit PlaylistTreeBuilder(std::string rootDisplayName = "Library");
  ~PlaylistTreeBuilder();

  PlaylistTreeBuilder(const PlaylistTreeBuilder&) = delete;
  PlaylistTreeBuilder& operator=(const PlaylistTreeBuilder&) = delete;
  PlaylistTreeBuilder(PlaylistTreeBuilder&&) noexcept;
  PlaylistTreeBuilder& operator=(PlaylistTreeBuilder&&) noexcept;

  void addDirectory(PlaylistTreeDirectory directory);
  void addSong(PlaylistTreeSong song);
  bool upsertSong(PlaylistTreeSong song);
  bool removeSubtree(const std::filesystem::path& relativePath);
  bool renameSubtree(const std::filesystem::path& oldRelativePath, const std::filesystem::path& newRelativePath);
  void attachExternalLyrics(const std::filesystem::path& audioRelativePath,
                            const std::filesystem::path& lrcRelativePath,
                            std::string lrcHash,
                            std::filesystem::file_time_type lrcMtime,
                            std::vector<LyricLine> lyrics);
  [[nodiscard]] PlaylistTreeSnapshot publish();
  [[nodiscard]] PlaylistTreeStats stats() const noexcept;

private:
  std::string rootDisplayName_;
  std::uint64_t nextVersion_{1};
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
