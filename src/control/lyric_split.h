#pragma once

#include "seriona/control/control_contracts.h"   // LyricSplitConvention

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 歌词「原文 / 译文」切分（内部模块，非 inc/ 公共契约）。
//
// 本模块是 Python 参考实现
//   tools/lyric_split_regression/lyric_split_tool.py
// 的逐行等价移植，服务 D18（整首推断文档级约定）/ D5（译文取最后一段）/
// D10（多段并列整行作原文）/ D20（切点永不落在配对括号内部）等决策。
//
// 硬约束（改动前必读）：
//   1. 纯函数、无 I/O、无状态、无随机。不要在这里读文件、写日志、加缓存。
//   2. **不得**使用 ICU 判定语言类。参考实现用的是一张手工窄码点表
//      （见 classOfCodePoint），而 ICU 原生脚本类会在全角拉丁、希腊/阿拉伯/
//      泰文、CJK 扩展 B+、拉丁扩展附加等区间给出**不同**答案 —— 任何 ICU
//      往返都只会把一次机械直译变成静默分歧源。
//   3. 内部文本统一 UTF-8 `std::string`；所有 cut / 位置 / position 一律用
//      **字节偏移**。`classOfCodePoint` 收码点，其余收字节，交界处显式换算。
//   4. `.strip()`/`.rstrip()`/`.lstrip()`/空白判定必须实现 Python `str` 的
//      Unicode 空白语义，禁止裸 `std::isspace`（ASCII 局部语义）。注意
//      U+FEFF 不是 Python 空白（实测 `'\ufeff'.isspace() is False`）。

