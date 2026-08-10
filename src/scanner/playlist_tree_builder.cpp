#include "seriona/scanner/playlist_tree_builder.h"

#include "spdlog/spdlog.h"

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

[[nodiscard]] bool isCueContainer(const PlaylistTreeSong& song) {
  return song.relativePath.extension() == ".cue" && song.metadata.filePath == song.relativePath &&
         song.metadata.sourceFilePath.empty() && !song.metadata.offset.has_value();
}

[[nodiscard]] bool isCueTrack(const PlaylistTreeSong& song) {
  return song.relativePath.extension() == ".cue" && !song.metadata.sourceFilePath.empty() &&
         song.metadata.offset.has_value();
}

[[nodiscard]] std::string songKeyFor(const PlaylistTreeSong& song) {
  if (isCueTrack(song)) {
    if (!song.metadata.logicalTrackId.empty()) {
      return song.metadata.logicalTrackId;
    }
    return pathKey(song.relativePath) + "#" + pathKey(song.metadata.sourceFilePath) + "@" +
           std::to_string(song.metadata.offset->count());
  }
  return pathKey(song.relativePath);
}

struct MutableNode {
  PlaylistNode node{};
  std::filesystem::path relativePath{"."};
  PlaylistTreeStats stats{};
  bool explicitDirectory{false};
  bool virtualDirectory{false};
};

