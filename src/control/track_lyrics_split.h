#pragma once

#include "seriona/control/control_contracts.h"
#include "seriona/control/lyric_split_store.h"

#include "lyric_split.h"  // LyricSplitResult / LyricSplitConvention

#include <functional>
#include <optional>
#include <string_view>

// 控制层惰性切分（内部模块，非 inc/ 公共契约）。
//
// 「当前曲目 → 逐行结果」的取数流程：库树取 effectiveLyrics → cleanLine 全量 →
// inferSplitConvention 得文档级约定 → 逐行以 (rawText, target_lang, conv) 查 store →
// 未命中则先试同 timestamp 参照行配对、否则 splitLyricLine，并 putAuto → 组装快照。
//
// 本模块是纯同步函数，不做调度：调用方（MediaController）负责在工作线程上调用它。
namespace seriona::control {

// 切分函数注入点。生产用默认（算法层 splitLyricLine）；测试可注入计数探针，
// 证明命中 auto 行时不重复切分。
using LyricLineSplitFn = std::function<LyricSplitResult(std::string_view line,
                                                       LyricSplitConvention convention,
                                                       std::string_view targetLanguage)>;

// 返回 nullopt 表示无需展示：无当前曲目、库树中找不到该曲目、或该曲目清洗后无正文行。
[[nodiscard]] std::optional<TrackLyricsSnapshot> buildTrackLyricsSnapshot(
    const PlayerStateSnapshot& player,
    const LibraryStateSnapshot& library,
    std::string_view targetLanguage,
    LyricSplitStore& store,
    const LyricLineSplitFn& splitLine = {});

}
