#include "lyric_split.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 歌词「原文 / 译文」切分的 C++ 实现。
//
// 唯一的语义事实来源是 Python 参考实现：
//   tools/lyric_split_regression/lyric_split_tool.py
// 本文件按「逐区间 / 逐常量直译」的方式移植，不重新设计算法。
//
// 本文件已完成：类型、语言类窄表、清洗、规范记号映射、构建接入（todo 4）、
// 候选枚举 / 括号区 / 边缘剥离 / 两套验证原语（todo 5）、三条切分路径（todo 6），
// 以及歌曲级约定推断 acceptsVote / inferSplitConvention（todo 7）。
// 仍待落地：切分入口 splitLyricLine 的完整 route 编排（todo 8）；本文件里
// `splitLyricLine` 仍是刻意留白的最小实现（见其函数体注释），只保证可编译、不崩。

namespace seriona::control {
namespace {

// ── UTF-8 基础（文本模型：内部统一 UTF-8 std::string，位置一律字节偏移）─────────
// 参考实现的 `str` 是码点序列，而 C++ 的 `std::string` 是字节序列。凡「按字符」
// 判定/计数的步骤（class_of_cp / profile / 空白剥离 / \d 与 \s）都必须先解码成
// 码点；凡 cut / position 一律用字节偏移，交界处显式换算。**不得**混用码点下标。

struct DecodedCodePoint {
  std::uint32_t codePoint;
  std::size_t byteLength;
};

// 无效 UTF-8 字节退化为 U+FFFD 且只前进 1 字节，保证不越界、不死循环。
// 「非法」包含三类：格式不合法（截断 / 非续字节 / 过长引导字节），以及格式合法但
// 语义非法 —— overlong、UTF-16 代理区、超出 U+10FFFF（见 decodeUtf8At 尾部的判定）。
// 参考实现读到的是已由 codec 解码的合法文本，正常情况下不会走到这里；但本模块的
// 测试与「以字节偏移为准」的换算都依赖 decodeUtf8At 的 byteLength，故必须兑现注释。
constexpr std::uint32_t kReplacementCodePoint = 0xFFFDU;

[[nodiscard]] DecodedCodePoint decodeUtf8At(std::string_view text, std::size_t offset) noexcept {
  const auto lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80U) {
    return {lead, 1};
  }
  std::size_t length = 0;
  std::uint32_t codePoint = 0;
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    codePoint = static_cast<std::uint32_t>(lead & 0x1FU);
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    codePoint = static_cast<std::uint32_t>(lead & 0x0FU);
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    codePoint = static_cast<std::uint32_t>(lead & 0x07U);
  } else {
    return {kReplacementCodePoint, 1};
  }
  if (offset + length > text.size()) {
    return {kReplacementCodePoint, 1};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return {kReplacementCodePoint, 1};
    }
    codePoint = (codePoint << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
  }
  // 格式合法但语义非法的三类序列，一律退化为 U+FFFD 且只前进 1 字节：
  //   1) overlong：`C0 80`(U+0000)、`C1 81`(U+0041)、`E0 80 80`、`F0 80 80 80` …
  //   2) UTF-16 代理区：`ED A0 80` … `ED BF BF`（U+D800..U+DFFF）
  //   3) 超出 Unicode 上界：`F4 90 80 80` … （U+110000+）
  // 必须拒绝：这些序列若被接受会解出【可分类或可当空白】的码点（`C1 81` → U+0041
  // 拉丁、`E0 80 A0` → U+0020 空白），与「已由 codec 解码」的参考实现静默分叉。
  const bool overlong = (length == 2 && codePoint < 0x80U) ||
                        (length == 3 && codePoint < 0x800U) ||
                        (length == 4 && codePoint < 0x10000U);
  const bool surrogate = codePoint >= 0xD800U && codePoint <= 0xDFFFU;
  if (overlong || surrogate || codePoint > 0x10FFFFU) {
    return {kReplacementCodePoint, 1};
  }
  return {codePoint, length};
}

// ── 码点区间表（按起点有序、互不重叠 ⇒ 二分查表 ≡ 参考实现的 if 链）────────────

struct CodePointSpan {
  std::uint32_t first;
  std::uint32_t last;
};

template <std::size_t N>
[[nodiscard]] constexpr bool inSortedSpans(const std::array<CodePointSpan, N>& spans,
                                           std::uint32_t codePoint) noexcept {
  const auto upper = std::upper_bound(spans.begin(), spans.end(), codePoint,
                                      [](std::uint32_t value, const CodePointSpan& span) {
                                        return value < span.first;
                                      });
  if (upper == spans.begin()) {
    return false;
  }
  return codePoint <= (upper - 1)->last;
}

template <std::size_t N>
[[nodiscard]] constexpr bool spansSortedAndDisjoint(const std::array<CodePointSpan, N>& spans) noexcept {
  for (std::size_t index = 1; index < N; ++index) {
    if (spans[index].first <= spans[index - 1].last) {
      return false;
    }
  }
  return true;
}

// Python `str.isspace()` / 正则 `\s`（str 模式）的精确集合：29 个码点、10 段。
// 实测 `set(re.match(r'\s', chr(cp)) for cp) == set(chr(cp).isspace())` 为真。
// **U+FEFF 不在其中**（实测 `'\ufeff'.isspace() is False`），把它当空白会与参考实现分叉。
constexpr std::array<CodePointSpan, 10> kPythonWhitespaceRanges{{
    {0x0009U, 0x000DU},
    {0x001CU, 0x0020U},
    {0x0085U, 0x0085U},
    {0x00A0U, 0x00A0U},
    {0x1680U, 0x1680U},
    {0x2000U, 0x200AU},
    {0x2028U, 0x2029U},
    {0x202FU, 0x202FU},
    {0x205FU, 0x205FU},
    {0x3000U, 0x3000U},
}};

// 正则 `\d`（str 模式）= Unicode 十进制数字（类别 Nd），不是 [0-9]：
// 实测 `'\u0660'.isdigit()` 为真、`TIMESTAMP_RE.sub('', '[٠١:٢٣]x') == 'x'`。
// 若用 ASCII 判据，非拉丁数字时间戳会被漏剥而与参考实现分叉。
constexpr std::array<CodePointSpan, 71> kPythonDecimalDigitRanges{{
    {0x0030U, 0x0039U},
    {0x0660U, 0x0669U},
    {0x06F0U, 0x06F9U},
    {0x07C0U, 0x07C9U},
    {0x0966U, 0x096FU},
    {0x09E6U, 0x09EFU},
    {0x0A66U, 0x0A6FU},
    {0x0AE6U, 0x0AEFU},
    {0x0B66U, 0x0B6FU},
    {0x0BE6U, 0x0BEFU},
    {0x0C66U, 0x0C6FU},
    {0x0CE6U, 0x0CEFU},
    {0x0D66U, 0x0D6FU},
    {0x0DE6U, 0x0DEFU},
    {0x0E50U, 0x0E59U},
    {0x0ED0U, 0x0ED9U},
    {0x0F20U, 0x0F29U},
    {0x1040U, 0x1049U},
    {0x1090U, 0x1099U},
    {0x17E0U, 0x17E9U},
    {0x1810U, 0x1819U},
    {0x1946U, 0x194FU},
    {0x19D0U, 0x19D9U},
    {0x1A80U, 0x1A89U},
    {0x1A90U, 0x1A99U},
    {0x1B50U, 0x1B59U},
    {0x1BB0U, 0x1BB9U},
    {0x1C40U, 0x1C49U},
    {0x1C50U, 0x1C59U},
    {0xA620U, 0xA629U},
    {0xA8D0U, 0xA8D9U},
    {0xA900U, 0xA909U},
    {0xA9D0U, 0xA9D9U},
    {0xA9F0U, 0xA9F9U},
    {0xAA50U, 0xAA59U},
    {0xABF0U, 0xABF9U},
    {0xFF10U, 0xFF19U},
    {0x104A0U, 0x104A9U},
    {0x10D30U, 0x10D39U},
    {0x10D40U, 0x10D49U},
    {0x11066U, 0x1106FU},
    {0x110F0U, 0x110F9U},
    {0x11136U, 0x1113FU},
    {0x111D0U, 0x111D9U},
    {0x112F0U, 0x112F9U},
    {0x11450U, 0x11459U},
    {0x114D0U, 0x114D9U},
    {0x11650U, 0x11659U},
    {0x116C0U, 0x116C9U},
    {0x116D0U, 0x116E3U},
    {0x11730U, 0x11739U},
    {0x118E0U, 0x118E9U},
    {0x11950U, 0x11959U},
    {0x11BF0U, 0x11BF9U},
    {0x11C50U, 0x11C59U},
    {0x11D50U, 0x11D59U},
    {0x11DA0U, 0x11DA9U},
    {0x11F50U, 0x11F59U},
    {0x16130U, 0x16139U},
    {0x16A60U, 0x16A69U},
    {0x16AC0U, 0x16AC9U},
    {0x16B50U, 0x16B59U},
    {0x16D70U, 0x16D79U},
    {0x1CCF0U, 0x1CCF9U},
    {0x1D7CEU, 0x1D7FFU},
    {0x1E140U, 0x1E149U},
    {0x1E2F0U, 0x1E2F9U},
    {0x1E4F0U, 0x1E4F9U},
    {0x1E5F1U, 0x1E5FAU},
    {0x1E950U, 0x1E959U},
    {0x1FBF0U, 0x1FBF9U},
}};