namespace seriona::control {

// 语言类（对应 Python `class_of_cp` 的返回值）。
// `Unknown` 对应 Python 的 `None` = 中性（空白/标点/数字/未收录），会黏附到相邻强段。
enum class LyricLanguage { Unknown, Han, Japanese, Korean, Latin, Cyrillic };

// ── 模块入口（公开面，签名与计划逐字一致）────────────────────────────────────

// 【唯一入口】三个入参缺一不可：convention 是切分结果的【决定性输入】，不能像早期
// 草案那样写成 splitLyricLine(line, target) —— 那会与参考实现分叉。与 Python
// `split_line(text, convention, target)` 一一对应。
struct LyricSplitResult {
  std::string original;
  std::string translation;
  LyricSplitConvention effectiveConvention{LyricSplitConvention::None};  // 对应 SplitResult.convention
  // "high" | "medium" | "none" —— 与 Python 同【字符串记号】：闸门要求两端 dump
  // 逐字节一致，字符串记号让映射层不存在，消除整类「值一样但编码不同」的假差异。
  std::string confidence;
  // 切分成功时 = route（strong / strong-fallback / bracket / weak / …）；
  // 未切分时 = empty | qq-sentinel | no-convention | validate-failed。
  std::string reason;
};
[[nodiscard]] LyricSplitResult splitLyricLine(std::string_view line,
                                              LyricSplitConvention convention,
                                              std::string_view targetLanguage);

// 文档级约定推断（D18）。输入是【已 cleanLine 的】行序列。
struct LyricSplitConventionVotes {
  LyricSplitConvention convention{LyricSplitConvention::None};
  std::vector<std::pair<LyricSplitConvention, int>> votes;  // 全量票数，供复核清单与调试
};
[[nodiscard]] LyricSplitConventionVotes inferSplitConvention(
    const std::vector<std::string>& cleanedLines, std::string_view targetLanguage);

// 整首切分结果（与 Python `DocumentSplit` 一一对应）。`results` 保留「清洗后的行 ↔
// 切分结果」的配对，供复核清单按行定位；`votes` 是全量票数（首次得票顺序，见
// inferSplitConvention）。输入 `rawLines` 是【未清洗】的原始行（含时间戳/制作人员行）。
struct LyricDocumentSplit {
  std::string path;
  LyricSplitConvention convention{LyricSplitConvention::None};
  std::vector<std::pair<LyricSplitConvention, int>> votes;
  std::vector<std::pair<std::string, LyricSplitResult>> results;
};
[[nodiscard]] LyricDocumentSplit splitDocument(const std::vector<std::string>& rawLines,
                                               std::string_view path,
                                               std::string_view targetLanguage);

// 算法版本：随【算法语义】变更手动递增（改了判据、加了 route、改了常量）。
// 仅用于让陈旧 auto 行失效，与 scanner schema 版本无关。
inline constexpr std::string_view kLyricSplitAlgoVersion = "1";

// ── 内部辅助（本模块白盒测试与 todo 5–8 使用；不是对外契约）──────────────────
// 之所以在此声明：测试目标链接 seriona_control 静态库，需要可见的声明才能断言。

// 码点 -> 语言类。逐区间直译参考实现的手工窄码点表，**全程不用 ICU**：
//   平假名 3040-309F / 片假名 30A0-30FF、31F0-31FF / 半角片假名 FF66-FF9D  => Japanese
//   汉字   4E00-9FFF、3400-4DBF、F900-FAFF、3005-3007                        => Han
//   谚文   AC00-D7AF、1100-11FF                                             => Korean
//   拉丁   41-5A、61-7A、C0-24F                                             => Latin
//   西里尔 400-4FF                                                          => Cyrillic
//   其余一律 Unknown。
// 反例锁定（这些区间 ICU 会给不同答案，故禁用 ICU）：全角拉丁 FF21-FF3A、
// 希腊 370-3FF、阿拉伯 600-6FF、泰文 E00-E7F、CJK 扩展 B+ 20000+、
// 拉丁扩展附加 1E00-1EFF —— 参考实现在它们上全部返回 None。
[[nodiscard]] LyricLanguage classOfCodePoint(std::uint32_t codePoint) noexcept;

// 语言类计数（等效 Python `profile()` 的 Counter，只统计非 Unknown 的字符）。
inline constexpr std::size_t kLyricLanguageClassCount = 6;
using LyricLanguageProfile = std::array<std::uint32_t, kLyricLanguageClassCount>;
[[nodiscard]] constexpr std::size_t lyricLanguageIndex(LyricLanguage language) noexcept {
  return static_cast<std::size_t>(language);
}
[[nodiscard]] LyricLanguageProfile profileOf(std::string_view text) noexcept;

// 清洗单行（等效 Python `clean_line`）。返回 std::nullopt 表示该行不是歌词正文。
// **顺序不可换**：先剥时间戳 → 再去行内 `[tag:value]` → 最后滤制作人员（P1 陷阱：
// 否则 `[00:00.000][by:x]` 会漏过过滤）。
[[nodiscard]] std::optional<std::string> cleanLine(std::string_view raw);

// ── 内部辅助（todo 5 起：候选枚举 / 括号区 / 边缘剥离 / 两套验证原语）─────────
// 与 classOfCodePoint / profileOf / cleanLine 同区，供本模块白盒测试与 todo 6–8 使用。
// **位置与 cut 的入参与返回值一律是 UTF-8 字节偏移**（参考实现是码点下标；换算在
// .cpp 的 decodeUtf8At 里累计），混用码点下标是本模块最容易踩的分叉源。

// 候选切点枚举（等效 Python `candidate_positions`）。
// kind ∈ STRONG_SEPARATORS ∪ WEAK_BOUNDARIES 时，返回每个匹配**之后**的字节偏移
// （切点在匹配位置【之前】，即 [0,pos) 是原文）；kind == "SCRIPT" 时返回「非目标
// 决定性语言类 → 汉字」的每一次过渡位置；其它 kind 返回空。
[[nodiscard]] std::vector<std::size_t> candidatePositions(std::string_view text,
                                                          std::string_view kind);

// 最外层配对括号区的起始字节偏移，**从右到左**排列；遇错配闭括号忽略之（不错位）。
[[nodiscard]] std::vector<std::size_t> bracketRegions(std::string_view text);

// 以 start 处开括号起始的最外层括号区的**结束位置（不含）**；无配对返回 nullopt。
[[nodiscard]] std::optional<std::size_t> bracketSpan(std::string_view text, std::size_t start);

// 切点是否落在最外层配对括号区的**内部** —— 即切开了配对括号（违反 D20）。
// 独立于「人工 ` / ` 标注」的客观判据，供护栏与守卫共用。
[[nodiscard]] bool cutInsideBracket(std::string_view text, std::size_t cut);

// 在切点处剥离分隔符与空白，返回 (左, 右) 两个 [0,cut) / [cut,len) 的子串视图。
// kind 给出且非空时**只剥它本身**（不可剥「所有分隔符」，否则 `＼なので～す／ / ＼的~说／`
// 里作为内容的 `／` 会被误删）；为空（nullopt 或空串）时改剥 SEPARATOR_TRAIL_CHARS。
// 分隔符与空白**交替**剥离直到稳定：`原文 /  / 译文` 折叠后的切点若一次性剥离会停在
// `原文 /`（残留一根分隔符）。
[[nodiscard]] std::pair<std::string_view, std::string_view> trimEdges(
    std::string_view text, std::size_t cut, std::optional<std::string_view> kind = std::nullopt);

// 双向验证（等效 Python `validate_cut`）：右侧须含目标语言类且**不含** JA/KO；
// 左侧须含至少一个 NON_TARGET_DECISIVE。target 不在 TARGET_CLASS 表里 ⇒ nullopt。
[[nodiscard]] std::optional<std::pair<std::string, std::string>> validateCut(
    std::string_view text, std::size_t cut, std::string_view target);

// validateCut 的严格版本（等效 Python `validate_cut_pure`）：追加「右侧第一个**可分类**
// 字符必须是目标语言类」（跳过引号/括号等无类字符）与「右侧不得含 MIXED_SCRIPT_BLOCKERS
// （JA/KO/CYRL；**拉丁刻意不在内**，译文夹英文/数字极常见）」；guarded 时再拒绝落在配对
// 括号内部的切点。**guarded 默认 true** —— 执行期必须默认开启守卫。
[[nodiscard]] std::optional<std::pair<std::string, std::string>> validateCutPure(
    std::string_view text, std::size_t cut, std::string_view target, bool guarded = true);

// ── 内部辅助（todo 6 起：三条切分路径 + 结果记入）─────────────────────────────
// 三条路径的返回值统一为 (原文, 译文) 的**拷贝**，与 todo 5 的 validateCut 一致；
// 不返回 string_view（视图会悬垂到临时串上）。位置一律 UTF-8 字节偏移。
using LyricCut = std::pair<std::string, std::string>;

// 把切点序列合并成「分隔符段」（等效 Python `_separator_runs`）：相邻两切点之间若
// 只剩本分隔符与空白即属同一段；段 `end` 会吃掉后续 Python 空白。`positions` 为空 ⇒
// 返回空。**必须合并而非放宽 kMaxSeparatorOccurrences**：`A / B / C` 每段有内容时仍
// 须各自成段（多段并列），只有空段才并段。
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> separatorRuns(
    std::string_view text, const std::vector<std::size_t>& positions, std::string_view kind);

// 显式分隔符切分（等效 Python `_cut_explicit`，D5「取最右有效切点」）。guarded=true 时
// **先剔除落在括号内部的候选，再计段数**（段计数的是可用分隔）；`runs.size() >=
// kMaxSeparatorOccurrences` ⇒ nullopt。译文侧再剥一层 SEPARATOR_TRAIL_CHARS 残留
// （原文侧不剥：`／` 可能是内容）；两侧皆非空才接受，两侧相同也照切。
[[nodiscard]] std::optional<LyricCut> cutExplicit(std::string_view text, std::string_view kind,
                                                  bool guarded = true);

// 脚本过渡切分（等效 Python `_cut_script`）：**最后一次**「非目标决定性类 → 汉字」
// 过渡（guarded 时跳过括号内部的过渡点），切完 trimEdges；右侧不得含任何
// kNonTargetDecisive（含拉丁）。**刻意没有「切不出回退到别的路径」的兜底**。
[[nodiscard]] std::optional<LyricCut> cutScript(std::string_view text, std::string_view target,
                                                bool guarded = true);

// 括号区内容是否【就是译文】（等效 Python `_is_target_bracket`）：括号区 profile 含
// 目标语言类且**不含** JA/KO/CYRL（**不用占比判据**）。start 不是开括号或无配对 ⇒ false。
[[nodiscard]] bool isTargetBracket(std::string_view text, std::size_t start,
                                   std::string_view target);

// 把括号区起点当切点（等效 Python `_cut_bracket`）：仅当 isTargetBracket 通过才切，
// 切完 trimEdges，两侧皆非空才返回。
[[nodiscard]] std::optional<LyricCut> cutBracket(std::string_view text, std::size_t start,
                                                 std::string_view target);

// 按最外层括号区（bracketRegions 已是右→左）逐个尝试（等效 Python `_find_any_bracket`）；
// trailingOnly 时要求括号区之后到行尾经 Python strip 后为空，返回第一个成功的 cutBracket。
[[nodiscard]] std::optional<LyricCut> findAnyBracket(std::string_view text,
                                                     std::string_view target,
                                                     bool trailingOnly = false);

// 把切分结果记入 LyricSplitResult（等效 Python `_accept`）：confidence = (route == "weak")
// ? "medium" : "high"；route **原样**记入 reason，convention 原样记入 effectiveConvention。
[[nodiscard]] LyricSplitResult accept(std::string_view left, std::string_view right,
                                      LyricSplitConvention convention, std::string_view route);

// ── 内部辅助（todo 7 起：歌曲级约定推断）─────────────────────────────────────

// 该行是否支持把 kind 视为本文件的切分约定（等效 Python `_accepts`）。必须与 splitLyricLine
// 的受理条件一致，否则会出现「票数来自一种判据、执行却用另一种」的漂移。
// **唯一例外是 D20 括号守卫**：`cutExplicit` 一律以 guarded=false 计票 —— 计票问的是
// 「作者用了哪种分隔符**写法**」，不是「本行切在**哪里**」。作者把分隔符写进注音括号
// （`（くも / ）`）时该行**依然**是本文件用 `/` 的证据；若把守卫放进计票，这类文件的约定
// 会整体推断失败、整首译文丢失。守卫只在执行期生效。
// 分派（逐字照 Python）：kind ∈ STRONG_SEPARATORS ⇒ cutExplicit(guarded=false)；
// kind ∈ WEAK_BOUNDARIES ⇒ candidatePositions 上任一 validateCutPure(guarded=false) 通过；
// 否则（"SCRIPT"）⇒ cutScript(guarded=false)。
[[nodiscard]] bool acceptsVote(std::string_view line, std::string_view kind,
                               std::string_view target);

}
