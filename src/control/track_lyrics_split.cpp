#include "track_lyrics_split.h"

#include "seriona/scanner/lyric_reference_pairing.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace seriona::control {
namespace {

// 成功切分 = reason 不在「未切分」集合内 且 译文非空（SA-33）。
[[nodiscard]] bool splitSucceeded(const std::string_view reason, const std::string_view translation) {
  if (translation.empty()) {
    return false;
  }
  return reason != "empty" && reason != "qq-sentinel" && reason != "no-convention" &&
         reason != "validate-failed";
}

struct AutoOutcome {
  std::string original;
  std::string translation;
  bool split{false};
};

[[nodiscard]] const scanner::SongMetadata* findSong(const LibraryStateSnapshot& library,
                                                    const std::string_view trackId) {
  if (!library.libraryTree.has_value()) {
    return nullptr;
  }
  for (const auto& node : library.libraryTree->nodes) {
    if (node.song.has_value() && node.song->trackId == trackId) {
      return &*node.song;
    }
  }
  return nullptr;
}

}

std::optional<TrackLyricsSnapshot> buildTrackLyricsSnapshot(const PlayerStateSnapshot& player,
                                                            const LibraryStateSnapshot& library,
                                                            const std::string_view targetLanguage,
                                                            LyricSplitStore& store,
                                                            const LyricLineSplitFn& splitLine) {
  if (!player.currentTrack.has_value()) {
    return std::nullopt;
  }
  const auto* song = findSong(library, player.currentTrack->trackId);
  if (song == nullptr) {
    return std::nullopt;
  }

  // ① 清洗全量行（保留时间戳），丢弃 cleanLine 判定为非正文的行。
  std::vector<scanner::LyricLine> cleanedLines;
  cleanedLines.reserve(song->effectiveLyrics.size());
  for (const auto& line : song->effectiveLyrics) {
    if (const auto cleaned = cleanLine(line.text); cleaned.has_value()) {
      cleanedLines.push_back(scanner::LyricLine{.timestamp = line.timestamp, .text = *cleaned});
    }
  }
  if (cleanedLines.empty()) {
    return std::nullopt;
  }

  // ② 文档级约定：整首一次，是切分与 store 键的决定性输入。
  std::vector<std::string> cleanedTexts;
  cleanedTexts.reserve(cleanedLines.size());
  for (const auto& line : cleanedLines) {
    cleanedTexts.push_back(line.text);
  }
  const LyricSplitConvention convention = inferSplitConvention(cleanedTexts, targetLanguage).convention;

  // ⑨ 参照行配对：在已清洗行上做（§8.8 要求配对前先清洗），配对优先于行内分隔符路径。
  const std::size_t lineCount = cleanedLines.size();
  const std::vector<scanner::LyricReferenceGroup> groups = scanner::groupLyricReferenceLines(cleanedLines);
  std::vector<bool> consumedByGroup(lineCount, false);
  std::vector<int> groupOriginalOf(lineCount, -1);
  for (std::size_t g = 0; g < groups.size(); ++g) {
    if (groups[g].originalIndex >= lineCount) {
      continue;
    }
    groupOriginalOf[groups[g].originalIndex] = static_cast<int>(g);
    for (const auto index : groups[g].translationIndexes) {
      if (index < lineCount) {
        consumedByGroup[index] = true;
      }
    }
  }

  const LyricLineSplitFn splitter = splitLine ? splitLine : LyricLineSplitFn{&splitLyricLine};

  const auto computeAuto = [&](const std::size_t index) -> AutoOutcome {
    const std::string& text = cleanedLines[index].text;
    const int group = groupOriginalOf[index];
    if (group >= 0) {
      std::string merged;
      for (const auto partner : groups[static_cast<std::size_t>(group)].translationIndexes) {
        if (partner >= lineCount) {
          continue;
        }
        if (!merged.empty()) {
          merged.push_back('\n');
        }
        merged += cleanedLines[partner].text;
      }
      if (!merged.empty()) {
        return AutoOutcome{.original = text, .translation = std::move(merged), .split = true};
      }
    }
    const LyricSplitResult result = splitter(text, convention, targetLanguage);
    return AutoOutcome{.original = result.original,
                       .translation = result.translation,
                       .split = splitSucceeded(result.reason, result.translation)};
  };

  TrackLyricsSnapshot snapshot;
  snapshot.trackId = song->trackId;
  snapshot.targetLanguage = std::string(targetLanguage);
  snapshot.convention = convention;
  snapshot.position = TranslationPosition::LastLanguageSegment;

  for (std::size_t i = 0; i < lineCount; ++i) {
    if (consumedByGroup[i]) {
      continue;
    }
    const std::string& text = cleanedLines[i].text;
    // ③ 先查表：load 已保证 manual 优先、auto 需 algo_version 匹配。
    const auto loaded = store.load(text, targetLanguage, convention);

    SplitLyricLine row;
    row.timestamp = cleanedLines[i].timestamp;
    row.text = text;

    if (loaded.has_value() && loaded->source == LyricSplitSource::Manual) {
      // ⑤ 命中 manual：用它，并把「若走 auto 会得到的」结果留作展示（不额外查表）。
      row.original = loaded->original;
      row.translation = loaded->translation;
      row.split = !loaded->translation.empty();
      row.manualOverride = true;
      const AutoOutcome autoOutcome = computeAuto(i);
      row.autoOriginal = autoOutcome.original;
      row.autoTranslation = autoOutcome.translation;
    } else if (loaded.has_value()) {
      row.original = loaded->original;
      row.translation = loaded->translation;
      row.split = !loaded->translation.empty();
    } else {
      // ④ 未命中：配对优先，否则算法切分，然后写 auto 行。
      const AutoOutcome autoOutcome = computeAuto(i);
      store.putAuto(LyricSplitEntry{.rawText = text,
                                    .targetLanguage = snapshot.targetLanguage,
                                    .convention = convention,
                                    .original = autoOutcome.original,
                                    .translation = autoOutcome.translation,
                                    .source = LyricSplitSource::Auto});
      row.original = autoOutcome.original;
      row.translation = autoOutcome.translation;
      row.split = autoOutcome.split;
    }
    snapshot.lines.push_back(std::move(row));
  }

  return snapshot;
}

}