static_assert(spansSortedAndDisjoint(kPythonWhitespaceRanges),
              "Python 空白表必须按起点有序且互不重叠，否则二分查表与参考实现语义不等价");
static_assert(spansSortedAndDisjoint(kPythonDecimalDigitRanges),
              "Python Nd 表必须按起点有序且互不重叠，否则二分查表与参考实现语义不等价");

[[nodiscard]] constexpr bool isPythonWhitespace(std::uint32_t codePoint) noexcept {
  return inSortedSpans(kPythonWhitespaceRanges, codePoint);
}

[[nodiscard]] constexpr bool isPythonDecimalDigit(std::uint32_t codePoint) noexcept {
  return inSortedSpans(kPythonDecimalDigitRanges, codePoint);
}

// ── 参考实现的语言类窄码点表（class_of_cp）────────────────────────────────────
// 逐区间直译参考实现，**全程不用 ICU**：ICU 原生脚本类会在全角拉丁 FF21-FF3A、
// 希腊/阿拉伯/泰文、CJK 扩展 B+、拉丁扩展附加 1E00-1EFF 上给出与参考实现不同的
// 答案（参考实现在这些区间一律返回 None）⇒ 任何 ICU 往返都只会把一次机械直译
// 变成静默分歧源。
//
// 区间已按起点有序且互不重叠（见下方 static_assert），故二分查表与参考实现的
// if 链语义严格等价。
struct LanguageRange {
  std::uint32_t first;
  std::uint32_t last;
  LyricLanguage language;
};

constexpr std::array<LanguageRange, 14> kLanguageRanges{{
    {0x0041U, 0x005AU, LyricLanguage::Latin},        // A-Z
    {0x0061U, 0x007AU, LyricLanguage::Latin},        // a-z
    {0x00C0U, 0x024FU, LyricLanguage::Latin},        // 拉丁扩展 A/B
    {0x0400U, 0x04FFU, LyricLanguage::Cyrillic},     // 西里尔
    {0x1100U, 0x11FFU, LyricLanguage::Korean},       // 谚文字母
    {0x3005U, 0x3007U, LyricLanguage::Han},          // 々〆〇
    {0x3040U, 0x309FU, LyricLanguage::Japanese},     // 平假名
    {0x30A0U, 0x30FFU, LyricLanguage::Japanese},     // 片假名
    {0x31F0U, 0x31FFU, LyricLanguage::Japanese},     // 片假名音标扩展
    {0x3400U, 0x4DBFU, LyricLanguage::Han},          // CJK 扩展 A
    {0x4E00U, 0x9FFFU, LyricLanguage::Han},          // CJK 统一表意
    {0xAC00U, 0xD7AFU, LyricLanguage::Korean},       // 谚文音节
    {0xF900U, 0xFAFFU, LyricLanguage::Han},          // 兼容汉字
    {0xFF66U, 0xFF9DU, LyricLanguage::Japanese},     // 半角片假名
}};

[[nodiscard]] constexpr bool languageRangesSortedAndDisjoint() noexcept {
  for (std::size_t index = 1; index < kLanguageRanges.size(); ++index) {
    if (kLanguageRanges[index].first <= kLanguageRanges[index - 1].last) {
      return false;
    }
  }
  return true;
}

static_assert(languageRangesSortedAndDisjoint(),
              "窄码点表必须按起点有序且互不重叠，否则二分查表与参考实现的 if 链语义不等价");

// ── 常量（逐字照抄参考实现 `lyric_split_tool.py:34-79`）────────────────────────
// 这些常量在本 todo（骨架）里尚未被切分逻辑使用，但由下方 static_assert 引用并
// 锁定其关键形状；todo 5–8 会直接取用。

// 强分隔符：歌词作者显式书写意图。含半角 `/` —— 它在相当数量的歌词里被直接当
// 分隔符使用（`惚れた腫れたの馬鹿騒ぎ/愛意被潑冷水的超蠢騷動`），优先级低于带空格
// 的 ` / `，由歌曲级约定决定该文件用的是哪一种。
// 末尾的反斜杠同理，且**必须留在末位**：默认没有歌词用它（仅 1 个文件 31 行实测），
// 但它会出现在正文里（`(Kill you\)`）。置于末位 ⇒ 只在前面的约定都不成立时才有
// 机会得票，故正文里的反斜杠不会抢走 ` / ` 的票（见 infer_convention 的「首个匹配即得票」）。
constexpr std::array<std::string_view, 6> kStrongSeparators{" / ", "｜", "|", "／", "/", "\\"};

// 弱边界：空白类。多为排版间距（尤其全角空格），须由「歌曲级约定」背书。
// 刻意【不含双空格】：双空格边界在任何情况下也都是单空格边界，而单空格配合
// 「最左 + 右侧须纯目标语言」能落到正确的括注边界；把双空格列入反而会在排版文本
// （如塔语 `PARADIGM SHIFT  ━━ 「…」`）上抢先匹配到作者的对齐空格。
constexpr std::array<std::string_view, 3> kWeakBoundaries{"\t", "　", " "};

// 一行出现这么多次显式分隔符，说明是多段并列（`原文A / 原文B / 译文`）而非
// 「原文 / 译文」，结构已经歧义；按 D10 放弃切分，不赌一个。
constexpr std::size_t kMaxSeparatorOccurrences = 2;

// SCRIPT 与括号路径的切点处可能残留的分隔类字符，须从两侧剥离。
// 刻意**不含反斜杠**：作者会把转义写进译文（` / \t人生中 大家都愚蠢地可爱` 里的
// `\t` 是制表符的转义写法），一概剥离会把它毁成 `t人生中…`（实测 131 行）。
constexpr std::array<std::string_view, 4> kSeparatorTrailChars{"/", "／", "|", "｜"};

// 弱约定文件里逐行额外尝试的唯一显式分隔符（见 split_line）。
constexpr std::string_view kSpacedSeparator{" / "};

// 候选类型（用于歌曲级约定推断，顺序即优先级）= 强分隔符 + 弱边界 + "SCRIPT"。
constexpr std::array<std::string_view, 10> kConventions{" / ", "｜", "|", "／", "/", "\\",
                                                        "\t", "　", " ", "SCRIPT"};

