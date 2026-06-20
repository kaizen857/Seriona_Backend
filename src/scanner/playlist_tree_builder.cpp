#include "seriona/scanner/playlist_tree_builder.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] std::string pathKey(const std::filesystem::path& path) {
  const auto normalized = path.lexically_normal();
  const auto text = normalized.generic_u8string();
  if (text.empty() || text == u8".") {
    return ".";
  }
  return {text.begin(), text.end()};
}

[[nodiscard]] std::string nodeIdFor(const std::string& prefix, const std::string& key) {
  return prefix + ":" + key;
}

[[nodiscard]] std::filesystem::path parentPathOf(const std::filesystem::path& relativePath) {
  const auto parent = relativePath.lexically_normal().parent_path();
  return parent.empty() ? std::filesystem::path{"."} : parent;
}

[[nodiscard]] std::string displayNameFor(const std::filesystem::path& path, const std::string& fallback) {
  const auto filename = path.filename().generic_u8string();
  if (!filename.empty()) {
    return {filename.begin(), filename.end()};
  }
  return fallback;
}

[[nodiscard]] std::chrono::milliseconds durationOf(const SongMetadata& metadata) noexcept {
  return metadata.duration.value_or(std::chrono::milliseconds{0});
}

struct MutableNode {
  PlaylistNode node{};
  std::filesystem::path relativePath{"."};
  PlaylistTreeStats stats{};
  bool explicitDirectory{false};
};

}

struct PlaylistTreeBuilder::Impl {
  std::map<std::string, MutableNode> nodes{};
  std::set<std::string> songKeys{};

  MutableNode& ensureDirectory(const std::filesystem::path& relativePath, const std::string& displayName) {
    const auto key = pathKey(relativePath);
    auto [iterator, inserted] = nodes.try_emplace(key);
    auto& entry = iterator->second;
    if (inserted) {
      entry.relativePath = relativePath.lexically_normal();
      entry.node.nodeId = nodeIdFor(key == "." ? "root" : "dir", key);
      entry.node.kind = key == "." ? PlaylistNodeKind::Root : PlaylistNodeKind::Directory;
      entry.node.displayName = key == "." ? displayName : displayNameFor(relativePath, displayName);
      if (key != ".") {
        const auto parent = ensureDirectory(parentPathOf(relativePath), "Library").node.nodeId;
        entry.node.parentNodeId = parent;
      }
    }
    entry.explicitDirectory = entry.explicitDirectory || key == "." || !displayName.empty();
    if (!displayName.empty() && key != ".") {
      entry.node.displayName = displayNameFor(relativePath, displayName);
    }
    return entry;
  }

  void addSongNode(PlaylistTreeSong song) {
    const auto songKey = pathKey(song.relativePath);
    songKeys.insert(songKey);
    auto& parent = ensureDirectory(parentPathOf(song.relativePath), "Library");
    auto& entry = nodes[songKey];
    entry.relativePath = song.relativePath.lexically_normal();
    entry.node.nodeId = nodeIdFor("track", songKey);
    entry.node.parentNodeId = parent.node.nodeId;
    entry.node.kind = PlaylistNodeKind::Track;
    entry.node.displayName = song.metadata.title.empty() ? displayNameFor(song.relativePath, songKey) : song.metadata.title;
    entry.node.song = std::move(song.metadata);
    if (entry.node.song->trackId.empty()) {
      entry.node.song->trackId = songKey;
    }
    if (entry.node.song->filePath.empty()) {
      entry.node.song->filePath = song.relativePath;
    }
  }

  [[nodiscard]] PlaylistTreeStats recomputeStats(const std::string& key) {
    auto& entry = nodes.at(key);
    PlaylistTreeStats stats{};
    if (entry.node.kind == PlaylistNodeKind::Track && entry.node.song.has_value()) {
      stats.songCount = 1;
      stats.totalDuration = durationOf(*entry.node.song);
    } else {
      for (const auto& childId : entry.node.childNodeIds) {
        const auto childKey = keyFromNodeId(childId);
        const auto childStats = recomputeStats(childKey);
        stats.songCount += childStats.songCount;
        stats.totalDuration += childStats.totalDuration;
      }
    }
    entry.stats = stats;
    return stats;
  }

  [[nodiscard]] static std::string keyFromNodeId(const std::string& nodeId) {
    const auto delimiter = nodeId.find(':');
    if (delimiter == std::string::npos) {
      return nodeId;
    }
    return nodeId.substr(delimiter + 1U);
  }

