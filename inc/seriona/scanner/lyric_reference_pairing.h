#pragma once

#include "seriona/scanner/scanner_contracts.h"  // scanner::LyricLine

#include <chrono>
#include <cstddef>
#include <vector>

// D22 同时间戳参照行（ESLyric 约定）：同一 timestamp 的多行里，**第 1 行是原文**，其余行是
// 候选译文。分组只以 timestamp 为键 —— ESLyric 明文「参照行不要求为连续行」，因此不得依赖
// 行相邻判定（用 map/分组，不是「看上一行」）。
//
// 归属本层（scanner）而非 control 的理由：解析期产出行序列的是 scanner（`lrc_parser.cpp`
// 的 todo 11 稳定序是 D22 的前提），本函数只消费 `scanner::LyricLine` 序列；control 层的
// 消费者（todo 26 惰性切分）沿既有的 control → scanner 依赖方向取用。
//
// 消费者：控制层惰性切分（todo 26）。取到 `song.effectiveLyrics` 后先调本函数；命中的组直接
// 产出原文/译文，不再走行内分隔符路径（行内切分为高优先、参照行为其补充，编排在 todo 26）。
// 本函数是纯函数，不在库树里做配对，也不写任何持久状态。
namespace seriona::scanner {

struct LyricReferenceGroup {
  std::chrono::milliseconds timestamp{0};
  // 组内第 1 行（原文）在输入序列中的下标。
  std::size_t originalIndex{0};
  // 组内第 2..n 行（候选译文）在输入序列中的下标，保持输入顺序。
  std::vector<std::size_t> translationIndexes;
  // 组内行数 > 2，或组内出现文本重复时为真；内部置信度标记，不进任何快照。
  bool confidenceReduced{false};
};

// 按 timestamp 分组，只返回含 ≥2 行的组（单行组没有参照关系）。
// · unsynced 哨兵行（timestamp < 0）不参与分组，也不出现在任何组中。**timestamp == 0 是合法
//   时间戳**（0ms），正常参与分组 —— 判据是「< 0」，不是「== 0」。同理，两个同为哨兵的行
//   不构成组。
// · 返回的组按 timestamp 升序；组内下标保持输入顺序，因此「第 1 行 = 原文」以**输入顺序**
//   为准，要求输入已是 todo 11 的「仅按 timestamp 的稳定序」（parseLrcText 与
//   parsePlainTextLyrics 均保证），才等价于「文件中的先出现者」。
// · 组内首行没有正文（空白 / 整行仅为行内元数据标签）时，该组跳过不返回（§8.8 的
//   「若组内第 1 行本身就是空白/占位则跳过」）。
[[nodiscard]] std::vector<LyricReferenceGroup> groupLyricReferenceLines(
    const std::vector<LyricLine>& lines);

}  // namespace seriona::scanner