// 票键（原始 kind 串）-> LyricSplitConvention，**按 kConventions 顺序**一一对应。
// 注意：这里的 kind 是原始分隔符串（`" / "`），**不是** conventionToken 的规范记号
// （conventionToken(StrongSlashSpaced) 返回 `"S: / "`）—— 两者是不同映射，不可混用。
// 它同时驱动「逐行投票」与「按序扫描门槛」两步。
constexpr std::array<std::pair<LyricSplitConvention, std::string_view>, 10> kConventionKinds{{
    {LyricSplitConvention::StrongSlashSpaced, " / "},
    {LyricSplitConvention::StrongFullwidthBar, "｜"},
    {LyricSplitConvention::StrongBar, "|"},
    {LyricSplitConvention::StrongFullwidthSlash, "／"},
    {LyricSplitConvention::StrongSlash, "/"},
    {LyricSplitConvention::StrongBackslash, "\\"},
    {LyricSplitConvention::WeakTab, "\t"},
    {LyricSplitConvention::WeakFullwidthSpace, "　"},
    {LyricSplitConvention::WeakSpace, " "},
    {LyricSplitConvention::ScriptTransition, "SCRIPT"},
}};

// 目标语言 -> 其语言类。默认中文。
constexpr std::array<std::pair<std::string_view, LyricLanguage>, 4> kTargetClass{{
    {"zh", LyricLanguage::Han},
    {"ja", LyricLanguage::Japanese},
    {"ko", LyricLanguage::Korean},
    {"en", LyricLanguage::Latin},
}};

// 能证明「这是另一种语言」的决定性语言类（汉字不是决定性特征，中日共享）。
constexpr std::array<LyricLanguage, 4> kNonTargetDecisive{
    LyricLanguage::Japanese, LyricLanguage::Latin, LyricLanguage::Korean, LyricLanguage::Cyrillic};

// 弱边界右侧出现这些脚本即判为「仍是原文」：三者与汉字同处 CJK 视觉区，混排时
// 无法靠肉眼区分原文/译文边界，比拉丁字母更危险。拉丁类不算阻塞（译文常夹英文）。
constexpr std::array<LyricLanguage, 3> kMixedScriptBlockers{LyricLanguage::Japanese,
                                                            LyricLanguage::Korean,
                                                            LyricLanguage::Cyrillic};

// 括号开/闭集合（逐字符照抄参考实现的 `set("（(「【〔〈《『［[{＜<")` 等）。
constexpr std::array<std::string_view, 13> kOpeners{
    "（", "(", "「", "【", "〔", "〈", "《", "『", "［", "[", "{", "＜", "<"};
constexpr std::array<std::string_view, 13> kClosers{
    "）", ")", "」", "】", "〕", "〉", "》", "』", "］", "]", "}", "＞", ">"};

// 制作人员行的关键词（逐字照抄 CREDIT_RE 的 alternation 顺序 —— 顺序即 Python
// 正则的匹配顺序，不可重排）。
constexpr std::array<std::string_view, 31> kCreditAlternatives{
    "作词",  "作曲", "编曲", "歌",    "唱",   "原唱",    "原曲",  "発売日",
    "収録",  "ミックス", "歌詞", "翻译",   "訳",   "词",      "曲",    "演唱",
    "制作",  "混音", "录音", "母带",   "By",   "BY",      "by",    "Arr",
    "Arrange", "Lyrics", "Vocals", "Vocal", "Music", "Mix", "Master"};

// 关键形状的编译期锁定（同时让这些常量在骨架期就被引用）。
static_assert(kStrongSeparators.back() == "\\",
              "STRONG_SEPARATORS 的反斜杠必须在末位（见常量注释：正文里的反斜杠不得抢走 ` / ` 的票）");
static_assert(kStrongSeparators.size() == 6 && kWeakBoundaries.size() == 3);
static_assert(kConventions.size() == kStrongSeparators.size() + kWeakBoundaries.size() + 1);
static_assert(kConventions.back() == "SCRIPT");
static_assert(kSpacedSeparator == kStrongSeparators.front());
static_assert(kMaxSeparatorOccurrences == 2);
static_assert(kSeparatorTrailChars.size() == 4);
static_assert(kTargetClass.size() == 4 && kNonTargetDecisive.size() == 4);
static_assert(kMixedScriptBlockers.size() == 3);
static_assert(kOpeners.size() == 13 && kClosers.size() == 13);
static_assert(kCreditAlternatives.size() == 31);

// kConventionKinds 必须与 kConventions 逐位同序同串（票键表与候选顺序表不得漂移）。
[[nodiscard]] constexpr bool conventionKindsMirrorConventions() noexcept {
  for (std::size_t index = 0; index < kConventions.size(); ++index) {
    if (kConventionKinds[index].second != kConventions[index]) {
      return false;
    }
  }
  return true;
}

static_assert(kConventionKinds.size() == kConventions.size());
static_assert(conventionKindsMirrorConventions(),
              "kConventionKinds 的顺序/取值必须与 kConventions 一致（顺序即优先级）");
static_assert(kConventionKinds.front().first == LyricSplitConvention::StrongSlashSpaced);
static_assert(kConventionKinds.back().first == LyricSplitConvention::ScriptTransition);

[[nodiscard]] constexpr bool separatorTrailHasNoBackslash() noexcept {
  for (const auto value : kSeparatorTrailChars) {
    if (value == "\\") {
      return false;
    }
  }
  return true;
}

static_assert(separatorTrailHasNoBackslash(),
              "SEPARATOR_TRAIL_CHARS 刻意不含反斜杠（会毁掉译文里的转义写法 \\t，实测 131 行）");

// ── Python `str` 空白语义的 strip 家族 ───────────────────────────────────────

[[nodiscard]] std::size_t skipPythonWhitespace(std::string_view text, std::size_t offset) noexcept {
  std::size_t cursor = offset;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!isPythonWhitespace(decoded.codePoint)) {
      break;
    }
    cursor += decoded.byteLength;
  }
  return cursor;
}

[[nodiscard]] std::string_view lstripPython(std::string_view text) noexcept {
  return text.substr(skipPythonWhitespace(text, 0));
}

[[nodiscard]] std::string_view rstripPython(std::string_view text) noexcept {
  std::size_t contentEnd = 0;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!isPythonWhitespace(decoded.codePoint)) {
      contentEnd = cursor + decoded.byteLength;
    }
    cursor += decoded.byteLength;
  }
  return text.substr(0, contentEnd);
}

[[nodiscard]] std::string_view stripPython(std::string_view text) noexcept {
  return rstripPython(lstripPython(text));
}

// ── todo 6：Python 字符集 strip（`str.strip(chars)` / `lstrip(chars)`）────────
// Python 的 `str.strip(chars)` 里 chars 是**码点集合**（不是子串）：会剥掉任意成员。
// `_separator_runs` 的 `text[...].strip(kind)`、`_cut_explicit` 的
// `right.lstrip("".join(SEPARATOR_TRAIL_CHARS))` 都依赖这一语义 —— 用 `starts_with`
// 逐子串比会漏掉「单个成员」的情形（如 kind=" / " 剥掉裸空格与裸斜杠）。
[[nodiscard]] bool codePointInCharSet(std::uint32_t codePoint, std::string_view chars) noexcept {
  std::size_t cursor = 0;
  while (cursor < chars.size()) {
    const auto decoded = decodeUtf8At(chars, cursor);
    if (decoded.codePoint == codePoint) {
      return true;
    }
    cursor += decoded.byteLength;
  }
  return false;
}

[[nodiscard]] bool codePointInSeparatorTrailChars(std::uint32_t codePoint) noexcept {
  for (const auto token : kSeparatorTrailChars) {
    // 集合成员均为单码点，故逐成员比较码点（token 一定非空）。
    if (decodeUtf8At(token, 0).codePoint == codePoint) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string_view lstripPythonChars(std::string_view text,
                                                 std::string_view chars) noexcept {
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!codePointInCharSet(decoded.codePoint, chars)) {
      break;
    }
    cursor += decoded.byteLength;
  }
  return text.substr(cursor);
}