  void rebuildChildren() {
    for (auto& [_, entry] : nodes) {
      entry.node.childNodeIds.clear();
    }
    for (const auto& [key, entry] : nodes) {
      if (key == "." || !entry.node.parentNodeId.has_value()) {
        continue;
      }
      const auto parentKey = keyFromNodeId(*entry.node.parentNodeId);
      nodes[parentKey].node.childNodeIds.push_back(entry.node.nodeId);
    }
    for (auto& [_, entry] : nodes) {
      std::ranges::sort(entry.node.childNodeIds, [this](const std::string& lhsId, const std::string& rhsId) {
        const auto& lhs = nodes.at(keyFromNodeId(lhsId)).node;
        const auto& rhs = nodes.at(keyFromNodeId(rhsId)).node;
        const auto lhsDir = lhs.kind == PlaylistNodeKind::Directory;
        const auto rhsDir = rhs.kind == PlaylistNodeKind::Directory;
        if (lhsDir != rhsDir) {
          return lhsDir;
        }
        return lhs.displayName < rhs.displayName;
      });
    }
  }

  void pruneEmptyDirectories() {
    bool removed = true;
    while (removed) {
      removed = false;
      for (auto iterator = nodes.begin(); iterator != nodes.end();) {
        const auto removable = iterator->first != "." && iterator->second.node.kind == PlaylistNodeKind::Directory &&
                               iterator->second.node.childNodeIds.empty();
        if (removable) {
          iterator = nodes.erase(iterator);
          removed = true;
        } else {
          ++iterator;
        }
      }
      if (removed) {
        rebuildChildren();
      }
    }
  }
};

PlaylistTreeBuilder::PlaylistTreeBuilder(std::string rootDisplayName)
    : rootDisplayName_(std::move(rootDisplayName)), impl_(std::make_unique<Impl>()) {
  impl_->ensureDirectory(".", rootDisplayName_);
}

PlaylistTreeBuilder::~PlaylistTreeBuilder() = default;
PlaylistTreeBuilder::PlaylistTreeBuilder(PlaylistTreeBuilder&&) noexcept = default;
PlaylistTreeBuilder& PlaylistTreeBuilder::operator=(PlaylistTreeBuilder&&) noexcept = default;

void PlaylistTreeBuilder::addDirectory(PlaylistTreeDirectory directory) {
  impl_->ensureDirectory(directory.relativePath, directory.displayName);
}

void PlaylistTreeBuilder::addSong(PlaylistTreeSong song) {
  impl_->addSongNode(std::move(song));
}

void PlaylistTreeBuilder::attachExternalLyrics(const std::filesystem::path& audioRelativePath,
                                               const std::filesystem::path& lrcRelativePath,
                                               std::string lrcHash,
                                               std::filesystem::file_time_type lrcMtime,
                                               std::vector<LyricLine> lyrics) {
  const auto songKey = pathKey(audioRelativePath);
  auto iterator = impl_->nodes.find(songKey);
  if (iterator == impl_->nodes.end() || !iterator->second.node.song.has_value()) {
    return;
  }
  auto& song = *iterator->second.node.song;
  song.effectiveLyricsSource = LyricsSource::ExternalLrc;
  song.effectiveLyrics = std::move(lyrics);
  song.externalLyricsPath = lrcRelativePath;
  song.externalLyricsHash = std::move(lrcHash);
  song.externalLyricsMtime = lrcMtime;
}

PlaylistTreeSnapshot PlaylistTreeBuilder::publish() {
  impl_->rebuildChildren();
  impl_->pruneEmptyDirectories();
  static_cast<void>(impl_->recomputeStats("."));

  PlaylistTreeSnapshot snapshot{};
  snapshot.version = nextVersion_++;
  snapshot.generatedAt = std::chrono::steady_clock::now();
  snapshot.rootNodeId = impl_->nodes.at(".").node.nodeId;

  for (const auto& [_, entry] : impl_->nodes) {
    snapshot.nodes.push_back(entry.node);
  }
  std::ranges::sort(snapshot.nodes, [](const PlaylistNode& lhs, const PlaylistNode& rhs) {
    if (lhs.kind != rhs.kind) {
      return lhs.kind < rhs.kind;
    }
    return lhs.nodeId < rhs.nodeId;
  });
  return snapshot;
}

PlaylistTreeStats PlaylistTreeBuilder::stats() const noexcept {
  const auto iterator = impl_->nodes.find(".");
  if (iterator == impl_->nodes.end()) {
    return {};
  }
  return iterator->second.stats;
}

}