// MutableNode 变体的 cue 轨判定：cue 轨的父节点是 cue 虚拟目录（键 = relativePath 本身），
// 而非 parent_path —— 与 songKeyFor/isCueTrack 使用同一组判别特征。
[[nodiscard]] bool isCueTrackNode(const MutableNode& entry) {
  return entry.node.kind == PlaylistNodeKind::Track && entry.node.song.has_value() &&
         entry.relativePath.extension() == ".cue" && !entry.node.song->sourceFilePath.empty() &&
         entry.node.song->offset.has_value();
}

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
    const auto songKey = songKeyFor(song);
    songKeys.insert(songKey);
    auto& parent = isCueTrack(song) ? ensureDirectory(song.relativePath, displayNameFor(song.relativePath, songKey))
                                    : ensureDirectory(parentPathOf(song.relativePath), "Library");
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

  // upsertSong 私有实现：key 定位（与 addSongNode/songKeyFor 同一规则）。节点已存在
  // （modify）时原地更新 song 元数据 + displayName，保留 nodeId（同 key 同 nodeId），
  // 不重复添加；不存在（create）时复用 addSongNode 插入新节点。返回是否新插入。
  bool upsertSongNode(PlaylistTreeSong song) {
    const auto songKey = songKeyFor(song);
    const auto existing = nodes.find(songKey);
    if (existing != nodes.end() && existing->second.node.kind == PlaylistNodeKind::Track) {
      auto& entry = existing->second;
      entry.relativePath = song.relativePath.lexically_normal();
      entry.node.displayName = song.metadata.title.empty() ? displayNameFor(song.relativePath, songKey)
                                                           : song.metadata.title;
      entry.node.song = std::move(song.metadata);
      if (entry.node.song->trackId.empty()) {
        entry.node.song->trackId = songKey;
      }
      if (entry.node.song->filePath.empty()) {
        entry.node.song->filePath = song.relativePath;
      }
      songKeys.insert(songKey);
      rebuildChildren();
      static_cast<void>(recomputeStats("."));
      return false;
    }
    addSongNode(std::move(song));
    rebuildChildren();
    static_cast<void>(recomputeStats("."));
    return true;
  }

  void addVirtualDirectoryNode(PlaylistTreeSong song) {
    auto& entry = ensureDirectory(song.relativePath, displayNameFor(song.relativePath, pathKey(song.relativePath)));
    entry.explicitDirectory = true;
    entry.virtualDirectory = true;
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
                               iterator->second.node.childNodeIds.empty() && !iterator->second.virtualDirectory;
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

  bool removeSubtree(const std::filesystem::path& relativePath) {
    const auto targetKey = pathKey(relativePath);
    if (targetKey == ".") {
      return false;
    }
    const auto target = nodes.find(targetKey);
    if (target == nodes.end()) {
      return false;
    }
    if (target->second.node.parentNodeId.has_value()) {
      const auto parentKey = keyFromNodeId(*target->second.node.parentNodeId);
      const auto parent = nodes.find(parentKey);
      if (parent != nodes.end()) {
        std::erase(parent->second.node.childNodeIds, target->second.node.nodeId);
      }
    }
    std::vector<std::string> pending{target->second.node.nodeId};
    std::vector<std::string> removedKeys;
    while (!pending.empty()) {
      const auto nodeId = pending.back();
      pending.pop_back();
      const auto key = keyFromNodeId(nodeId);
      const auto entry = nodes.find(key);
      if (entry == nodes.end()) {
        continue;
      }
      removedKeys.push_back(key);
      for (const auto& childId : entry->second.node.childNodeIds) {
        pending.push_back(childId);
      }
    }
    for (const auto& key : removedKeys) {
      const auto entry = nodes.find(key);
      if (entry == nodes.end()) {
        continue;
      }
      if (entry->second.node.kind == PlaylistNodeKind::Track && entry->second.node.song.has_value()) {
        songKeys.erase(key);
      }
      nodes.erase(entry);
    }
    rebuildChildren();
    pruneEmptyDirectories();
    static_cast<void>(recomputeStats("."));
    return true;
  }

  bool renameSubtree(const std::filesystem::path& oldRelativePath, const std::filesystem::path& newRelativePath) {
    const auto oldKey = pathKey(oldRelativePath);
    const auto newKey = pathKey(newRelativePath);
    if (oldKey == "." || newKey == "." || oldKey == newKey) {
      return false;
    }
    if (nodes.find(oldKey) == nodes.end()) {
      return false;
    }
    if (const auto existing = nodes.find(newKey); existing != nodes.end() && existing->first != oldKey) {
      return false;
    }

    std::map<std::string, std::string> keyRewrites;
    for (const auto& [key, _] : nodes) {
      if (key == oldKey) {
        keyRewrites[key] = newKey;
      } else if (key.rfind(oldKey + "/", 0) == 0) {
        keyRewrites[key] = newKey + key.substr(oldKey.size());
      }
    }
    for (const auto& [key, updatedKey] : keyRewrites) {
      if (updatedKey.empty() || updatedKey == ".") {
        return false;
      }
      if (const auto collision = nodes.find(updatedKey); collision != nodes.end() && collision->first != key) {
        return false;
      }
    }
    if (keyRewrites.empty()) {
      return false;
    }

    const auto rewrittenKeyOf = [&keyRewrites](const std::string& key) {
      const auto iterator = keyRewrites.find(key);
      return iterator == keyRewrites.end() ? key : iterator->second;
    };
    const auto rewriteText = [&oldKey, &newKey](const std::string& text) {
      if (text == oldKey) {
        return newKey;
      }
      if (text.rfind(oldKey + "/", 0) == 0) {
        return newKey + text.substr(oldKey.size());
      }
      return text;
    };

    std::map<std::string, MutableNode> moved;
    for (auto& [key, entry] : nodes) {
      const auto updatedKey = rewrittenKeyOf(key);
      auto movedEntry = std::move(entry);
      const auto oldRelativeText = pathKey(movedEntry.relativePath);
      const auto updatedRelativeText = rewriteText(oldRelativeText);
      if (updatedRelativeText != oldRelativeText) {
        movedEntry.relativePath = std::filesystem::path{updatedRelativeText};
      }
      const bool keyChanged = updatedKey != key;
      if (keyChanged) {
        const auto delimiter = movedEntry.node.nodeId.find(':');
        const auto prefix = delimiter == std::string::npos ? "" : movedEntry.node.nodeId.substr(0, delimiter);
        movedEntry.node.nodeId = nodeIdFor(prefix, updatedKey);
        if (movedEntry.node.kind == PlaylistNodeKind::Directory) {
          movedEntry.node.displayName = displayNameFor(movedEntry.relativePath, movedEntry.node.displayName);
        }
      }
      if (movedEntry.node.kind == PlaylistNodeKind::Track && movedEntry.node.song.has_value()) {
        auto& song = *movedEntry.node.song;
        if (song.title.empty() && (keyChanged || updatedRelativeText != oldRelativeText)) {
          movedEntry.node.displayName = displayNameFor(movedEntry.relativePath, movedEntry.node.displayName);
        }
        if (updatedRelativeText != oldRelativeText) {
          if (!song.filePath.empty()) {
            song.filePath = std::filesystem::path{rewriteText(pathKey(song.filePath))};
          }
          if (!song.sourceFilePath.empty()) {
            song.sourceFilePath = std::filesystem::path{rewriteText(pathKey(song.sourceFilePath))};
          }
          song.logicalTrackId = rewriteText(song.logicalTrackId);
          song.trackId = rewriteText(song.trackId);
        }
      }
      moved.insert_or_assign(updatedKey, std::move(movedEntry));
    }
    nodes = std::move(moved);

    std::set<std::string> updatedSongKeys;
    for (const auto& songKey : songKeys) {
      updatedSongKeys.insert(rewrittenKeyOf(songKey));
    }
    songKeys = std::move(updatedSongKeys);

    const auto parentKeyOf = [](const MutableNode& entry) {
      if (isCueTrackNode(entry)) {
        return pathKey(entry.relativePath);
      }
      return pathKey(parentPathOf(entry.relativePath));
    };
    std::set<std::string> missingParents;
    for (const auto& [key, entry] : nodes) {
      if (key == ".") {
        continue;
      }
      const auto parentKey = parentKeyOf(entry);
      if (parentKey == key || nodes.find(parentKey) != nodes.end()) {
        continue;
      }
      missingParents.insert(parentKey);
    }
    for (const auto& parentKey : missingParents) {
      (void)ensureDirectory(std::filesystem::path{parentKey}, "Library");
    }
    for (auto& [key, entry] : nodes) {
      if (key == "." || entry.node.kind == PlaylistNodeKind::Root) {
        entry.node.parentNodeId.reset();
        continue;
      }
      const auto parentKey = parentKeyOf(entry);
      if (parentKey == key) {
        continue;
      }
      const auto parent = nodes.find(parentKey);
      if (parent != nodes.end()) {
        entry.node.parentNodeId = parent->second.node.nodeId;
      }
    }

    rebuildChildren();
    pruneEmptyDirectories();
    static_cast<void>(recomputeStats("."));
    return true;
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
  if (isCueContainer(song)) {
    impl_->addVirtualDirectoryNode(std::move(song));
    return;
  }
  impl_->addSongNode(std::move(song));
}

bool PlaylistTreeBuilder::upsertSong(PlaylistTreeSong song) {
  if (isCueContainer(song)) {
    const auto containerKey = pathKey(song.relativePath);
    const bool inserted = impl_->nodes.find(containerKey) == impl_->nodes.end();
    impl_->addVirtualDirectoryNode(std::move(song));
    return inserted;
  }
  return impl_->upsertSongNode(std::move(song));
}

bool PlaylistTreeBuilder::removeSubtree(const std::filesystem::path& relativePath) {
  return impl_->removeSubtree(relativePath);
}

bool PlaylistTreeBuilder::renameSubtree(const std::filesystem::path& oldRelativePath,
                                        const std::filesystem::path& newRelativePath) {
  return impl_->renameSubtree(oldRelativePath, newRelativePath);
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
  const auto treeStats = impl_->nodes.at(".").stats;
  spdlog::debug("playlist tree published: {} nodes, {} songs, {}ms total duration", snapshot.nodes.size(),
                treeStats.songCount, treeStats.totalDuration.count());
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