// 等价 Python `text.lstrip("".join(SEPARATOR_TRAIL_CHARS))`：按【码点集合】剥离
// SEPARATOR_TRAIL_CHARS 的任意成员，不是子串语义。
[[nodiscard]] std::string_view lstripSeparatorTrailChars(std::string_view text) noexcept {
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!codePointInSeparatorTrailChars(decoded.codePoint)) {
      break;
    }
    cursor += decoded.byteLength;
  }
  return text.substr(cursor);
}

[[nodiscard]] std::string_view rstripPythonChars(std::string_view text,
                                                 std::string_view chars) noexcept {
  std::size_t contentEnd = 0;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!codePointInCharSet(decoded.codePoint, chars)) {
      contentEnd = cursor + decoded.byteLength;
    }
    cursor += decoded.byteLength;
  }
  return text.substr(0, contentEnd);
}

[[nodiscard]] std::string_view stripPythonChars(std::string_view text,
                                                std::string_view chars) noexcept {
  return rstripPythonChars(lstripPythonChars(text, chars), chars);
}

// ── 四个正则的等价实现 ───────────────────────────────────────────────────────
// 参考实现（不可照抄 std::regex：`\d`/`\s` 的 Unicode 语义与 Python 不同）：
//   TIMESTAMP_RE = ^(?:\[\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?\])+
//   INLINE_TAG_RE = \[[a-zA-Z_]+:[^\]]*\]        （无 IGNORECASE）
//   METADATA_RE = ^\s*\[[a-zA-Z_]+:              （有 IGNORECASE ⇒ 见 isMetadataWordCodePoint）
//   CREDIT_RE = ^\s*(<31 个关键词>)\s*[:：]

struct DecimalRun {
  std::size_t count;
  std::size_t byteLength;
};

[[nodiscard]] DecimalRun consumeDecimalRun(std::string_view text, std::size_t offset) noexcept {
  DecimalRun run{0, 0};
  std::size_t cursor = offset;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!isPythonDecimalDigit(decoded.codePoint)) {
      break;
    }
    ++run.count;
    cursor += decoded.byteLength;
  }
  run.byteLength = cursor - offset;
  return run;
}

// 匹配一个 `\[…\]` 时间戳组，返回其后字节偏移；不匹配返回 nullopt。
// 逐条复刻 `\d{1,3}` / `\d{1,2}` 的贪心+回溯结果（数字串超长即整体失败）。
[[nodiscard]] std::optional<std::size_t> matchTimestampGroup(std::string_view text,
                                                             std::size_t offset) noexcept {
  std::size_t cursor = offset + 1;  // 调用方已确认 text[offset] == '['
  const DecimalRun first = consumeDecimalRun(text, cursor);
  if (first.count < 1 || first.count > 3) {
    return std::nullopt;
  }
  cursor += first.byteLength;
  if (cursor >= text.size() || text[cursor] != ':') {
    return std::nullopt;
  }
  ++cursor;
  const DecimalRun second = consumeDecimalRun(text, cursor);
  if (second.count < 1 || second.count > 2) {
    return std::nullopt;
  }
  cursor += second.byteLength;
  if (cursor < text.size() && (text[cursor] == '.' || text[cursor] == ':')) {
    ++cursor;
    const DecimalRun fraction = consumeDecimalRun(text, cursor);
    if (fraction.count < 1 || fraction.count > 3) {
      return std::nullopt;
    }
    cursor += fraction.byteLength;
  }
  if (cursor < text.size() && text[cursor] == ']') {
    return cursor + 1;
  }
  return std::nullopt;
}

// 返回开头被 `TIMESTAMP_RE` 吃掉的字节数（0 = 未匹配）。
[[nodiscard]] std::size_t timestampPrefixLength(std::string_view text) noexcept {
  std::size_t cursor = 0;
  std::size_t groups = 0;
  while (cursor < text.size() && text[cursor] == '[') {
    const auto groupEnd = matchTimestampGroup(text, cursor);
    if (!groupEnd.has_value()) {
      break;
    }
    cursor = *groupEnd;
    ++groups;
  }
  return groups == 0 ? 0 : cursor;
}

[[nodiscard]] bool isAsciiWordByte(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

// Python `re.IGNORECASE` 对字符类 `[a-zA-Z_]` 的等价集合：ASCII 字母/下划线，外加
// Unicode 简单大小写折叠后落回该类内的 4 个码点（U+0130 İ / U+0131 ı / U+017F ſ /
// U+212A K）。穷举 0..0x10FFFF 实测确认集合封闭、无第 5 个成员：
//   re.compile(r'[a-zA-Z_]+', re.IGNORECASE) 额外匹配的码点恰为这 4 个。
// **只有 METADATA_RE 带 IGNORECASE**；INLINE_TAG_RE 不带，故 removeInlineTags 仍用
// isAsciiWordByte —— 把本谓词搬过去会引入新的分歧。
[[nodiscard]] bool isMetadataWordCodePoint(std::uint32_t codePoint) noexcept {
  if (codePoint < 0x80U) {
    return isAsciiWordByte(static_cast<char>(codePoint));
  }
  return codePoint == 0x0130U || codePoint == 0x0131U || codePoint == 0x017FU ||
         codePoint == 0x212AU;
}

// 全局删除全部 `\[[a-zA-Z_]+:[^\]]*\]`（非重叠、自左向右）。
[[nodiscard]] std::string removeInlineTags(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    if (text[cursor] == '[') {
      std::size_t wordEnd = cursor + 1;
      while (wordEnd < text.size() && isAsciiWordByte(text[wordEnd])) {
        ++wordEnd;
      }
      if (wordEnd > cursor + 1 && wordEnd < text.size() && text[wordEnd] == ':') {
        std::size_t closing = wordEnd + 1;
        while (closing < text.size() && text[closing] != ']') {
          ++closing;
        }
        if (closing < text.size()) {
          cursor = closing + 1;  // 整段是行内 tag，删除
          continue;
        }
      }
    }
    result.push_back(text[cursor]);
    ++cursor;
  }
  return result;
}

[[nodiscard]] bool matchMetadata(std::string_view text) noexcept {
  std::size_t cursor = skipPythonWhitespace(text, 0);
  if (cursor >= text.size() || text[cursor] != '[') {
    return false;
  }
  ++cursor;
  const std::size_t wordStart = cursor;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!isMetadataWordCodePoint(decoded.codePoint)) {
      break;
    }
    cursor += decoded.byteLength;
  }
  if (cursor == wordStart) {
    return false;
  }
  return cursor < text.size() && text[cursor] == ':';
}

[[nodiscard]] bool matchCredit(std::string_view text) noexcept {
  const std::size_t start = skipPythonWhitespace(text, 0);
  const std::string_view tail = text.substr(start);
  for (const auto alternative : kCreditAlternatives) {
    if (!tail.starts_with(alternative)) {
      continue;
    }
    // Python 的 alternation 是「左起首个能接上 `\s*[:：]` 的分支」；
    // 逐个试、失败就换下一个分支，正是这个语义。
    const std::size_t colon = skipPythonWhitespace(text, start + alternative.size());
    // 半角 `:` 或全角 `：`（U+FF1A）；全角冒号是多字节，故按 UTF-8 字节前缀比较。
    if (colon < text.size() &&
        (text[colon] == ':' || text.substr(colon).starts_with("："))) {
      return true;
    }
  }
  return false;
}

// ── todo 5 辅助：括号字符判定 / 目标类 / 决定性类谓词 ─────────────────────────

enum class BracketCharKind { None, Opener, Closer };

// 等价 Python 的 `ch in OPENERS` / `ch in CLOSERS`：所有开/闭括号都是单码点，
// 集合内无互为字节前缀的成员，故直接按 UTF-8 字节前缀比较即可。
[[nodiscard]] BracketCharKind bracketCharKindAt(std::string_view text, std::size_t offset) noexcept {
  const std::string_view tail = text.substr(offset);
  for (const auto opener : kOpeners) {
    if (tail.starts_with(opener)) {
      return BracketCharKind::Opener;
    }
  }
  for (const auto closer : kClosers) {
    if (tail.starts_with(closer)) {
      return BracketCharKind::Closer;
    }
  }
  return BracketCharKind::None;
}

// 等价 Python `TARGET_CLASS.get(target)`：不在表里返回 nullopt。
[[nodiscard]] std::optional<LyricLanguage> targetClassOf(std::string_view target) noexcept {
  for (const auto& entry : kTargetClass) {
    if (entry.first == target) {
      return entry.second;
    }
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool isNonTargetDecisive(LyricLanguage language) noexcept {
  return std::find(kNonTargetDecisive.begin(), kNonTargetDecisive.end(), language) !=
         kNonTargetDecisive.end();
}

[[nodiscard]] constexpr bool isMixedScriptBlocker(LyricLanguage language) noexcept {
  return std::find(kMixedScriptBlockers.begin(), kMixedScriptBlockers.end(), language) !=
         kMixedScriptBlockers.end();
}

// 等价 Python `kind in STRONG_SEPARATORS or kind in WEAK_BOUNDARIES`。
[[nodiscard]] bool isSeparatorKind(std::string_view kind) noexcept {
  for (const auto separator : kStrongSeparators) {
    if (separator == kind) {
      return true;
    }
  }
  for (const auto boundary : kWeakBoundaries) {
    if (boundary == kind) {
      return true;
    }
  }
  return false;
}

// 等价 Python `kind in STRONG_SEPARATORS` / `kind in WEAK_BOUNDARIES`（_accepts 的分派依据）。
[[nodiscard]] constexpr bool isStrongSeparatorKind(std::string_view kind) noexcept {
  for (const auto separator : kStrongSeparators) {
    if (separator == kind) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] constexpr bool isWeakBoundaryKind(std::string_view kind) noexcept {
  for (const auto boundary : kWeakBoundaries) {
    if (boundary == kind) {
      return true;
    }
  }
  return false;
}

// LyricSplitConvention -> 原始 kind 串（票键 / kConventions 元素），双向映射。
// 这是 splitLyricLine 从枚举取回「分支判据用的 kind 串」的唯一途径；**不得**改用
// conventionToken（那是 `S: / ` 规范记号，与 kind 串是两套映射，见 kConventionKinds 注释）。
[[nodiscard]] std::optional<std::string_view> kindOfConvention(
    LyricSplitConvention convention) noexcept {
  for (const auto& entry : kConventionKinds) {
    if (entry.first == convention) {
      return entry.second;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<LyricSplitConvention> conventionOfKind(
    std::string_view kind) noexcept {
  for (const auto& entry : kConventionKinds) {
    if (entry.second == kind) {
      return entry.first;
    }
  }
  return std::nullopt;
}

}  // namespace

// ── 公共实现 ────────────────────────────────────────────────────────────────

LyricLanguage classOfCodePoint(std::uint32_t codePoint) noexcept {
  const auto upper = std::upper_bound(kLanguageRanges.begin(), kLanguageRanges.end(), codePoint,
                                      [](std::uint32_t value, const LanguageRange& range) {
                                        return value < range.first;
                                      });
  if (upper == kLanguageRanges.begin()) {
    return LyricLanguage::Unknown;
  }
  const auto& candidate = *(upper - 1);
  if (codePoint <= candidate.last) {
    return candidate.language;
  }
  return LyricLanguage::Unknown;
}

LyricLanguageProfile profileOf(std::string_view text) noexcept {
  LyricLanguageProfile counts{};
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    const LyricLanguage language = classOfCodePoint(decoded.codePoint);
    if (language != LyricLanguage::Unknown) {
      counts[lyricLanguageIndex(language)] += 1;
    }
    cursor += decoded.byteLength;
  }
  return counts;
}

std::optional<std::string> cleanLine(std::string_view raw) {
  // P1 陷阱：顺序必须是 先剥时间戳 → 再去行内 [tag:value] → 最后滤制作人员，
  // 否则 `[00:00.000][by:x]` 会漏过过滤。
  const std::string_view stripped = stripPython(raw);
  if (stripped.empty()) {
    return std::nullopt;
  }
  const std::string_view afterTimestamp = stripPython(stripped.substr(timestampPrefixLength(stripped)));
  if (afterTimestamp.empty()) {
    return std::nullopt;
  }
  const std::string withoutTags = removeInlineTags(afterTimestamp);
  const std::string_view body = stripPython(withoutTags);
  if (body.empty()) {
    return std::nullopt;
  }
  if (matchMetadata(body) || matchCredit(body)) {
    return std::nullopt;
  }
  return std::string(body);
}

// ── todo 5：候选枚举 / 括号区 / 边缘剥离 / 两套验证原语 ────────────────────────
// 全部是 Python 参考实现的逐行等价移植。**位置与 cut 一律是 UTF-8 字节偏移**，
// 参考实现是码点下标；凡涉及「按字符」迭代的地方都在 decodeUtf8At 的 byteLength
// 里累计字节，绝不把码点下标当偏移用（见文件头「字节偏移」硬约束）。

std::vector<std::size_t> candidatePositions(std::string_view text, std::string_view kind) {
  std::vector<std::size_t> positions;
  if (isSeparatorKind(kind)) {
    std::size_t start = 0;
    while (true) {
      const std::size_t hit = text.find(kind, start);
      if (hit == std::string_view::npos) {
        break;
      }
      positions.push_back(hit + kind.size());  // 切点在匹配【之前】⇒ 取匹配末尾
      start = hit + 1;
    }
  } else if (kind == "SCRIPT") {
    std::optional<LyricLanguage> previous;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
      const auto decoded = decodeUtf8At(text, cursor);
      const LyricLanguage current = classOfCodePoint(decoded.codePoint);
      if (current == LyricLanguage::Unknown) {
        cursor += decoded.byteLength;  // Python 的 continue：跳过无类字符且不更新 prev
        continue;
      }
      if (previous.has_value() && isNonTargetDecisive(*previous) && current == LyricLanguage::Han) {
        positions.push_back(cursor);
      }
      previous = current;
      cursor += decoded.byteLength;
    }
  }
  return positions;
}

std::vector<std::size_t> bracketRegions(std::string_view text) {
  std::vector<std::size_t> stack;
  std::vector<std::size_t> starts;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    const BracketCharKind kind = bracketCharKindAt(text, cursor);
    if (kind == BracketCharKind::Opener) {
      stack.push_back(cursor);
    } else if (kind == BracketCharKind::Closer && !stack.empty()) {
      const std::size_t start = stack.back();
      stack.pop_back();
      if (stack.empty()) {
        starts.push_back(start);  // 仅最外层（内层嵌套由外层代表）
      }
    }
    cursor += decoded.byteLength;
  }
  std::reverse(starts.begin(), starts.end());
  return starts;
}

std::optional<std::size_t> bracketSpan(std::string_view text, std::size_t start) {
  std::size_t depth = 0;
  std::size_t cursor = start;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    const BracketCharKind kind = bracketCharKindAt(text, cursor);
    if (kind == BracketCharKind::Opener) {
      ++depth;
    } else if (kind == BracketCharKind::Closer && depth > 0) {
      --depth;
      if (depth == 0) {
        return cursor + decoded.byteLength;  // 结束位置【不含】配对闭括号之后
      }
    }
    cursor += decoded.byteLength;
  }
  return std::nullopt;
}

bool cutInsideBracket(std::string_view text, std::size_t cut) {
  for (const std::size_t start : bracketRegions(text)) {
    const auto end = bracketSpan(text, start);
    if (end.has_value() && start < cut && cut < *end) {
      return true;
    }
  }
  return false;
}

std::pair<std::string_view, std::string_view> trimEdges(std::string_view text, std::size_t cut,
                                                        std::optional<std::string_view> kind) {
  if (cut > text.size()) {
    cut = text.size();  // Python 的 text[:cut] / text[cut:] 对越界下标是钳制而非异常
  }
  std::string_view left = text.substr(0, cut);
  std::string_view right = text.substr(cut);
  const bool onlyGivenKind = kind.has_value() && !kind->empty();
  while (true) {
    const std::pair<std::string_view, std::string_view> before{left, right};
    if (onlyGivenKind) {
      while (right.starts_with(*kind)) {
        right.remove_prefix(kind->size());
      }
      while (left.ends_with(*kind)) {
        left.remove_suffix(kind->size());
      }
    } else {
      for (const auto token : kSeparatorTrailChars) {
        while (right.starts_with(token)) {
          right.remove_prefix(token.size());
        }
        while (left.ends_with(token)) {
          left.remove_suffix(token.size());
        }
      }
    }
    left = rstripPython(left);
    right = lstripPython(right);
    if (left == before.first && right == before.second) {
      break;  // 分隔符与空白【交替】剥到稳定为止
    }
  }
  return {left, right};
}

std::optional<std::pair<std::string, std::string>> validateCut(std::string_view text, std::size_t cut,
                                                               std::string_view target) {
  const auto targetClass = targetClassOf(target);
  if (!targetClass.has_value()) {
    return std::nullopt;
  }
  const auto trimmed = trimEdges(text, cut, std::nullopt);
  if (trimmed.first.empty() || trimmed.second.empty()) {
    return std::nullopt;
  }
  const LyricLanguageProfile rightProfile = profileOf(trimmed.second);
  if (rightProfile[lyricLanguageIndex(*targetClass)] == 0) {
    return std::nullopt;
  }
  if (rightProfile[lyricLanguageIndex(LyricLanguage::Japanese)] != 0 ||
      rightProfile[lyricLanguageIndex(LyricLanguage::Korean)] != 0) {
    return std::nullopt;
  }
  const LyricLanguageProfile leftProfile = profileOf(trimmed.first);
  bool hasDecisiveEvidence = false;
  for (const auto language : kNonTargetDecisive) {
    if (leftProfile[lyricLanguageIndex(language)] != 0) {
      hasDecisiveEvidence = true;
      break;
    }
  }
  if (!hasDecisiveEvidence) {
    return std::nullopt;
  }
  return std::make_pair(std::string(trimmed.first), std::string(trimmed.second));
}

std::optional<std::pair<std::string, std::string>> validateCutPure(std::string_view text,
                                                                   std::size_t cut,
                                                                   std::string_view target,
                                                                   bool guarded) {
  const auto targetClass = targetClassOf(target);
  if (!targetClass.has_value()) {
    return std::nullopt;  // 与 validateCut 同判据（target 不在表里）
  }
  const auto got = validateCut(text, cut, target);
  if (!got.has_value()) {
    return std::nullopt;
  }
  const std::string_view right = got->second;
  std::optional<LyricLanguage> head;
  std::size_t cursor = 0;
  while (cursor < right.size()) {
    const auto decoded = decodeUtf8At(right, cursor);
    const LyricLanguage current = classOfCodePoint(decoded.codePoint);
    if (current != LyricLanguage::Unknown) {
      head = current;  // 右侧第一个【可分类】字符（跳过引号/括号等无类字符）
      break;
    }
    cursor += decoded.byteLength;
  }
  if (!head.has_value() || *head != *targetClass) {
    return std::nullopt;
  }
  const LyricLanguageProfile rightProfile = profileOf(right);
  for (std::size_t index = 0; index < kLyricLanguageClassCount; ++index) {
    if (rightProfile[index] != 0 &&
        isMixedScriptBlocker(static_cast<LyricLanguage>(index))) {
      return std::nullopt;
    }
  }
  if (guarded && cutInsideBracket(text, cut)) {
    return std::nullopt;
  }
  return got;
}

// ── todo 6：三条切分路径（显式 / 脚本 / 括号）与结果记入 ──────────────────────
// 逐行等价移植参考实现 `_separator_runs` / `_cut_explicit` / `_cut_script` /
// `_is_target_bracket` / `_cut_bracket` / `_find_any_bracket` / `_accept`。
// 位置一律【字节偏移】（参考实现是码点下标，换算在 decodeUtf8At 的 byteLength 累计）。

std::vector<std::pair<std::size_t, std::size_t>> separatorRuns(
    std::string_view text, const std::vector<std::size_t>& positions, std::string_view kind) {
  std::vector<std::pair<std::size_t, std::size_t>> spans;
  if (positions.empty()) {
    return spans;
  }
  std::vector<std::vector<std::size_t>> runs{{positions.front()}};
  for (std::size_t index = 1; index < positions.size(); ++index) {
    const std::size_t pos = positions[index];
    const std::size_t previous = runs.back().back();
    // Python `text[prev:pos].strip(kind).strip() == ""`：先按 kind 的码点集合剥、
    // 再按 Python 空白剥；两者都空 ⇒ 两切点属同一段（中间只有分隔符与空白）。
    const std::string_view between = text.substr(previous, pos - previous);
    if (stripPython(stripPythonChars(between, kind)).empty()) {
      runs.back().push_back(pos);
    } else {
      runs.push_back(std::vector<std::size_t>{pos});
    }
  }
  for (const auto& run : runs) {
    std::size_t start = run.front() - kind.size();  // 段起点 = 匹配起点（字节）
    std::size_t end = run.back();
    while (end < text.size()) {
      const auto decoded = decodeUtf8At(text, end);
      if (!isPythonWhitespace(decoded.codePoint)) {
        break;
      }
      end += decoded.byteLength;  // 段 end 吃掉后续 Python 空白
    }
    spans.emplace_back(start, end);
  }
  return spans;
}

std::optional<LyricCut> cutExplicit(std::string_view text, std::string_view kind, bool guarded) {
  std::vector<std::size_t> positions = candidatePositions(text, kind);
  if (guarded) {
    // 先剔除括号内部的候选，**段的计数也必须在剔除之后**（计的是可用分隔，不是
    // 字面出现次数）：`（假名 / 罗马音）` 里的分隔符是读音装饰。
    std::vector<std::size_t> usable;
    usable.reserve(positions.size());
    for (const std::size_t pos : positions) {
      if (!cutInsideBracket(text, pos)) {
        usable.push_back(pos);
      }
    }
    positions = std::move(usable);
  }
  std::vector<std::pair<std::size_t, std::size_t>> runs = separatorRuns(text, positions, kind);
  if (runs.size() >= kMaxSeparatorOccurrences) {
    return std::nullopt;  // 多段并列（D32/D10）：放弃切分，不赌一个
  }
  std::sort(runs.begin(), runs.end(), [](const auto& lhs, const auto& rhs) {
    // Python `sorted(runs, reverse=True)`：先按起点降序、再按终点降序（取最右段先试）。
    if (lhs.first != rhs.first) {
      return lhs.first > rhs.first;
    }
    return lhs.second > rhs.second;
  });
  for (const auto& run : runs) {
    const std::string_view left = rstripPython(text.substr(0, run.first));
    std::string_view right = lstripPython(text.substr(run.second));
    // 译文侧再剥一层分隔符噪声（`原文 / /译文` 的裸 `/` 是记号残留，实测 127 行）；
    // 原文侧【不】剥 —— `＼なので～す／` 的 `／` 是内容（见 trimEdges 的说明）。
    right = lstripPython(lstripSeparatorTrailChars(right));
    if (!left.empty() && !right.empty()) {
      return LyricCut{std::string(left), std::string(right)};  // 两侧相同也照切
    }
  }
  return std::nullopt;
}

std::optional<LyricCut> cutScript(std::string_view text, std::string_view target, bool guarded) {
  static_cast<void>(target);  // 过渡判据硬编码 HAN（与 candidate_positions 的 SCRIPT 分支一致）
  std::optional<LyricLanguage> previous;
  std::optional<std::size_t> cut;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    const LyricLanguage current = classOfCodePoint(decoded.codePoint);
    if (current == LyricLanguage::Unknown) {
      cursor += decoded.byteLength;  // Python 的 continue：无类字符不更新 previous
      continue;
    }
    if (previous.has_value() && isNonTargetDecisive(*previous) &&
        current == LyricLanguage::Han && !(guarded && cutInsideBracket(text, cursor))) {
      cut = cursor;  // 覆盖式赋值 ⇒ 最终留下【最后一次】过渡
    }
    previous = current;
    cursor += decoded.byteLength;
  }
  if (!cut.has_value()) {
    return std::nullopt;
  }
  const auto trimmed = trimEdges(text, *cut, std::nullopt);
  if (trimmed.first.empty() || trimmed.second.empty()) {
    return std::nullopt;
  }
  const LyricLanguageProfile rightProfile = profileOf(trimmed.second);
  for (const auto language : kNonTargetDecisive) {
    if (rightProfile[lyricLanguageIndex(language)] != 0) {
      return std::nullopt;  // 右侧不得含任何非目标决定性类（含拉丁）
    }
  }
  return LyricCut{std::string(trimmed.first), std::string(trimmed.second)};
}

bool isTargetBracket(std::string_view text, std::size_t start, std::string_view target) {
  const auto targetClass = targetClassOf(target);
  if (!targetClass.has_value()) {
    // Python 对表外的 target 会 KeyError；本模块对未知 target 一律「不匹配」，
    // 与 validateCut/validateCutPure 的 nullopt 口径一致（调用方只会传有效 target）。
    return false;
  }
  const auto end = bracketSpan(text, start);
  if (!end.has_value()) {
    return false;
  }
  const LyricLanguageProfile counts = profileOf(text.substr(start, *end - start));
  if (counts[lyricLanguageIndex(*targetClass)] == 0) {
    return false;
  }
  // 不用占比判据：译文常夹英文（`「I Miss You 一刻也停不下来」` 占比仅 0.47），而日文
  // 注音括号占比也可能不低（0.42）。真正的区别是语言【种类】：含 JA/KO/CYRL ⇒ 不是译文。
  return counts[lyricLanguageIndex(LyricLanguage::Japanese)] == 0 &&
         counts[lyricLanguageIndex(LyricLanguage::Korean)] == 0 &&
         counts[lyricLanguageIndex(LyricLanguage::Cyrillic)] == 0;
}

std::optional<LyricCut> cutBracket(std::string_view text, std::size_t start,
                                   std::string_view target) {
  if (!isTargetBracket(text, start, target)) {
    return std::nullopt;
  }
  const auto trimmed = trimEdges(text, start, std::nullopt);
  if (trimmed.first.empty() || trimmed.second.empty()) {
    return std::nullopt;
  }
  return LyricCut{std::string(trimmed.first), std::string(trimmed.second)};
}

std::optional<LyricCut> findAnyBracket(std::string_view text, std::string_view target,
                                       bool trailingOnly) {
  // bracketRegions 已按【右→左】返回最外层起点，故这里直接顺序尝试即最右优先。
  for (const std::size_t start : bracketRegions(text)) {
    const auto end = bracketSpan(text, start);
    if (!end.has_value()) {
      continue;
    }
    if (trailingOnly && !stripPython(text.substr(*end)).empty()) {
      continue;  // 只接受延伸到行尾的括号区（否则会把日文注音的（こうかきょう）当译文）
    }
    if (auto got = cutBracket(text, start, target); got.has_value()) {
      return got;
    }
  }
  return std::nullopt;
}

LyricSplitResult accept(std::string_view left, std::string_view right,
                        LyricSplitConvention convention, std::string_view route) {
  LyricSplitResult result;
  result.original = std::string(left);
  result.translation = std::string(right);
  result.effectiveConvention = convention;
  result.confidence = (route == "weak") ? "medium" : "high";
  result.reason = std::string(route);
  return result;
}

std::string conventionToken(LyricSplitConvention convention) {
  // 见 inc/seriona/control/control_contracts.h 的注释：返回的是【转义前原始记号】，
  // 与 Python `_convention_notation()` 逐字节一致；写 TSV 时再套 `_escape_tsv`。
  switch (convention) {
    case LyricSplitConvention::None:
      return "-";
    case LyricSplitConvention::StrongSlashSpaced:
      return "S: / ";
    case LyricSplitConvention::StrongFullwidthBar:
      return "S:｜";
    case LyricSplitConvention::StrongBar:
      return "S:|";
    case LyricSplitConvention::StrongFullwidthSlash:
      return "S:／";
    case LyricSplitConvention::StrongSlash:
      return "S:/";
    case LyricSplitConvention::StrongBackslash:
      return "S:\\";  // 单反斜杠，共 3 字节
    case LyricSplitConvention::WeakTab:
      return "W:\t";  // "W:" + TAB 字符，共 3 字节（★MED-R1：不是字面 `\t` 两字符）
    case LyricSplitConvention::WeakFullwidthSpace:
      return "W:　";
    case LyricSplitConvention::WeakSpace:
      return "W: ";
    case LyricSplitConvention::ScriptTransition:
      return "SCRIPT";
  }
  // 覆盖全部枚举值后仍到达这里 = 调用方传入了未命名的非法枚举值（契约违反）。
  // 不静默返回空串：那会让持久化/闸门把「非法」与「None」混淆。
  throw std::invalid_argument("conventionToken: 未知的 LyricSplitConvention 枚举值");
}

std::optional<LyricSplitConvention> conventionFromToken(std::string_view token) {
  if (token == "-") {
    return LyricSplitConvention::None;
  }
  if (token == "S: / ") {
    return LyricSplitConvention::StrongSlashSpaced;
  }
  if (token == "S:｜") {
    return LyricSplitConvention::StrongFullwidthBar;
  }
  if (token == "S:|") {
    return LyricSplitConvention::StrongBar;
  }
  if (token == "S:／") {
    return LyricSplitConvention::StrongFullwidthSlash;
  }
  if (token == "S:/") {
    return LyricSplitConvention::StrongSlash;
  }
  if (token == "S:\\") {
    return LyricSplitConvention::StrongBackslash;
  }
  if (token == "W:\t") {
    return LyricSplitConvention::WeakTab;
  }
  if (token == "W:　") {
    return LyricSplitConvention::WeakFullwidthSpace;
  }
  if (token == "W: ") {
    return LyricSplitConvention::WeakSpace;
  }
  if (token == "SCRIPT") {
    return LyricSplitConvention::ScriptTransition;
  }
  return std::nullopt;
}

LyricSplitResult splitLyricLine(std::string_view line,
                                LyricSplitConvention convention,
                                std::string_view targetLanguage) {
  // 逐分支复刻 Python `split_line`（lyric_split_tool.py:482-587）。分支【顺序】本身是
  // 契约：先空输入、再 `//` 哨兵、再 None、再强、再弱/SCRIPT、最后括号兜底。任何重排
  // 都会改变多类行的结果（例如把括号兜底提前会污染含 ` / ` 的行）。
  if (line.empty()) {
    return LyricSplitResult{std::string(line), "", LyricSplitConvention::None, "none", "empty"};
  }
  // QQ 音乐用整行 `//`（strip 后）表示「此行无译文」。
  if (stripPython(line) == "//") {
    return LyricSplitResult{std::string(line), "", LyricSplitConvention::None, "high",
                            "qq-sentinel"};
  }
  if (convention == LyricSplitConvention::None) {
    // 文件级门槛不否决行内作者表态：行内写着 ` / ` 就直接采信。只试带空格的 ` / `
    // （裸 `/` 会命中 `SOL_FAGE/1x10` 类原文记法）。
    const auto got = cutExplicit(line, kSpacedSeparator);
    if (got.has_value()) {
      return accept(got->first, got->second, LyricSplitConvention::StrongSlashSpaced, "strong");
    }
    return LyricSplitResult{std::string(line), "", LyricSplitConvention::None, "none",
                            "no-convention"};
  }

  const auto kind = kindOfConvention(convention);
  if (!kind.has_value()) {
    // 表外枚举值不可达（调用方只会传有效约定）；保守退回未切分。
    return LyricSplitResult{std::string(line), "", LyricSplitConvention::None, "none",
                            "validate-failed"};
  }

  if (isStrongSeparatorKind(*kind)) {
    const auto got = cutExplicit(line, *kind);
    if (got.has_value()) {
      return accept(got->first, got->second, convention, "strong");
    }
    // 仅当候选【全部落在括号内部】才允许结构性回退；候选在括号外被否决（多段并列）时
    // 必须整行作原文，绝不回退（D10/D28）。
    const std::vector<std::size_t> candidates = candidatePositions(line, *kind);
    bool allInsideBracket = !candidates.empty();
    if (allInsideBracket) {
      for (const std::size_t position : candidates) {
        if (!cutInsideBracket(line, position)) {
          allInsideBracket = false;
          break;
        }
      }
    }
    if (allInsideBracket) {
      for (const std::string_view other : kStrongSeparators) {
        if (other == *kind) {
          continue;
        }
        const auto otherGot = cutExplicit(line, other);
        if (otherGot.has_value()) {
          return accept(otherGot->first, otherGot->second, conventionOfKind(other).value(),
                        "strong-fallback");
        }
      }
      const auto bracketGot = findAnyBracket(line, targetLanguage, /*trailingOnly=*/true);
      if (bracketGot.has_value()) {
        return accept(bracketGot->first, bracketGot->second, convention,
                      "strong-bracket-fallback");
      }
      // 弱边界候选按【位置升序】先逐个 validateCutPure，全败才 cutScript（参考实现是
      // 两段；写成「直接 cutScript」会漏掉前半段、产出不同结果）。
      std::vector<std::size_t> weakPositions;
      for (const std::string_view boundary : kWeakBoundaries) {
        for (const std::size_t position : candidatePositions(line, boundary)) {
          weakPositions.push_back(position);
        }
      }
      std::sort(weakPositions.begin(), weakPositions.end());
      weakPositions.erase(std::unique(weakPositions.begin(), weakPositions.end()),
                          weakPositions.end());
      for (const std::size_t position : weakPositions) {
        const auto pureGot = validateCutPure(line, position, targetLanguage);
        if (pureGot.has_value()) {
          return accept(pureGot->first, pureGot->second, convention, "strong-weak-fallback");
        }
      }
      const auto scriptGot = cutScript(line, targetLanguage);
      if (scriptGot.has_value()) {
        return accept(scriptGot->first, scriptGot->second, convention, "strong-weak-fallback");
      }
    }
    return LyricSplitResult{std::string(line), "", LyricSplitConvention::None, "none",
                            "validate-failed"};
  }

  if (isWeakBoundaryKind(*kind) || convention == LyricSplitConvention::ScriptTransition) {
    // 行内显式 ` / ` 的证据强于文件弱约定。
    const auto got = cutExplicit(line, kSpacedSeparator);
    if (got.has_value()) {
      return accept(got->first, got->second, LyricSplitConvention::StrongSlashSpaced, "strong");
    }
    // 行内已含显式 ` / ` 时不再走括号结构猜测（否则原文会被污染成 `「A」 / 「B」 / 「C」 /`）。
    if (line.find(kSpacedSeparator) == std::string_view::npos) {
      const auto bracketGot = findAnyBracket(line, targetLanguage, /*trailingOnly=*/true);
      if (bracketGot.has_value()) {
        return accept(bracketGot->first, bracketGot->second, convention, "bracket");
      }
    }
    if (isWeakBoundaryKind(*kind)) {
      // 弱边界按候选位置【从最左往右】逐个试（取最右会切进译文内部）。
      for (const std::size_t position : candidatePositions(line, *kind)) {
        const auto pureGot = validateCutPure(line, position, targetLanguage);
        if (pureGot.has_value()) {
          return accept(pureGot->first, pureGot->second, convention, "weak");
        }
      }
    } else {
      const auto scriptGot = cutScript(line, targetLanguage);
      if (scriptGot.has_value()) {
        return accept(scriptGot->first, scriptGot->second, convention, "weak");
      }
    }
  }

  const auto bracketGot = findAnyBracket(line, targetLanguage);
  if (bracketGot.has_value()) {
    return accept(bracketGot->first, bracketGot->second, convention, "bracket");
  }
  return LyricSplitResult{std::string(line), "", LyricSplitConvention::None, "none",
                          "validate-failed"};
}

bool acceptsVote(std::string_view line, std::string_view kind, std::string_view target) {
  // 计票与执行解耦（D36⑤）：唯一例外是 D20 括号守卫，一律以 guarded=false 计票。
  if (isStrongSeparatorKind(kind)) {
    return cutExplicit(line, kind, /*guarded=*/false).has_value();
  }
  if (isWeakBoundaryKind(kind)) {
    for (const std::size_t position : candidatePositions(line, kind)) {
      if (validateCutPure(line, position, target, /*guarded=*/false).has_value()) {
        return true;  // Python `any(...)`
      }
    }
    return false;
  }
  // 其余只有 "SCRIPT"（kConventions 末位）—— 必须走 cutScript，**绝不**塞进
  // cutExplicit/separatorRuns（后者对 "SCRIPT" 会静默错）。
  return cutScript(line, target, /*guarded=*/false).has_value();
}

LyricSplitConventionVotes inferSplitConvention(const std::vector<std::string>& cleanedLines,
                                               std::string_view targetLanguage) {
  // 逐行投票：每行只投一次，取 kConventions 中第一个被 acceptsVote 受理的 kind
  // （顺序即优先级 —— 反斜杠在 kStrongSeparators 末位正是为此）。
  std::array<int, kConventions.size()> counts{};
  std::vector<std::size_t> voteOrder;  // 各 kind 首次得票的顺序（复刻 Python dict 插入顺序）
  for (const std::string& line : cleanedLines) {
    for (std::size_t index = 0; index < kConventionKinds.size(); ++index) {
      if (acceptsVote(line, kConventionKinds[index].second, targetLanguage)) {
        if (counts[index] == 0) {
          voteOrder.push_back(index);
        }
        ++counts[index];
        break;
      }
    }
  }
  LyricSplitConventionVotes result;
  const std::size_t total = cleanedLines.size();
  if (total == 0) {
    return result;  // {None, {}}
  }
  // 保留全部已得票的 kind（含不达标者），按【首次得票顺序】（与 Python `dict(votes)` 一致）。
  for (const std::size_t index : voteOrder) {
    result.votes.emplace_back(kConventionKinds[index].first, counts[index]);
  }
  for (std::size_t index = 0; index < kConventionKinds.size(); ++index) {
    const int count = counts[index];
    // 门槛：绝对数 >= 2 且占比 >= 40%。用 double 真除法精确复刻 Python 的
    // `votes[kind] / total >= 0.4`（不要改写成整数式 count*5 >= total*2 而无说明）。
    if (count >= 2 && static_cast<double>(count) / static_cast<double>(total) >= 0.4) {
      result.convention = kConventionKinds[index].first;
      break;  // 按 kConventions 顺序取【第一个】满足门槛者；皆不满足 ⇒ None
    }
  }
  return result;
}

LyricDocumentSplit splitDocument(const std::vector<std::string>& rawLines,
                                 std::string_view path,
                                 std::string_view targetLanguage) {
  // 逐字复刻 Python `split_document`（lyric_split_tool.py:598-603）：
  // 清洗（丢弃非歌词正文行）-> 推断文档级约定 -> 逐行切分，保留「清洗后的行 ↔ 结果」配对。
  std::vector<std::string> cleaned;
  cleaned.reserve(rawLines.size());
  for (const std::string& raw : rawLines) {
    const auto line = cleanLine(raw);
    if (line.has_value()) {
      cleaned.push_back(*line);
    }
  }
  const LyricSplitConventionVotes inferred = inferSplitConvention(cleaned, targetLanguage);
  LyricDocumentSplit document;
  document.path = std::string(path);
  document.convention = inferred.convention;
  document.votes = inferred.votes;
  document.results.reserve(cleaned.size());
  for (const std::string& line : cleaned) {
    document.results.emplace_back(line,
                                  splitLyricLine(line, inferred.convention, targetLanguage));
  }
  return document;
}

}  // namespace seriona::control
