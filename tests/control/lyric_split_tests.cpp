#include "control/lyric_split.h"
#include "scanner/path_utf8.h"

#include <doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::control {
namespace {

// 测试侧【独立书写】的参考窄码点表：刻意按语言类分组（不做按起点排序），与
// src/control/lyric_split.cpp 的「按区间起点排序」形态不同，避免同一处转写错误
// 被实现与测试共享。数值取自 Python 参考实现 class_of_cp 的 if 链。
struct ExpectedLanguageRange {
  std::uint32_t first;
  std::uint32_t last;
  LyricLanguage language;
};

constexpr std::array<ExpectedLanguageRange, 14> kExpectedLanguageRanges{{
    {0x3040U, 0x309FU, LyricLanguage::Japanese},
    {0x30A0U, 0x30FFU, LyricLanguage::Japanese},
    {0x31F0U, 0x31FFU, LyricLanguage::Japanese},
    {0xFF66U, 0xFF9DU, LyricLanguage::Japanese},
    {0x4E00U, 0x9FFFU, LyricLanguage::Han},
    {0x3400U, 0x4DBFU, LyricLanguage::Han},
    {0xF900U, 0xFAFFU, LyricLanguage::Han},
    {0x3005U, 0x3007U, LyricLanguage::Han},
    {0xAC00U, 0xD7AFU, LyricLanguage::Korean},
    {0x1100U, 0x11FFU, LyricLanguage::Korean},
    {0x0041U, 0x005AU, LyricLanguage::Latin},
    {0x0061U, 0x007AU, LyricLanguage::Latin},
    {0x00C0U, 0x024FU, LyricLanguage::Latin},
    {0x0400U, 0x04FFU, LyricLanguage::Cyrillic},
}};

[[nodiscard]] LyricLanguage expectedClassOfCodePoint(std::uint32_t codePoint) {
  for (const auto& range : kExpectedLanguageRanges) {
    if (codePoint >= range.first && codePoint <= range.last) {
      return range.language;
    }
  }
  return LyricLanguage::Unknown;
}

struct ExpectedConventionToken {
  LyricSplitConvention convention;
  std::string_view token;
};

// 11 个枚举值 -> 预期的【转义前原始记号】。WeakTab 的原始记号是 "W:" + TAB 字符
// （共 3 字节）—— 这是 ★MED-R1 订正后的语义，写 TSV 时才由 _escape_tsv 转义成 4 字节。
constexpr std::array<ExpectedConventionToken, 11> kExpectedTokens{{
    {LyricSplitConvention::None, "-"},
    {LyricSplitConvention::StrongSlashSpaced, "S: / "},
    {LyricSplitConvention::StrongFullwidthBar, "S:｜"},
    {LyricSplitConvention::StrongBar, "S:|"},
    {LyricSplitConvention::StrongFullwidthSlash, "S:／"},
    {LyricSplitConvention::StrongSlash, "S:/"},
    {LyricSplitConvention::StrongBackslash, "S:\\"},
    {LyricSplitConvention::WeakTab, "W:\t"},
    {LyricSplitConvention::WeakFullwidthSpace, "W:　"},
    {LyricSplitConvention::WeakSpace, "W: "},
    {LyricSplitConvention::ScriptTransition, "SCRIPT"},
}};

[[nodiscard]] std::string hexBytesOf(std::string_view text) {
  constexpr char kDigits[] = "0123456789ABCDEF";
  std::string rendered;
  rendered.reserve(text.size() * 3);
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (index != 0) {
      rendered.push_back(' ');
    }
    const auto byte = static_cast<unsigned char>(text[index]);
    rendered.push_back(kDigits[byte >> 4U]);
    rendered.push_back(kDigits[byte & 0x0FU]);
  }
  return rendered;
}

// route/reason 用例的统一期望：**五项同时断言**（original / translation /
// effectiveConvention / confidence / reason）—— 只比原文/译文会让「reason 不同但
// 两侧相同」的两条 route（no-convention vs validate-failed）互相冒充。
struct ExpectedSplit {
  std::string_view original;
  std::string_view translation;
  LyricSplitConvention convention;
  std::string_view confidence;
  std::string_view reason;
};

void checkSplit(const LyricSplitResult& actual, const ExpectedSplit& expected) {
  CHECK(actual.original == expected.original);
  CHECK(actual.translation == expected.translation);
  CHECK(actual.effectiveConvention == expected.convention);
  CHECK(actual.confidence == expected.confidence);
  CHECK(actual.reason == expected.reason);
}

[[nodiscard]] std::vector<std::string> readFixtureLines(const std::string& path) {
  std::ifstream input{path, std::ios::binary};
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  return lines;
}

// ── D20 括号配对性质（todo 10）────────────────────────────────────────────────
// 测试侧【独立书写】的成对表（刻意与实现的 kOpeners/kClosers 分开维护）。编译期
// 断言两表等长、逐位非空、且 26 个记号两两不同，避免一次转写错误让性质断言空转。
constexpr std::array<std::string_view, 13> kExpectedOpeners{
    "（", "(", "「", "【", "〔", "〈", "《", "『", "［", "[", "{", "＜", "<"};
constexpr std::array<std::string_view, 13> kExpectedClosers{
    "）", ")", "」", "】", "〕", "〉", "》", "』", "］", "]", "}", "＞", ">"};

[[nodiscard]] constexpr bool bracketPairTablesAreConsistent() noexcept {
  if constexpr (kExpectedOpeners.size() != kExpectedClosers.size()) {
    return false;
  }
  for (std::size_t index = 0; index < kExpectedOpeners.size(); ++index) {
    if (kExpectedOpeners[index].empty() || kExpectedClosers[index].empty() ||
        kExpectedOpeners[index] == kExpectedClosers[index]) {
      return false;
    }
    for (std::size_t other = index + 1; other < kExpectedOpeners.size(); ++other) {
      if (kExpectedOpeners[index] == kExpectedOpeners[other] ||
          kExpectedClosers[index] == kExpectedClosers[other] ||
          kExpectedOpeners[index] == kExpectedClosers[other] ||
          kExpectedClosers[index] == kExpectedOpeners[other]) {
        return false;
      }
    }
  }
  return true;
}
static_assert(bracketPairTablesAreConsistent(), "括号成对表必须等长且 26 个记号两两不同");

[[nodiscard]] std::size_t countOccurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t at = text.find(needle);
  while (at != std::string_view::npos) {
    ++count;
    at = text.find(needle, at + needle.size());
  }
  return count;
}

// D20 的可观测判据：一次切分产生的两侧，各自括号必须配对（「」不得被拆成两半）。
[[nodiscard]] bool bracketsAreBalanced(std::string_view text) {
  std::size_t openers = 0;
  std::size_t closers = 0;
  for (const auto opener : kExpectedOpeners) {
    openers += countOccurrences(text, opener);
  }
  for (const auto closer : kExpectedClosers) {
    closers += countOccurrences(text, closer);
  }
  return openers == closers;
}

}  // namespace

TEST_CASE("lyric split convention token round trip and uniqueness") {
  std::set<std::string> distinctTokens;
  std::cout << "[convention-tokens] count=" << kExpectedTokens.size() << '\n';
  for (const auto& entry : kExpectedTokens) {
    const std::string token = conventionToken(entry.convention);
    CAPTURE(entry.convention);
    CAPTURE(token);
    CAPTURE(token.size());
    std::cout << "[convention-tokens] enum=" << static_cast<int>(entry.convention)
              << " utf8_bytes=" << token.size() << " hex=" << hexBytesOf(token) << " token="
              << token << '\n';
    CHECK(token == entry.token);
    const auto restored = conventionFromToken(token);
    REQUIRE(restored.has_value());
    CHECK(*restored == entry.convention);
    distinctTokens.insert(token);
  }
  std::cout << "[convention-tokens] distinct=" << distinctTokens.size() << '\n';
  // 11 个记号两两不同（钉住「记号无损、无碰撞」）。
  CHECK(kExpectedTokens.size() == 11);
  CHECK(distinctTokens.size() == kExpectedTokens.size());
}

TEST_CASE("lyric split convention raw notation is pre-escape form") {
  // ★MED-R1：原始记号不是 TSV 已转义形态。
  const std::string backslash = conventionToken(LyricSplitConvention::StrongBackslash);
  CHECK(backslash == std::string{"S:\\"});
  CHECK(backslash.size() == 3);
  CHECK(static_cast<unsigned char>(backslash[2]) == 0x5CU);

  const std::string tab = conventionToken(LyricSplitConvention::WeakTab);
  CHECK(tab == std::string{"W:\t"});
  CHECK(tab.size() == 3);
  CHECK(static_cast<unsigned char>(tab[2]) == 0x09U);

  // 已转义形态（4 字节）不是记号，必须被拒绝。
  CHECK_FALSE(conventionFromToken("W:\\t").has_value());
  CHECK_FALSE(conventionFromToken("S:\\\\").has_value());
  CHECK_FALSE(conventionFromToken("").has_value());
  CHECK_FALSE(conventionFromToken("nonsense").has_value());

  // 未知枚举值不得静默返回空串。
  const auto tokenOfUnnamedValue = [] {
    return conventionToken(static_cast<LyricSplitConvention>(13));
  };
  CHECK_THROWS_AS(tokenOfUnnamedValue(), std::invalid_argument);
}

TEST_CASE("classOfCodePoint agrees with independent narrow range table on 0..0x10FFFF") {
  const auto started = std::chrono::steady_clock::now();
  std::size_t mismatches = 0;
  for (std::uint32_t codePoint = 0; codePoint <= 0x10FFFFU; ++codePoint) {
    const LyricLanguage actual = classOfCodePoint(codePoint);
    const LyricLanguage expected = expectedClassOfCodePoint(codePoint);
    if (actual != expected) {
      if (mismatches < 8) {
        MESSAGE("码点 U+", codePoint, " 期望 ", static_cast<int>(expected), " 实得 ",
                static_cast<int>(actual));
      }
      ++mismatches;
    }
  }
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  std::cout << "[exhaustive-codepoints] mismatches=" << mismatches
            << " elapsed_ms=" << elapsedMs << '\n';
  CHECK(mismatches == 0);
  // 无预设（-O0）构建下的上界；R14-A N-4 已把原设 50ms 放宽到 200ms。
  CHECK(elapsedMs < 200);
}

TEST_CASE("classOfCodePoint returns Unknown exactly where ICU would diverge") {
  // 这些区间 ICU 的原生脚本类会给出与参考实现不同的答案（参考实现一律 None），
  // 是本模块禁用 ICU 的直接证据。
  CHECK(classOfCodePoint(0xFF21U) == LyricLanguage::Unknown);   // 全角拉丁 A
  CHECK(classOfCodePoint(0x03B1U) == LyricLanguage::Unknown);   // 希腊 α
  CHECK(classOfCodePoint(0x0627U) == LyricLanguage::Unknown);   // 阿拉伯 ا
  CHECK(classOfCodePoint(0x0E01U) == LyricLanguage::Unknown);   // 泰文 ก
  CHECK(classOfCodePoint(0x20000U) == LyricLanguage::Unknown);  // CJK 扩展 B
  CHECK(classOfCodePoint(0x1E00U) == LyricLanguage::Unknown);   // 拉丁扩展附加

  CHECK(classOfCodePoint(0x3042U) == LyricLanguage::Japanese);  // あ
  CHECK(classOfCodePoint(0x30A2U) == LyricLanguage::Japanese);  // ア
  CHECK(classOfCodePoint(0xFF71U) == LyricLanguage::Japanese);  // ｱ
  CHECK(classOfCodePoint(0x4E2DU) == LyricLanguage::Han);       // 中
  CHECK(classOfCodePoint(0x3005U) == LyricLanguage::Han);       // 々
  CHECK(classOfCodePoint(0xAC00U) == LyricLanguage::Korean);    // 가
  CHECK(classOfCodePoint(0x0041U) == LyricLanguage::Latin);     // A
  CHECK(classOfCodePoint(0x041FU) == LyricLanguage::Cyrillic);  // П
}

TEST_CASE("profileOf counts only non-neutral language classes") {
  const LyricLanguageProfile counts = profileOf("ab中あП　!");
  CHECK(counts[lyricLanguageIndex(LyricLanguage::Latin)] == 2);
  CHECK(counts[lyricLanguageIndex(LyricLanguage::Han)] == 1);
  CHECK(counts[lyricLanguageIndex(LyricLanguage::Japanese)] == 1);
  CHECK(counts[lyricLanguageIndex(LyricLanguage::Cyrillic)] == 1);
  CHECK(counts[lyricLanguageIndex(LyricLanguage::Korean)] == 0);
  CHECK(counts[lyricLanguageIndex(LyricLanguage::Unknown)] == 0);
}

TEST_CASE("cleanLine strips timestamps then inline tags then credits") {
  struct Case {
    std::string_view input;
    std::optional<std::string> expected;
  };
  const Case cases[] = {
      // 承重：Python str 的 Unicode 空白语义（U+3000 必须被剥）。
      {"　原文　", std::string{"原文"}},
      // U+FEFF 不是 Python 空白，不得被剥。
      {"\ufeffX\ufeff", std::string{"\ufeffX\ufeff"}},
      // P1 顺序陷阱：先剥时间戳，否则 [by:x] 会漏过过滤。
      {"[00:00.000][by:x]foo", std::string{"foo"}},
      {"[00:00.000][01:02.345]bar", std::string{"bar"}},
      {"[by:x]foo", std::string{"foo"}},
      {"  [00:00]  y  ", std::string{"y"}},
      // \d 是 Unicode Nd：阿拉伯-印度数字时间戳也要剥（ASCII 判据会漏）。
      {"[٠١:٢٣]arabic", std::string{"arabic"}},
      // 时间戳数字串超长 ⇒ 整体不匹配（贪心+回溯语义）。
      {"[1:2.3.4]q", std::string{"[1:2.3.4]q"}},
      {"[ 00:00]x", std::string{"[ 00:00]x"}},
      // 行内 tag 的贪心 [^\]]* 吃到第一个 ]。
      {"[a:[b]tail", std::string{"tail"}},
      {"[a_b:c]ok", std::string{"ok"}},
      {"[abc]x", std::string{"[abc]x"}},
      {"[]x", std::string{"[]x"}},
      {"[:]x", std::string{"[:]x"}},
      // 制作人员行（alternation 顺序与回溯）。
      {"作词: 某人", std::nullopt},
      {"词：某某", std::nullopt},
      {"歌詞: x", std::nullopt},
      {"Arr:x", std::nullopt},
      {"Music: x", std::nullopt},
      {"by: someone", std::nullopt},
      {"By: someone", std::nullopt},
      {"歌曲: x", std::string{"歌曲: x"}},  // `歌` 后接 `曲` 不是冒号 ⇒ 不是制作人员
      {"arr: x", std::string{"arr: x"}},    // CREDIT_RE 无 IGNORECASE
      {"mix:x", std::string{"mix:x"}},
      // 空行 / 纯空白 / 剥完即空 ⇒ 非歌词正文。
      {"[00:00]", std::nullopt},
      {"[00:00]   ", std::nullopt},
      {"  \u00a0  ", std::nullopt},
      {"\u3000", std::nullopt},
      {"\t\n", std::nullopt},
      {"", std::nullopt},
  };
  for (const auto& item : cases) {
    CAPTURE(item.input);
    const auto actual = cleanLine(item.input);
    REQUIRE(actual.has_value() == item.expected.has_value());
    if (item.expected.has_value()) {
      CHECK(*actual == *item.expected);
    }
  }
}

TEST_CASE("metadata word set equals Python re.IGNORECASE on [a-zA-Z_]") {
  // METADATA_RE 带 re.IGNORECASE，而 Python 的 IGNORECASE 对 [a-zA-Z_] 会额外纳入
  // 4 个非 ASCII 码点（İ U+0130 / ı U+0131 / ſ U+017F / K U+212A）。穷举 0..0x10FFFF
  // 确认集合封闭、恰为这 4 个。它们必须和 ASCII 字母一样被当元数据行丢弃。
  CHECK_FALSE(cleanLine("[\u0130:x]").has_value());
  CHECK_FALSE(cleanLine("[\u0131:x]").has_value());
  CHECK_FALSE(cleanLine("[\u017f:x]").has_value());
  CHECK_FALSE(cleanLine("[\u212a:x]").has_value());
  CHECK_FALSE(cleanLine("[\u0130\u0131_\u017f\u212a:x]").has_value());
  CHECK_FALSE(cleanLine("  [\u212a:x]  ").has_value());

  // 负向对照：不在该集合内的字符不得让其成为元数据行。
  CHECK(cleanLine("[\u0416:x]").value() == "[\u0416:x]");  // 西里尔 Ж
  CHECK(cleanLine("[abc]x").value() == "[abc]x");          // 无冒号
  CHECK(cleanLine("[]x").value() == "[]x");
  CHECK(cleanLine("[:]x").value() == "[:]x");
  CHECK(cleanLine("[\u212a]x").value() == "[\u212a]x");    // 有 K 但无冒号
  CHECK(cleanLine("[\u0130]x").value() == "[\u0130]x");

  // INLINE_TAG_RE 刻意【不带】IGNORECASE：同样这 4 个码点出现在行中时，行内 tag
  // 不得被删除（否则就是顺手把这个谓词搬过去引入的新分歧）。
  CHECK(cleanLine("x[\u0130:y]z").value() == "x[\u0130:y]z");
  CHECK(cleanLine("x[\u212a:y]z").value() == "x[\u212a:y]z");
  CHECK(cleanLine("x[abc:y]z").value() == "xz");
}

TEST_CASE("language table matches Python-derived class counts and checksum") {
  // 两个常量由 Python 权威实现【机器推导】，不是人工转写：生成器与原始输出见
  // .omo/evidence/task-4-lyric-split-auto-detect/qa/，基准为参考实现 class_of_cp()。
  // 编码 = 码点升序 0..0x10FFFF，每码点贡献 1 字节类下标（LyricLanguage 声明顺序），
  // 64 位 FNV-1a。任何区间数值转写错误（抄错上界、漏抄一段、两类互换）都会立刻变红——
  // 这是人工书写的 kExpectedLanguageRanges 覆盖不到的那一类错误。
  constexpr std::array<std::uint32_t, 6> kPythonDerivedClassCounts{
      1073601U,  // Unknown
      28099U,    // HAN
      264U,      // JA
      11440U,    // KO
      452U,      // LATN
      256U,      // CYRL
  };
  constexpr std::uint64_t kPythonDerivedClassChecksum = 0xD6305DAF2049E300ULL;

  std::array<std::uint32_t, 6> counts{};
  std::uint64_t checksum = 0xCBF29CE484222325ULL;  // FNV-1a 64 offset basis
  for (std::uint32_t codePoint = 0; codePoint <= 0x10FFFFU; ++codePoint) {
    const std::size_t index = lyricLanguageIndex(classOfCodePoint(codePoint));
    counts[index] += 1;
    checksum = (checksum ^ index) * 0x100000001B3ULL;  // FNV-1a 64 prime
  }
  CHECK(counts == kPythonDerivedClassCounts);
  CHECK(checksum == kPythonDerivedClassChecksum);
}

TEST_CASE("lyric split splitLyricLine is implemented") {
  // 原占位用例（todo 4 骨架）断言「永不切分」；真实实现落地后该断言必红，故改写为
  // 断言真实行为：空输入走 empty，显式 ` / ` 走 strong。
  checkSplit(splitLyricLine("", LyricSplitConvention::None, "zh"),
             ExpectedSplit{.original = "", .translation = "",
                           .convention = LyricSplitConvention::None, .confidence = "none",
                           .reason = "empty"});
  checkSplit(splitLyricLine("原文 / 译文", LyricSplitConvention::StrongSlashSpaced, "zh"),
             ExpectedSplit{.original = "原文", .translation = "译文",
                           .convention = LyricSplitConvention::StrongSlashSpaced,
                           .confidence = "high", .reason = "strong"});

  CHECK(kLyricSplitAlgoVersion == "1");
}

// ── todo 8：splitLyricLine 的 route / reason 编排（逐字复刻参考实现 :482-587）─────
// 期望值全部由参考实现实测产出后固化，生成脚本与原始输出见
// .omo/evidence/task-8-lyric-split-auto-detect/qa/。10 个 route/reason 各 ≥1 例，
// 每例同时断言五个字段。

TEST_CASE("lyric split covers all ten route and reason branches") {
  struct Case {
    std::string_view text;
    LyricSplitConvention convention;
    ExpectedSplit expected;
  };
  const Case cases[] = {
      // empty：空输入。
      {.text = "", .convention = LyricSplitConvention::None,
       .expected = {.original = "", .translation = "", .convention = LyricSplitConvention::None,
                    .confidence = "none", .reason = "empty"}},
      // qq-sentinel：`//` 整行哨兵（strip 后 == `//`），原文保留原样（含两侧空格）。
      {.text = " // ", .convention = LyricSplitConvention::None,
       .expected = {.original = " // ", .translation = "", .convention = LyricSplitConvention::None,
                    .confidence = "high", .reason = "qq-sentinel"}},
      // no-convention：约定 None 且行内无 ` / `（不留门也不硬切）。
      {.text = "原文没有分隔符", .convention = LyricSplitConvention::None,
       .expected = {.original = "原文没有分隔符", .translation = "",
                    .convention = LyricSplitConvention::None, .confidence = "none",
                    .reason = "no-convention"}},
      // strong：约定强分隔符，行内命中。
      {.text = "原文 / 译文", .convention = LyricSplitConvention::StrongSlashSpaced,
       .expected = {.original = "原文", .translation = "译文",
                    .convention = LyricSplitConvention::StrongSlashSpaced, .confidence = "high",
                    .reason = "strong"}},
      // strong-fallback：本约定的 ` / ` 候选全在括号内 ⇒ 改用括号外的其它强分隔符 `/`，
      // 此时 effectiveConvention 记的是【实际生效的 other】而非文件约定。
      {.text = "高架橋（こうかきょう） 雨（あめ）降（ふ）らす神様（かみさま / ）/高架橋 降雨之神",
       .convention = LyricSplitConvention::StrongSlashSpaced,
       .expected = {.original = "高架橋（こうかきょう） 雨（あめ）降（ふ）らす神様（かみさま / ）",
                    .translation = "高架橋 降雨之神", .convention = LyricSplitConvention::StrongSlash,
                    .confidence = "high", .reason = "strong-fallback"}},
      // strong-bracket-fallback：约定 `/`，其候选在括号内，无其它强分隔符可用 ⇒ 括号区起点
      // （trailingOnly），effectiveConvention 仍是文件约定。
      {.text = "おぼつかぬ足取り 「/略带动摇的步伐」", .convention = LyricSplitConvention::StrongSlash,
       .expected = {.original = "おぼつかぬ足取り", .translation = "「/略带动摇的步伐」",
                    .convention = LyricSplitConvention::StrongSlash, .confidence = "high",
                    .reason = "strong-bracket-fallback"}},
      // strong-weak-fallback：` / ` 在注音括号内、又无别的强分隔符/括号区 ⇒ 退到弱边界，
      // 仍然记文件约定。
      {.text = "蔷薇を想わせる绯色の『口红』(ローズレッドルージュ / )  令人联想起玫瑰的绯色口红",
       .convention = LyricSplitConvention::StrongSlashSpaced,
       .expected = {.original = "蔷薇を想わせる绯色の『口红』(ローズレッドルージュ / )",
                    .translation = "令人联想起玫瑰的绯色口红",
                    .convention = LyricSplitConvention::StrongSlashSpaced, .confidence = "high",
                    .reason = "strong-weak-fallback"}},
      // bracket：弱约定 + 行尾纯目标语言括号区 ⇒ 括号路径。
      {.text = "霞む夏の灯「朦胧的夏日灯火」", .convention = LyricSplitConvention::WeakSpace,
       .expected = {.original = "霞む夏の灯", .translation = "「朦胧的夏日灯火」",
                    .convention = LyricSplitConvention::WeakSpace, .confidence = "high",
                    .reason = "bracket"}},
      // weak：弱边界（空白），confidence 降为 medium。
      {.text = "君の名前 你的名字", .convention = LyricSplitConvention::WeakSpace,
       .expected = {.original = "君の名前", .translation = "你的名字",
                    .convention = LyricSplitConvention::WeakSpace, .confidence = "medium",
                    .reason = "weak"}},
      // validate-failed：多段并列（≥2 个可用分隔段）⇒ 整行作原文，且【不】回退到括号/空白。
      {.text = "（Afterburner） / 揺るぎない Spirit / （Afterburner） / 坚定不移的Spirit",
       .convention = LyricSplitConvention::StrongSlashSpaced,
       .expected = {.original = "（Afterburner） / 揺るぎない Spirit / （Afterburner） / 坚定不移的Spirit",
                    .translation = "", .convention = LyricSplitConvention::None,
                    .confidence = "none", .reason = "validate-failed"}},
  };
  std::set<std::string> coveredReasons;
  for (const auto& item : cases) {
    CAPTURE(item.text);
    checkSplit(splitLyricLine(item.text, item.convention, "zh"), item.expected);
    coveredReasons.insert(std::string{item.expected.reason});
  }
  // 10 个 route/reason 全覆盖（少于 10 说明有分支被漏测或被合并）。
  CHECK(coveredReasons.size() == 10);
}

TEST_CASE("lyric split named regression lines match the reference implementation") {
  // §4.3 计划点名的具体行（期望值由参考实现实测固化）。
  const std::string bareSlash = "花/開かせる花/花 / 花／绽放的花／花";
  const LyricSplitResult bare =
      splitLyricLine(bareSlash, LyricSplitConvention::StrongSlashSpaced, "zh");
  checkSplit(bare, ExpectedSplit{.original = "花/開かせる花/花", .translation = "花／绽放的花／花",
                                 .convention = LyricSplitConvention::StrongSlashSpaced,
                                 .confidence = "high", .reason = "strong"});
  // 原文内部的裸 `/` 【不】构成强分隔符：它必须原样留在原文里。
  CHECK(bare.original.find('/') != std::string::npos);

  // 两侧完全相同的行【照切】（D：作者写了 `A / A` 即确有此结构）；不得断言「整行作原文」。
  const std::string identical =
      "Shake up Tonight(シェイカップトゥナイトゥ) / Shake up Tonight(シェイカップトゥナイトゥ)";
  const std::string identicalSide = "Shake up Tonight(シェイカップトゥナイトゥ)";
  checkSplit(splitLyricLine(identical, LyricSplitConvention::StrongSlashSpaced, "zh"),
             ExpectedSplit{.original = identicalSide, .translation = identicalSide,
                           .convention = LyricSplitConvention::StrongSlashSpaced,
                           .confidence = "high", .reason = "strong"});

  // 塔语 + 日文括注完整保留在原文（括注里的 `/` 不是分隔符，` / ` 之前的整段都是原文）。
  const std::string tower =
      "jAzLYEtN LYAsiance/.（みんなと理想を繋ぐ詩を謳います） / 我将咏唱一首连接大家与理想的诗";
  const LyricSplitResult towerResult =
      splitLyricLine(tower, LyricSplitConvention::StrongSlashSpaced, "zh");
  checkSplit(towerResult,
             ExpectedSplit{.original = "jAzLYEtN LYAsiance/.（みんなと理想を繋ぐ詩を謳います）",
                           .translation = "我将咏唱一首连接大家与理想的诗",
                           .convention = LyricSplitConvention::StrongSlashSpaced,
                           .confidence = "high", .reason = "strong"});
  CHECK(towerResult.original.find("（みんなと理想を繋ぐ詩を謳います）") != std::string::npos);

  // 全角空格（U+3000）不作分界：原文里的 `土　肌` 必须原样保留。
  const LyricSplitResult fullwidth =
      splitLyricLine("土　肌を見せてなお / 大地啊，即便裸露着肌肤",
                     LyricSplitConvention::StrongSlashSpaced, "zh");
  checkSplit(fullwidth, ExpectedSplit{.original = "土　肌を見せてなお",
                                      .translation = "大地啊，即便裸露着肌肤",
                                      .convention = LyricSplitConvention::StrongSlashSpaced,
                                      .confidence = "high", .reason = "strong"});
  CHECK(fullwidth.original.find("\u3000") != std::string::npos);

  // 完全粘连（无任何分隔符）⇒ 整行作原文。
  checkSplit(splitLyricLine("日文中文", LyricSplitConvention::StrongSlashSpaced, "zh"),
             ExpectedSplit{.original = "日文中文", .translation = "",
                           .convention = LyricSplitConvention::None, .confidence = "none",
                           .reason = "validate-failed"});
}

TEST_CASE("lyric split convention input changes the result (four distinct reasons)") {
  // 同一行在四种约定下给出四种不同结果 —— 按【含 reason】的口径计（只比 original/translation
  // 时 no-convention 与 validate-failed 相同，只有三种）。
  const std::string line = "悔しいけど好きって純情 虽然不甘但还是喜欢你 这份纯情";

  const LyricSplitResult weak = splitLyricLine(line, LyricSplitConvention::WeakSpace, "zh");
  checkSplit(weak, ExpectedSplit{.original = "悔しいけど好きって純情",
                                 .translation = "虽然不甘但还是喜欢你 这份纯情",
                                 .convention = LyricSplitConvention::WeakSpace,
                                 .confidence = "medium", .reason = "weak"});

  const LyricSplitResult none = splitLyricLine(line, LyricSplitConvention::None, "zh");
  checkSplit(none, ExpectedSplit{.original = line, .translation = "",
                                 .convention = LyricSplitConvention::None, .confidence = "none",
                                 .reason = "no-convention"});

  const LyricSplitResult script =
      splitLyricLine(line, LyricSplitConvention::ScriptTransition, "zh");
  checkSplit(script, ExpectedSplit{.original = "悔しいけど好きって",
                                   .translation = "純情 虽然不甘但还是喜欢你 这份纯情",
                                   .convention = LyricSplitConvention::ScriptTransition,
                                   .confidence = "medium", .reason = "weak"});

  const LyricSplitResult strong =
      splitLyricLine(line, LyricSplitConvention::StrongSlashSpaced, "zh");
  checkSplit(strong, ExpectedSplit{.original = line, .translation = "",
                                   .convention = LyricSplitConvention::None, .confidence = "none",
                                   .reason = "validate-failed"});

  // 四种【不同结果】：只比 (original, translation) 时 no-convention 与 validate-failed 相同、
  // 只有三种；必须把 reason 计入，四元组才两两不同。
  const auto keyOf = [](const LyricSplitResult& result) {
    return result.original + "\x1f" + result.translation + "\x1f" + result.reason;
  };
  const std::set<std::string> distinct{keyOf(weak), keyOf(none), keyOf(script), keyOf(strong)};
  CHECK(distinct.size() == 4);
}

TEST_CASE("lyric split entry point is a pure function") {
  struct Input {
    std::string_view text;
    LyricSplitConvention convention;
    std::string_view target;
  };
  const Input inputs[] = {
      {.text = "原文 / 译文", .convention = LyricSplitConvention::StrongSlashSpaced, .target = "zh"},
      {.text = "君の名前 你的名字", .convention = LyricSplitConvention::WeakSpace, .target = "zh"},
      {.text = "霞む夏の灯「朦胧的夏日灯火」", .convention = LyricSplitConvention::WeakSpace,
       .target = "zh"},
      {.text = "日文中文", .convention = LyricSplitConvention::StrongSlashSpaced, .target = "zh"},
      {.text = "", .convention = LyricSplitConvention::None, .target = "zh"},
  };
  for (const auto& item : inputs) {
    const LyricSplitResult first = splitLyricLine(item.text, item.convention, item.target);
    const LyricSplitResult second = splitLyricLine(item.text, item.convention, item.target);
    checkSplit(second, ExpectedSplit{.original = first.original,
                                     .translation = first.translation,
                                     .convention = first.effectiveConvention,
                                     .confidence = first.confidence, .reason = first.reason});
  }
}

TEST_CASE("lyric split inferSplitConvention votes keep first-vote order") {
  // S4-1：votes 的输出顺序 = 各 kind 【首次得票】顺序，不是 kConventions 顺序。
  // `['a｜b','a / b','a / b']` 的首票是 `｜`（先于 ` / ` 出现），故 votes 顺序必须是
  // ['｜', ' / ']；若实现改回「按 kConventions 顺序输出」，本用例即失败。
  const LyricSplitConventionVotes inferred =
      inferSplitConvention({"a｜b", "a / b", "a / b"}, "zh");
  CHECK(inferred.convention == LyricSplitConvention::StrongSlashSpaced);
  REQUIRE(inferred.votes.size() == 2);
  CHECK(inferred.votes[0].first == LyricSplitConvention::StrongFullwidthBar);
  CHECK(inferred.votes[0].second == 1);
  CHECK(inferred.votes[1].first == LyricSplitConvention::StrongSlashSpaced);
  CHECK(inferred.votes[1].second == 2);

  // 首个得票 kind 不是 kConventions[0] 的第二个例子（zh 夹具的实际投票）：
  // 首票是 ` `（弱边界），随后才是 SCRIPT 与 ` / `。
  const LyricSplitConventionVotes fixtureVotes = inferSplitConvention(
      {"君の名前 你的名字", "夢の中で 在梦里", "傷ついても前を向く即使受伤也要向前",
       "誰かの声 / 某人的声音"},
      "zh");
  CHECK(fixtureVotes.convention == LyricSplitConvention::WeakSpace);
  REQUIRE(fixtureVotes.votes.size() == 3);
  CHECK(fixtureVotes.votes[0].first == LyricSplitConvention::WeakSpace);
  CHECK(fixtureVotes.votes[0].second == 2);
  CHECK(fixtureVotes.votes[1].first == LyricSplitConvention::ScriptTransition);
  CHECK(fixtureVotes.votes[2].first == LyricSplitConvention::StrongSlashSpaced);
}

// ── todo 5：候选枚举 / 括号区 / 边缘剥离 / 两套验证原语 ────────────────────────
// 所有期望值都由 Python 参考实现【实测产出后固化】，不是手写推断；原始输出见
// .omo/evidence/task-5-lyric-split-auto-detect/qa/probe_expected_values{,_round2}.out.txt。
// 参考实现用码点下标，本实现用字节偏移；每处多字节样例都注明「码点值 → 字节值」。

TEST_CASE("candidatePositions returns byte offsets for separators and SCRIPT transitions") {
  // '原文 /  / 译文' 的 ' / ' 码点候选 = [5, 8]（匹配起点 2、5 各 +3）。原/文 各 3B
  // ⇒ 码点 5 落在字节 9、码点 8 落在字节 12。断言的必须是字节口径 [9, 12]。
  CHECK(candidatePositions("原文 /  / 译文", " / ") == std::vector<std::size_t>{9, 12});
  // 半角 '/' 码点 [4, 7] ⇒ 字节 [8, 11]。
  CHECK(candidatePositions("原文 /  / 译文", "/") == std::vector<std::size_t>{8, 11});
  // 弱边界 ' ' 码点 [3, 5, 6, 8] ⇒ 字节 [7, 9, 10, 12]。
  CHECK(candidatePositions("原文 /  / 译文", " ") == std::vector<std::size_t>{7, 9, 10, 12});
  CHECK(candidatePositions("原文 /  / 译文", "\t").empty());
  CHECK(candidatePositions("原文 /  / 译文", "　").empty());
  CHECK(candidatePositions("原文 /  / 译文", "nope").empty());

  // SCRIPT：仅「非目标决定性类 → 汉字」的过渡（HARD-CODED HAN，与 target 无关）。
  // 'a中bあ漢'：码点 [1, 4] ⇒ 字节 [1, 8]（a=1B、中=3B、b=1B ⇒ あ 起字节 5、漢 起字节 8）。
  CHECK(candidatePositions("a中bあ漢", "SCRIPT") == std::vector<std::size_t>{1, 8});
  // '日a本語中'：日 不是决定性类 ⇒ 只有 a(LATN)→本(HAN) 那一处，码点 2 ⇒ 字节 4。
  CHECK(candidatePositions("日a本語中", "SCRIPT") == std::vector<std::size_t>{4});
  CHECK(candidatePositions("あaい中", "SCRIPT") == std::vector<std::size_t>{7});
  CHECK(candidatePositions("abc", "SCRIPT").empty());
  CHECK(candidatePositions("中中", "SCRIPT").empty());
}

TEST_CASE("bracketRegions returns outermost starts right-to-left with byte offsets") {
  // 真嵌套：「a（b）c」 只有最外层起点（Python 实测 [0]）。若实现返回 [0, 2] 即挂。
  CHECK(bracketRegions("「a（b）c」") == std::vector<std::size_t>{0});
  // 并列的四个最外层区：Python 码点 [25, 18, 13, 3] ⇒ 字节 [73, 52, 37, 9]。
  // 本例含多字节字符，故两套坐标不同 —— 直接抄码点值会让字面正确的实现失败。
  const std::string mixed =
      "高架橋（こうかきょう） 雨（あめ）降（ふ）らす神様（かみさま / ）/高架橋 降雨之神";
  CHECK(bracketRegions(mixed) == std::vector<std::size_t>{73, 52, 37, 9});
  // 错配闭括号：忽略之，不产生越界或错位。
  CHECK(bracketRegions("（abc").empty());
  CHECK(bracketRegions("abc）").empty());
  CHECK(bracketRegions("）a（b").empty());
}

TEST_CASE("bracketSpan returns the exclusive byte end of a matched outermost region") {
  // 「a（b）c」 码点结束 7 ⇒ 字节 15（「3 + a1 + （3 + b1 + ）3 + c1 + 」3）。
  CHECK(bracketSpan("「a（b）c」", 0) == std::size_t{15});
  CHECK(bracketSpan("（abc", 0) == std::nullopt);
  CHECK(bracketSpan("abc", 0) == std::nullopt);
  // start 不是开括号时：闭括号在空栈上被忽略 ⇒ 无配对。
  CHECK(bracketSpan("「a」", 1) == std::nullopt);

  const std::string mixed =
      "高架橋（こうかきょう） 雨（あめ）降（ふ）らす神様（かみさま / ）/高架橋 降雨之神";
  CHECK(bracketSpan(mixed, 9) == std::size_t{33});   // 「こうかきょう」 cp11 ⇒ 33B
  CHECK(bracketSpan(mixed, 37) == std::size_t{49});  // 「あめ」       cp17 ⇒ 49B
  CHECK(bracketSpan(mixed, 52) == std::size_t{61});  // 「ふ」         cp21 ⇒ 61B
  CHECK(bracketSpan(mixed, 73) == std::size_t{94});  // 「かみさま / 」 cp34 ⇒ 94B
}

TEST_CASE("trimEdges strips only the given kind and alternates until stable") {
  // cut 的出处：Python `_cut_explicit` 只返回 (左,右)、不返回 cut；真正的 cut 取
  // `_separator_runs(text, candidate_positions(text,' / '),' / ')` 的段起点 = 码点 2。
  // 码点 2 ⇒ 字节 6（原/文 各 3B）。一次性剥离会停在 '原文 /'（残留一根分隔符），
  // 只有「分隔符与空白交替剥到稳定」才得到干净的 ('原文', '译文')。
  const auto spaced = trimEdges("原文 /  / 译文", 6, std::string_view{" / "});
  CHECK(spaced.first == "原文");
  CHECK(spaced.second == "译文");
  // kind 为空（nullopt）时改剥 SEPARATOR_TRAIL_CHARS，本例结果相同。
  const auto implicit = trimEdges("原文 /  / 译文", 6, std::nullopt);
  CHECK(implicit.first == "原文");
  CHECK(implicit.second == "译文");

  // 内容里的 '／' 不得被删：Python 码点 cut=7 ⇒ 字节 21（7 个码点各 3B）。
  const auto titled = trimEdges("＼なので～す／ / ＼的~说／", 21, std::string_view{" / "});
  CHECK(titled.first == "＼なので～す／");
  CHECK(titled.second == "＼的~说／");
}

TEST_CASE("validateCut and validateCutPure differ on a latin-internal space") {
  const std::string mixed = "We know...someday.\\我们知道。";
  // 拉丁原文内部的空格：码点 2 ⇒ 字节 2（前缀全 ASCII）。宽松版通过。
  const auto loose = validateCut(mixed, 2, "zh");
  REQUIRE(loose.has_value());
  CHECK(loose->first == "We");
  CHECK(loose->second == "know...someday.\\我们知道。");
  // 严格版拒绝：右侧第一个可分类字符是 'k'（拉丁），不是目标类 HAN。
  CHECK_FALSE(validateCutPure(mixed, 2, "zh").has_value());
  // 反向对照：真正的切分点（反斜杠之后的「我」）两套都通过。
  const auto pure = validateCutPure(mixed, 28, "zh");
  REQUIRE(pure.has_value());
  CHECK(pure->first == "We know...someday.\\我们知");
  CHECK(pure->second == "道。");
}

TEST_CASE("validateCutPure blocks JA/KO/CYRL on the right but deliberately not latin") {
  // 西里尔：validateCut 通过（右侧无 JA/KO 且含 HAN），纯版被 MIXED_SCRIPT_BLOCKERS 拒。
  CHECK(validateCut("あの日 / 那天П", 12, "zh").has_value());
  CHECK_FALSE(validateCutPure("あの日 / 那天П", 12, "zh").has_value());
  // 拉丁【刻意】不在阻塞集（译文夹英文/数字极常见）⇒ 两套都通过。
  // 这也是 QA failure 场景钉住的性质：把 LATN 加进 MIXED_SCRIPT_BLOCKERS 会让本条变红。
  CHECK(validateCutPure("あの日 / 那天Return", 12, "zh").has_value());
}

TEST_CASE("validateCutPure skips unclassifiable leading characters when finding the head") {
  // 右侧以「（无类）开头，跳过它后第一个可分类字符是 那（HAN）⇒ 通过。
  CHECK(validateCutPure("あの日 / 「那天」", 12, "zh").has_value());
  // 跳过「后是 'a'（拉丁）⇒ 拒绝（证明 head 判据在扫描，而不是只看第一个码点）。
  CHECK_FALSE(validateCutPure("あ / 「abc那天」", 6, "zh").has_value());
  // 同一输入宽松版放行（右侧含 HAN、左侧有 JA 决定性证据）。
  CHECK(validateCut("あ / 「abc那天」", 6, "zh").has_value());
}

TEST_CASE("validateCutPure guarded default rejects cuts inside a matched bracket pair") {
  // 'あ（い 那天）'：弱边界候选 码点 4 ⇒ 字节 10，落在（…）内部。
  const std::string text = "あ（い 那天）";
  CHECK(cutInsideBracket(text, 10));
  CHECK(validateCut(text, 10, "zh").has_value());
  CHECK_FALSE(validateCutPure(text, 10, "zh").has_value());   // guarded 默认 true
  CHECK(validateCutPure(text, 10, "zh", false).has_value());  // 显式关闭守卫才放行
}

TEST_CASE("cutInsideBracket is a strict-interior predicate") {
  // '（くも / ）'：候选 码点 6 ⇒ 字节 12；括号区 = [0, 15)。
  CHECK(cutInsideBracket("（くも / ）", 12));
  CHECK_FALSE(cutInsideBracket("（くも / ）", 0));   // start 不算内部（严格 start < cut）
  CHECK_FALSE(cutInsideBracket("（くも / ）", 15));  // end 不算内部（严格 cut < end）
  // 错配括号不产生任何「内部」。
  CHECK_FALSE(cutInsideBracket("（abc", 2));
  CHECK_FALSE(cutInsideBracket("abc）", 2));
  // 「a（b）c」：内层（b）不是最外层区，切点 1 落在最外层「」内部。
  CHECK(cutInsideBracket("「a（b）c」", 1));
}

TEST_CASE("validation rejects untargeted languages and same-script pairs") {
  // target 不在 TARGET_CLASS 表里 ⇒ 两套一致返回 nullopt。
  CHECK_FALSE(validateCut("あの日 / 那天", 12, "fr").has_value());
  CHECK_FALSE(validateCutPure("あの日 / 那天", 12, "fr").has_value());
  // 两侧都是汉字（原文与译文同类）⇒ 左侧没有「非目标决定性类」证据 ⇒ 拒绝。
  CHECK_FALSE(validateCut("原文 / 译文", 7, "zh").has_value());
  // 日文（JA）在右侧 ⇒ validateCut 直接拒。
  CHECK_FALSE(validateCut("あの日 / 那あ天", 12, "zh").has_value());
}

TEST_CASE("trimEdges and cleanLine honour Python Unicode whitespace (U+3000)") {
  // U+3000 是 Python 空白，紧邻分隔符时须被剥掉（ASCII 空白实现会残留）。
  // '原文　 / 译文'：Python 候选(' / ') 码点 6 ⇒ 字节 12（原文 6B + U+3000 3B + ' ' 1B + '/' 1B + ' ' 1B）。
  const auto wide = trimEdges("原文　 / 译文", 12, std::nullopt);
  CHECK(wide.first == "原文");
  CHECK(wide.second == "译文");
  // '原文 / 　译文'：Python 候选 码点 5 ⇒ 字节 9。
  const auto wide2 = trimEdges("原文 / 　译文", 9, std::nullopt);
  CHECK(wide2.first == "原文");
  CHECK(wide2.second == "译文");

  // cleanLine：中间 U+3000 保留（不是行首尾），行首尾 U+3000 剥掉。
  CHECK(cleanLine("原文　 / 译文").value() == "原文　 / 译文");
  CHECK(cleanLine("原文 / 　译文").value() == "原文 / 　译文");
  CHECK(cleanLine("　原文　").value() == "原文");
  CHECK_FALSE(cleanLine("　").has_value());
  // U+3000 不是 ASCII 空白：它出现在 '词' 与 '/' 之间时不会被当作分隔符两侧空白剥离，
  // 但因为它是 Python 空白，validateCut 的 rstrip/lstrip 仍会把它剥净。
  CHECK_FALSE(validateCut("原文　 / 译文", 12, "zh").has_value());
}

TEST_CASE("invalid UTF-8 degrades to U+FFFD with a one byte advance") {
  // notepad 遗留项：decodeUtf8At 原实现接受 overlong/代理/超界序列，与函数头注释不符。
  // 这些序列若被接受会解出【可分类】码点，用 profileOf / candidatePositions 即可观测。
  constexpr std::size_t kLatin = lyricLanguageIndex(LyricLanguage::Latin);
  // overlong 的 C1 81 旧行为解出 U+0041（拉丁 1 个）⇒ 修复后必须是 0 个拉丁。
  CHECK(profileOf("\xc1\x81")[kLatin] == 0);
  // 与后面合法的 'A' 合看只剩 1 个拉丁，证明 C1 81 不再冒充 'A'。
  CHECK(profileOf("\xc1\x81" "A")[kLatin] == 1);
  // SCRIPT 过渡同样被钉住：旧行为里 C1 81→拉丁 会产生「拉丁→中」的候选过渡。
  CHECK(candidatePositions("\xc1\x81" "中", "SCRIPT").empty());
  // overlong 的 E0 80 A0 旧行为解出 U+0020（Python 空白）⇒ cleanLine 会剥掉它；
  // 修复后它是无类的 U+FFFD，不得被当空白。
  const auto overlongSpace = cleanLine("\xe0\x80\xa0" "x");
  REQUIRE(overlongSpace.has_value());
  CHECK(*overlongSpace != "x");
}

// ── todo 6：三条切分路径（显式 / 脚本 / 括号）与结果记入 ──────────────────────
// 期望值全部由 Python 参考实现实测产出后固化；每个多字节样例都在 evidence 的
// codepoint-to-byte-proof.md 里给出「码点下标 → UTF-8 字节偏移」换算。参考实现是
// 码点口径，本实现一律字节口径，直接抄码点值会让字面正确的实现失败。
// 分工：`splitLine`（含 strong-bracket-fallback 等 route 编排）是 todo 8，本 todo
// 只断言 cutExplicit / cutScript / cutBracket / findAnyBracket / separatorRuns / accept。

TEST_CASE("separatorRuns merges only gaps that are separator-and-whitespace") {
  // '言葉 / 这句话'（8 码点 / 18B）：候选 cp [5] ⇒ 字节 [9]；段 cp [(2,5)] ⇒ 字节 [(6,9)]。
  CHECK(separatorRuns("言葉 / 这句话", {9}, " / ") ==
        (std::vector<std::pair<std::size_t, std::size_t>>{{6, 9}}));
  // '原文 /  / 译文'（10 码点 / 18B）：候选 cp [5,8] ⇒ 字节 [9,12]；中间只剩 ' / ' 与空白
  // ⇒ 空段合并，段 cp [(2,8)] ⇒ 字节 [(6,12)]（notepad 第 57 条换算，本例复验）。
  CHECK(separatorRuns("原文 /  / 译文", {9, 12}, " / ") ==
        (std::vector<std::pair<std::size_t, std::size_t>>{{6, 12}}));
  // 'Rrha num wa ene revm /  / 我做了一个梦'（32 码点 / 44B，前 20 码点全 ASCII）：
  // 候选 cp [23,26] ⇒ 字节 [23,26]；段 cp [(20,26)] ⇒ 字节 [(20,26)]。
  CHECK(separatorRuns("Rrha num wa ene revm /  / 我做了一个梦", {23, 26}, " / ") ==
        (std::vector<std::pair<std::size_t, std::size_t>>{{20, 26}}));
  // 每段都有内容 ⇒ 三段并列**不得合并**（D32）：候选 cp [16,31,47] ⇒ 字节 [20,45,65]；
  // 段 cp [(13,16),(28,31),(44,47)] ⇒ 字节 [(17,20),(42,45),(62,65)]。
  const std::string afterburner =
      "（Afterburner） / 揺るぎない Spirit / （Afterburner） / 坚定不移的Spirit";
  CHECK(separatorRuns(afterburner, {20, 45, 65}, " / ") ==
        (std::vector<std::pair<std::size_t, std::size_t>>{{17, 20}, {42, 45}, {62, 65}}));
  // 段 end 吃掉后续 Python 空白：'A /  ' 的候选 cp 4 ⇒ 字节 4；段 (1,4) 再吃一个空格 ⇒ (1,5)。
  CHECK(separatorRuns("A /  ", {4}, " / ") ==
        (std::vector<std::pair<std::size_t, std::size_t>>{{1, 5}}));
  // 空 positions ⇒ 空（不得崩、不得造段）。
  CHECK(separatorRuns("abc", std::vector<std::size_t>{}, " / ").empty());
  CHECK(separatorRuns("", std::vector<std::size_t>{}, " / ").empty());
}

TEST_CASE("isTargetBracket distinguishes translation brackets from phonetic ones") {
  // （くも / ）：含假名 JA ⇒ 是日文注音，不是译文容器（括号区 [0,15)）。
  CHECK_FALSE(isTargetBracket("（くも / ）", 0, "zh"));
  // 「I Miss You 一刻也停不下来」：含 HAN 且不含 JA/KO/CYRL ⇒ 是译文（H 占比仅 0.47 也照收）。
  CHECK(isTargetBracket("「I Miss You 一刻也停不下来」", 0, "zh"));
  // 纯假名注音括号 ⇒ false；纯汉字标点括号 ⇒ true。
  CHECK_FALSE(isTargetBracket("(君ひとりでは手に余る重圧)", 0, "zh"));
  CHECK(isTargetBracket("『日本語』", 0, "zh"));
  // 括号区不含目标语言类（纯拉丁）⇒ false。
  CHECK_FALSE(isTargetBracket("「hello」", 0, "zh"));
  CHECK_FALSE(isTargetBracket("「abc」", 0, "zh"));
  // start 不是开括号 / 无配对 ⇒ false。
  CHECK_FALSE(isTargetBracket("abc", 0, "zh"));
  CHECK_FALSE(isTargetBracket("「abc", 0, "zh"));
}

TEST_CASE("cutBracket and findAnyBracket cut only translation brackets") {
  // 高架橋例：四个最外层括号区按右→左为字节 [73,52,37,9]（todo 5 已钉换算）；每个
  // 括号区内容都含假名 ⇒ isTargetBracket 全 false ⇒ 两个函数都无切点。
  const std::string mixed =
      "高架橋（こうかきょう） 雨（あめ）降（ふ）らす神様（かみさま / ）/高架橋 降雨之神";
  for (const std::size_t start : bracketRegions(mixed)) {
    CHECK_FALSE(isTargetBracket(mixed, start, "zh"));
    CHECK_FALSE(cutBracket(mixed, start, "zh").has_value());
  }
  CHECK_FALSE(findAnyBracket(mixed, "zh", true).has_value());
  CHECK_FALSE(findAnyBracket(mixed, "zh", false).has_value());

  // 'あの日 「那天」'：括号起点 cp 4 ⇒ 字节 10；括号内容就是译文 ⇒ cutBracket 切出。
  const auto bracketed = cutBracket("あの日 「那天」", 10, "zh");
  REQUIRE(bracketed.has_value());
  CHECK(bracketed->first == "あの日");
  CHECK(bracketed->second == "「那天」");
  // 括号区延伸到行尾 ⇒ trailingOnly 与 false 同结果。
  CHECK(findAnyBracket("あの日 「那天」", "zh", true) == bracketed);
  CHECK(findAnyBracket("あの日 「那天」", "zh", false) == bracketed);

  // 'あの日（かみさま）「那天」'：最外层区 cp [9,3] ⇒ 字节 [27,9]；右→左先试字节 27
  // 的「那天」（译文容器），注音括号（字节 9）不是 ⇒ 原文含注音括号、译文是「那天」。
  CHECK_FALSE(cutBracket("あの日（かみさま）「那天」", 9, "zh").has_value());
  const auto inner = findAnyBracket("あの日（かみさま）「那天」", "zh", false);
  REQUIRE(inner.has_value());
  CHECK(inner->first == "あの日（かみさま）");
  CHECK(inner->second == "「那天」");
}

TEST_CASE("accept records route verbatim with weak downgraded to medium confidence") {
  const LyricSplitResult weak = accept("a", "b", LyricSplitConvention::StrongSlashSpaced, "weak");
  CHECK(weak.original == "a");
  CHECK(weak.translation == "b");
  CHECK(weak.effectiveConvention == LyricSplitConvention::StrongSlashSpaced);
  CHECK(weak.confidence == "medium");
  CHECK(weak.reason == "weak");

  const LyricSplitResult strong = accept("あ", "中", LyricSplitConvention::StrongSlash, "strong");
  CHECK(strong.original == "あ");
  CHECK(strong.translation == "中");
  CHECK(strong.effectiveConvention == LyricSplitConvention::StrongSlash);
  CHECK(strong.confidence == "high");
  CHECK(strong.reason == "strong");

  // 只有 weak 降级为 medium；其余 route 一律 high（route 原样记入 reason）。
  CHECK(accept("a", "b", LyricSplitConvention::None, "bracket").confidence == "high");
  CHECK(accept("a", "b", LyricSplitConvention::None, "strong-fallback").confidence == "high");
  CHECK(accept("a", "b", LyricSplitConvention::None, "strong-bracket-fallback").confidence ==
        "high");
  // 原文与译文可以相同（作者写了 A / A 就说明该行确有此结构，D30 不要求脚本证据）。
  const LyricSplitResult same = accept("A", "A", LyricSplitConvention::StrongSlashSpaced, "strong");
  CHECK(same.original == same.translation);
}

// ↓↓↓ 由 qa/gen_expectation_table.py 机器派生（勿手改）；原始输出见同名 .out.cpp ↓↓↓
TEST_CASE("cutExplicit matches python reference (machine-generated table)") {
  struct ExplicitCase {
    std::string_view input;
    std::string_view kind;
    bool guarded;
    std::optional<std::pair<std::string, std::string>> expected;
  };
  const ExplicitCase cases[] = {
      {"\u8a00\u8449 / \u8fd9\u53e5\u8bdd", " / ", true, std::make_pair(std::string{"\u8a00\u8449"}, std::string{"\u8fd9\u53e5\u8bdd"})},
      {"\u8a00\u8449 / \u8fd9\u53e5\u8bdd", " / ", false, std::make_pair(std::string{"\u8a00\u8449"}, std::string{"\u8fd9\u53e5\u8bdd"})},
      {"Rrha num wa ene revm /  / \u6211\u505a\u4e86\u4e00\u4e2a\u68a6", " / ", true, std::make_pair(std::string{"Rrha num wa ene revm"}, std::string{"\u6211\u505a\u4e86\u4e00\u4e2a\u68a6"})},
      {"/ [\u6211\u4eec\u547c\u5e94\u795e\u5b50] / [\u559c\u60a6\u632f\u594b\u5730\u5316\u4e3a\u8bd7] / [xxx]", " / ", true, std::nullopt},
      {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", " / ", true, std::nullopt},
      {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", "\uff5c", true, std::nullopt},
      {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", "|", true, std::nullopt},
      {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", "\uff0f", true, std::nullopt},
      {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", "/", true, std::nullopt},
      {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", "\\", true, std::nullopt},
      {"\u304a\u307c\u3064\u304b\u306c\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "/", true, std::nullopt},
      {"\u304a\u307c\u3064\u304b\u306c\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "/", false, std::make_pair(std::string{"\u304a\u307c\u3064\u304b\u306c\u8db3\u53d6\u308a \u300c"}, std::string{"\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d"})},
      {"\u539f\u6587 / \u8bd1\u6587", " / ", true, std::make_pair(std::string{"\u539f\u6587"}, std::string{"\u8bd1\u6587"})},
      {"\u540c\u3058 / \u540c\u3058", " / ", true, std::make_pair(std::string{"\u540c\u3058"}, std::string{"\u540c\u3058"})},
      {"A / A", " / ", true, std::make_pair(std::string{"A"}, std::string{"A"})},
      {"\u539f\u6587 /  / \u8bd1\u6587", " / ", true, std::make_pair(std::string{"\u539f\u6587"}, std::string{"\u8bd1\u6587"})},
      {"\uff3c\u306a\u306e\u3067\uff5e\u3059\uff0f / \uff3c\u7684~\u8bf4\uff0f", " / ", true, std::make_pair(std::string{"\uff3c\u306a\u306e\u3067\uff5e\u3059\uff0f"}, std::string{"\uff3c\u7684~\u8bf4\uff0f"})},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5 \u6210\u957f\u4e5f\u53ef\u4ee5", "\\", true, std::make_pair(std::string{"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044"}, std::string{"\u56de\u9996\u4e5f\u53ef\u4ee5 \u6210\u957f\u4e5f\u53ef\u4ee5"})},
      {"\u539f\u6587 / \\t\u4eba\u751f\u4e2d \u5927\u5bb6\u90fd\u611a\u8822\u5730\u53ef\u7231", " / ", true, std::make_pair(std::string{"\u539f\u6587"}, std::string{"\\t\u4eba\u751f\u4e2d \u5927\u5bb6\u90fd\u611a\u8822\u5730\u53ef\u7231"})},
      {"\u2605\uff0f / \\\u653e\u9a6c\u8fc7\u6765\uff01\u2026/", " / ", true, std::make_pair(std::string{"\u2605\uff0f"}, std::string{"\\\u653e\u9a6c\u8fc7\u6765\uff01\u2026/"})},
      {"\u539f\u6587 /  \\t\u4eba\u751f\u4e2d", " / ", true, std::make_pair(std::string{"\u539f\u6587"}, std::string{"\\t\u4eba\u751f\u4e2d"})},
      {"a\\tb / c", " / ", true, std::make_pair(std::string{"a\\tb"}, std::string{"c"})},
  };
  for (const auto& item : cases) {
    CAPTURE(item.input);
    CAPTURE(item.kind);
    const auto actual = cutExplicit(item.input, item.kind, item.guarded);
    REQUIRE(actual.has_value() == item.expected.has_value());
    if (item.expected.has_value()) {
      CHECK(actual->first == item.expected->first);
      CHECK(actual->second == item.expected->second);
    }
  }
}

TEST_CASE("cutScript matches python reference (machine-generated table)") {
  struct ScriptCase {
    std::string_view input;
    std::string_view target;
    bool guarded;
    std::optional<std::pair<std::string, std::string>> expected;
  };
  const ScriptCase cases[] = {
      {"We know...someday.\\\u6211\u4eec\u77e5\u9053\u3002", "zh", true, std::make_pair(std::string{"We know...someday.\\"}, std::string{"\u6211\u4eec\u77e5\u9053\u3002"})},
      {"We know...someday.\\\u6211\u4eec\u77e5\u9053\u3002", "zh", false, std::make_pair(std::string{"We know...someday.\\"}, std::string{"\u6211\u4eec\u77e5\u9053\u3002"})},
      {"abc\u4e2ddef\u6f22", "zh", true, std::make_pair(std::string{"abc\u4e2ddef"}, std::string{"\u6f22"})},
      {"\u3042a\u3044\u4e2d", "zh", true, std::make_pair(std::string{"\u3042a\u3044"}, std::string{"\u4e2d"})},
      {"\u65e5\u672c\u8a9e\u4e2d", "zh", true, std::nullopt},
      {"abc", "zh", true, std::nullopt},
      {"\u4e2d\u4e2d", "zh", true, std::nullopt},
      {"\u65e5a\u672c\u8a9e\u4e2d", "zh", true, std::make_pair(std::string{"\u65e5a"}, std::string{"\u672c\u8a9e\u4e2d"})},
      {"\u30e9\u30e9\u30e9\u4e2d \u6587", "zh", true, std::make_pair(std::string{"\u30e9\u30e9\u30e9"}, std::string{"\u4e2d \u6587"})},
  };
  for (const auto& item : cases) {
    CAPTURE(item.input);
    const auto actual = cutScript(item.input, item.target, item.guarded);
    REQUIRE(actual.has_value() == item.expected.has_value());
    if (item.expected.has_value()) {
      CHECK(actual->first == item.expected->first);
      CHECK(actual->second == item.expected->second);
    }
  }
}

TEST_CASE("findAnyBracket matches python reference (machine-generated table)") {
  struct BracketCase {
    std::string_view input;
    std::string_view target;
    bool trailingOnly;
    std::optional<std::pair<std::string, std::string>> expected;
  };
  const BracketCase cases[] = {
      {"\u3042\u306e\u65e5 \u300c\u90a3\u5929\u300d", "zh", true, std::make_pair(std::string{"\u3042\u306e\u65e5"}, std::string{"\u300c\u90a3\u5929\u300d"})},
      {"\u3042\u306e\u65e5 \u300c\u90a3\u5929\u300d", "zh", false, std::make_pair(std::string{"\u3042\u306e\u65e5"}, std::string{"\u300c\u90a3\u5929\u300d"})},
      {"\u3042\u306e\u65e5\uff08\u304b\u307f\u3055\u307e\uff09\u300c\u90a3\u5929\u300d", "zh", false, std::make_pair(std::string{"\u3042\u306e\u65e5\uff08\u304b\u307f\u3055\u307e\uff09"}, std::string{"\u300c\u90a3\u5929\u300d"})},
      {"\u9ad8\u67b6\u6a4b\uff08\u3053\u3046\u304b\u304d\u3087\u3046\uff09 \u96e8\uff08\u3042\u3081\uff09\u964d\uff08\u3075\uff09\u3089\u3059\u795e\u69d8\uff08\u304b\u307f\u3055\u307e / \uff09/\u9ad8\u67b6\u6a4b \u964d\u96e8\u4e4b\u795e", "zh", true, std::nullopt},
      {"\u9ad8\u67b6\u6a4b\uff08\u3053\u3046\u304b\u304d\u3087\u3046\uff09 \u96e8\uff08\u3042\u3081\uff09\u964d\uff08\u3075\uff09\u3089\u3059\u795e\u69d8\uff08\u304b\u307f\u3055\u307e / \uff09/\u9ad8\u67b6\u6a4b \u964d\u96e8\u4e4b\u795e", "zh", false, std::nullopt},
      {"\u300cI Miss You \u4e00\u523b\u4e5f\u505c\u4e0d\u4e0b\u6765\u300d", "zh", false, std::nullopt},
      {"abc", "zh", true, std::nullopt},
  };
  for (const auto& item : cases) {
    CAPTURE(item.input);
    const auto actual = findAnyBracket(item.input, item.target, item.trailingOnly);
    REQUIRE(actual.has_value() == item.expected.has_value());
    if (item.expected.has_value()) {
      CHECK(actual->first == item.expected->first);
      CHECK(actual->second == item.expected->second);
    }
  }
}
// ↑↑↑ 机器派生片段结束 ↑↑↑

// ── todo 7：acceptsVote / inferSplitConvention（歌曲级约定推断）────────────────
// 全部期望值由 Python 参考实现实测产出后固化（见下方机器派生表与证据目录）。

TEST_CASE("inferSplitConvention gates on both thresholds and refuses typographic spaces") {
  // 纯日文行的全角空格是排版间距：不得推出 WeakFullwidthSpace 或 WeakSpace 约定。
  const auto japanese = inferSplitConvention(
      std::vector<std::string>{"乾いた土にそっと　雨粒が流れてく"}, "zh");
  CHECK(japanese.convention == LyricSplitConvention::None);
  CHECK(japanese.votes.empty());

  // 某 kind 只拿 1 票（'｜' 1/10 = 10% < 40%）⇒ 不因它合格，整体仍取到 ' / '。
  const auto oneVote = inferSplitConvention(
      std::vector<std::string>{"a / b", "a / b", "a / b", "a / b", "a｜b",
                               "中文内容", "中文内容", "中文内容", "中文内容", "中文内容"},
      "zh");
  CHECK(oneVote.convention == LyricSplitConvention::StrongSlashSpaced);
  REQUIRE(oneVote.votes.size() == 2);
  CHECK(oneVote.votes[0].first == LyricSplitConvention::StrongSlashSpaced);
  CHECK(oneVote.votes[0].second == 4);
  CHECK(oneVote.votes[1].first == LyricSplitConvention::StrongFullwidthBar);
  CHECK(oneVote.votes[1].second == 1);

  // 门槛必须【同时】满足：2 票但占比 2/6 ≈ 33.3% < 40% ⇒ None（钉住占比门槛）。
  const auto ratioBelow = inferSplitConvention(
      std::vector<std::string>{"a / b", "a / b", "中文内容", "中文内容", "中文内容", "中文内容"},
      "zh");
  CHECK(ratioBelow.convention == LyricSplitConvention::None);
  // 占比 1/2 = 50% 达标但绝对数 < 2 ⇒ None（钉住绝对数门槛）。
  const auto countBelow =
      inferSplitConvention(std::vector<std::string>{"a / b", "中文内容"}, "zh");
  CHECK(countBelow.convention == LyricSplitConvention::None);

  // 只有【没有任何 kind 同时达标】时整体才 None。
  const auto noneAtAll =
      inferSplitConvention(std::vector<std::string>{"中文内容", "中文内容", "中文内容"}, "zh");
  CHECK(noneAtAll.convention == LyricSplitConvention::None);
  CHECK(noneAtAll.votes.empty());

  // total == 0 ⇒ {None, {}}。
  const auto empty = inferSplitConvention(std::vector<std::string>{}, "zh");
  CHECK(empty.convention == LyricSplitConvention::None);
  CHECK(empty.votes.empty());
}

TEST_CASE("acceptsVote decouples voting from the D20 bracket guard") {
  // 执行期（guarded=true）拒绝落在配对括号内部的切点 …
  CHECK_FALSE(cutExplicit("（くも / ）", " / ", /*guarded=*/true).has_value());
  CHECK_FALSE(cutExplicit("おぼつかなく足取り 「/略带动摇的步伐」", "/", /*guarded=*/true)
                  .has_value());
  // … 但计票期（acceptsVote 一律 guarded=false）仍把该行当作「作者用了这种写法」的证据。
  CHECK(acceptsVote("（くも / ）", " / ", "zh"));
  CHECK(acceptsVote("おぼつかなく足取り 「/略带动摇的步伐」", "/", "zh"));
  // 分隔符写在注音括号里的整首文件，约定仍能被推出（否则整首译文丢失）。
  const auto bracketFile = inferSplitConvention(
      std::vector<std::string>{"（くも / ）", "（くも / ）", "（くも / ）"}, "zh");
  CHECK(bracketFile.convention == LyricSplitConvention::StrongSlashSpaced);
  REQUIRE(bracketFile.votes.size() == 1);
  CHECK(bracketFile.votes[0].first == LyricSplitConvention::StrongSlashSpaced);
  CHECK(bracketFile.votes[0].second == 3);
}

TEST_CASE("acceptsVote dispatches strong / weak / SCRIPT branches") {
  // 强分隔符 ⇒ cutExplicit(guarded=false)。
  CHECK(acceptsVote("a / b", " / ", "zh"));
  CHECK_FALSE(acceptsVote("a /  / b / c", " / ", "zh"));  // 多段并列（D10）⇒ 拒绝
  CHECK(acceptsVote("背伸びしてもいい\\回首也可以", "\\", "zh"));
  CHECK_FALSE(acceptsVote("乾いた土にそっと　雨粒が流れてく", "\\", "zh"));

  // 弱边界 ⇒ candidatePositions 上任一 validateCutPure(guarded=false) 通过。
  CHECK(acceptsVote("abc 中文内容", " ", "zh"));
  CHECK_FALSE(acceptsVote("日文 中文", " ", "zh"));  // 左侧全是汉字 ⇒ 无决定性非目标类
  CHECK_FALSE(acceptsVote("a　b", "　", "zh"));       // 右侧无目标语言类
  CHECK_FALSE(acceptsVote("a\tb", "\t", "zh"));

  // SCRIPT ⇒ cutScript(guarded=false)，**绝不**塞进 cutExplicit/separatorRuns。
  CHECK(acceptsVote("abc中文内容", "SCRIPT", "zh"));
  CHECK(acceptsVote("We know...someday.\\我们知道。", "SCRIPT", "zh"));
  CHECK_FALSE(acceptsVote("中文内容", "SCRIPT", "zh"));
}

// ↓↓↓ 由 qa/gen_expectation_table.py 机器派生（勿手改）；原始输出见同名 .out.cpp ↓↓↓
TEST_CASE("acceptsVote matches python reference (machine-generated table)") {
  struct VoteCase {
    std::string_view input;
    std::string_view kind;
    bool expected;
  };
  const VoteCase cases[] = {
      {"\uff08\u304f\u3082 / \uff09", " / ", true},
      {"\uff08\u304f\u3082 / \uff09", "\uff5c", false},
      {"\uff08\u304f\u3082 / \uff09", "|", false},
      {"\uff08\u304f\u3082 / \uff09", "\uff0f", false},
      {"\uff08\u304f\u3082 / \uff09", "/", true},
      {"\uff08\u304f\u3082 / \uff09", "\\", false},
      {"\uff08\u304f\u3082 / \uff09", "\t", false},
      {"\uff08\u304f\u3082 / \uff09", "\u3000", false},
      {"\uff08\u304f\u3082 / \uff09", " ", false},
      {"\uff08\u304f\u3082 / \uff09", "SCRIPT", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", " / ", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "\uff5c", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "|", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "\uff0f", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "/", true},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "\\", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "\t", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "\u3000", false},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", " ", true},
      {"\u304a\u307c\u3064\u304b\u306a\u304f\u8db3\u53d6\u308a \u300c/\u7565\u5e26\u52a8\u6447\u7684\u6b65\u4f10\u300d", "SCRIPT", true},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", " / ", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "\uff5c", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "|", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "\uff0f", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "/", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "\\", true},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "\t", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "\u3000", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", " ", false},
      {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5", "SCRIPT", true},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", " / ", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "\uff5c", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "|", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "\uff0f", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "/", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "\\", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "\t", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "\u3000", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", " ", false},
      {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "SCRIPT", false},
      {"abc\u4e2d\u6587", " / ", false},
      {"abc\u4e2d\u6587", "\uff5c", false},
      {"abc\u4e2d\u6587", "|", false},
      {"abc\u4e2d\u6587", "\uff0f", false},
      {"abc\u4e2d\u6587", "/", false},
      {"abc\u4e2d\u6587", "\\", false},
      {"abc\u4e2d\u6587", "\t", false},
      {"abc\u4e2d\u6587", "\u3000", false},
      {"abc\u4e2d\u6587", " ", false},
      {"abc\u4e2d\u6587", "SCRIPT", true},
      {"\u4e2d\u6587\u5185\u5bb9", " / ", false},
      {"\u4e2d\u6587\u5185\u5bb9", "\uff5c", false},
      {"\u4e2d\u6587\u5185\u5bb9", "|", false},
      {"\u4e2d\u6587\u5185\u5bb9", "\uff0f", false},
      {"\u4e2d\u6587\u5185\u5bb9", "/", false},
      {"\u4e2d\u6587\u5185\u5bb9", "\\", false},
      {"\u4e2d\u6587\u5185\u5bb9", "\t", false},
      {"\u4e2d\u6587\u5185\u5bb9", "\u3000", false},
      {"\u4e2d\u6587\u5185\u5bb9", " ", false},
      {"\u4e2d\u6587\u5185\u5bb9", "SCRIPT", false},
      {"a / b", " / ", true},
      {"a / b", "\uff5c", false},
      {"a / b", "|", false},
      {"a / b", "\uff0f", false},
      {"a / b", "/", true},
      {"a / b", "\\", false},
      {"a / b", "\t", false},
      {"a / b", "\u3000", false},
      {"a / b", " ", false},
      {"a / b", "SCRIPT", false},
      {"a\uff5cb", " / ", false},
      {"a\uff5cb", "\uff5c", true},
      {"a\uff5cb", "|", false},
      {"a\uff5cb", "\uff0f", false},
      {"a\uff5cb", "/", false},
      {"a\uff5cb", "\\", false},
      {"a\uff5cb", "\t", false},
      {"a\uff5cb", "\u3000", false},
      {"a\uff5cb", " ", false},
      {"a\uff5cb", "SCRIPT", false},
      {"a|b", " / ", false},
      {"a|b", "\uff5c", false},
      {"a|b", "|", true},
      {"a|b", "\uff0f", false},
      {"a|b", "/", false},
      {"a|b", "\\", false},
      {"a|b", "\t", false},
      {"a|b", "\u3000", false},
      {"a|b", " ", false},
      {"a|b", "SCRIPT", false},
      {"a\uff0fb", " / ", false},
      {"a\uff0fb", "\uff5c", false},
      {"a\uff0fb", "|", false},
      {"a\uff0fb", "\uff0f", true},
      {"a\uff0fb", "/", false},
      {"a\uff0fb", "\\", false},
      {"a\uff0fb", "\t", false},
      {"a\uff0fb", "\u3000", false},
      {"a\uff0fb", " ", false},
      {"a\uff0fb", "SCRIPT", false},
      {"a/b", " / ", false},
      {"a/b", "\uff5c", false},
      {"a/b", "|", false},
      {"a/b", "\uff0f", false},
      {"a/b", "/", true},
      {"a/b", "\\", false},
      {"a/b", "\t", false},
      {"a/b", "\u3000", false},
      {"a/b", " ", false},
      {"a/b", "SCRIPT", false},
      {"a\\b", " / ", false},
      {"a\\b", "\uff5c", false},
      {"a\\b", "|", false},
      {"a\\b", "\uff0f", false},
      {"a\\b", "/", false},
      {"a\\b", "\\", true},
      {"a\\b", "\t", false},
      {"a\\b", "\u3000", false},
      {"a\\b", " ", false},
      {"a\\b", "SCRIPT", false},
      {"a\tb", " / ", false},
      {"a\tb", "\uff5c", false},
      {"a\tb", "|", false},
      {"a\tb", "\uff0f", false},
      {"a\tb", "/", false},
      {"a\tb", "\\", false},
      {"a\tb", "\t", false},
      {"a\tb", "\u3000", false},
      {"a\tb", " ", false},
      {"a\tb", "SCRIPT", false},
      {"a\u3000b", " / ", false},
      {"a\u3000b", "\uff5c", false},
      {"a\u3000b", "|", false},
      {"a\u3000b", "\uff0f", false},
      {"a\u3000b", "/", false},
      {"a\u3000b", "\\", false},
      {"a\u3000b", "\t", false},
      {"a\u3000b", "\u3000", false},
      {"a\u3000b", " ", false},
      {"a\u3000b", "SCRIPT", false},
      {"a b", " / ", false},
      {"a b", "\uff5c", false},
      {"a b", "|", false},
      {"a b", "\uff0f", false},
      {"a b", "/", false},
      {"a b", "\\", false},
      {"a b", "\t", false},
      {"a b", "\u3000", false},
      {"a b", " ", false},
      {"a b", "SCRIPT", false},
  };
  for (const auto& item : cases) {
    CAPTURE(item.input);
    CAPTURE(item.kind);
    CHECK(acceptsVote(item.input, item.kind, "zh") == item.expected);
  }
}

TEST_CASE("inferSplitConvention matches python reference (machine-generated table)") {
  struct InferCase {
    std::vector<std::string> lines;
    LyricSplitConvention convention;
    std::vector<std::pair<LyricSplitConvention, int>> votes;
  };
  const InferCase cases[] = {
      {std::vector<std::string>{std::string{"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f"}}, LyricSplitConvention::None, {}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"c / d"}, std::string{"e / f"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 3}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"a / b"}, std::string{"a / b"}, std::string{"a / b"}, std::string{"a\uff5cb"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 4}, {LyricSplitConvention::StrongFullwidthBar, 1}}},
      {std::vector<std::string>{std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::None, {}},
      {std::vector<std::string>{std::string{"\uff08\u304f\u3082 / \uff09"}, std::string{"\uff08\u304f\u3082 / \uff09"}, std::string{"\uff08\u304f\u3082 / \uff09"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 3}}},
      {std::vector<std::string>{std::string{"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5"}, std::string{"a\\b"}, std::string{"c\\d"}}, LyricSplitConvention::StrongBackslash, {{LyricSplitConvention::StrongBackslash, 3}}},
      {std::vector<std::string>{}, LyricSplitConvention::None, {}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::None, {{LyricSplitConvention::StrongSlashSpaced, 1}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::None, {{LyricSplitConvention::StrongSlashSpaced, 1}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"a / b"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::None, {{LyricSplitConvention::StrongSlashSpaced, 2}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"c / d"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 2}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"a / b"}, std::string{"a / b"}, std::string{"a / b"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}, std::string{"\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 4}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"c / d"}, std::string{"a\uff5cb"}, std::string{"b\uff5cc"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 2}, {LyricSplitConvention::StrongFullwidthBar, 2}}},
      {std::vector<std::string>{std::string{"x/y"}, std::string{"p/q"}, std::string{"r/s"}}, LyricSplitConvention::StrongSlash, {{LyricSplitConvention::StrongSlash, 3}}},
      {std::vector<std::string>{std::string{"x\uff0fy"}, std::string{"p\uff0fq"}, std::string{"r\uff0fs"}}, LyricSplitConvention::StrongFullwidthSlash, {{LyricSplitConvention::StrongFullwidthSlash, 3}}},
      {std::vector<std::string>{std::string{"a / b"}, std::string{"c / d"}, std::string{"x\\y"}}, LyricSplitConvention::StrongSlashSpaced, {{LyricSplitConvention::StrongSlashSpaced, 2}, {LyricSplitConvention::StrongBackslash, 1}}},
      {std::vector<std::string>{std::string{"\u65e5\u6587 \u4e2d\u6587"}, std::string{"\u65e5\u6587 \u4e2d\u6587"}, std::string{"abc \u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::None, {{LyricSplitConvention::WeakSpace, 1}}},
      {std::vector<std::string>{std::string{"a\tb"}, std::string{"c\td"}, std::string{"e\tf"}}, LyricSplitConvention::None, {}},
      {std::vector<std::string>{std::string{"abc\u4e2d\u6587\u5185\u5bb9"}, std::string{"def\u4e2d\u6587\u5185\u5bb9"}, std::string{"ghi\u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::ScriptTransition, {{LyricSplitConvention::ScriptTransition, 3}}},
      {std::vector<std::string>{std::string{"abc \u4e2d\u6587\u5185\u5bb9"}, std::string{"abc \u4e2d\u6587\u5185\u5bb9"}}, LyricSplitConvention::WeakSpace, {{LyricSplitConvention::WeakSpace, 2}}},
  };
  for (const auto& item : cases) {
    CAPTURE(item.lines.size());
    const auto actual = inferSplitConvention(item.lines, "zh");
    CHECK(actual.convention == item.convention);
    REQUIRE(actual.votes.size() == item.votes.size());
    for (std::size_t index = 0; index < item.votes.size(); ++index) {
      CAPTURE(index);
      CHECK(actual.votes[index].first == item.votes[index].first);
      CHECK(actual.votes[index].second == item.votes[index].second);
    }
  }
}
// ↑↑↑ 机器派生片段结束 ↑↑↑

// ── todo 8：splitDocument + 四语言夹具（TARGET_CLASS 映射）────────────────────
// 夹具刻意放在 tests/fixtures/lyric_split_lang/（**不在** tests/fixtures/lyric_split/ 内：
// 后者是 todo 3 用 --dump-tsv 逐字节冻结的，新增 .lrc 会破坏其 files=<M> 与行数）。
// 路径经编译定义 SERIONA_LYRIC_SPLIT_LANG_FIXTURE_DIR 注入（仿 SERIONA_WAVEFORM_FIXTURE_DIR）。

#ifdef SERIONA_LYRIC_SPLIT_LANG_FIXTURE_DIR

TEST_CASE("lyric split language fixtures exercise TARGET_CLASS mapping") {
  const std::filesystem::path fixtureRoot{SERIONA_LYRIC_SPLIT_LANG_FIXTURE_DIR};

  SUBCASE("zh 夹具：HAN 目标走弱约定，含一条中文译文被误判为原文的反例") {
    const LyricDocumentSplit document =
        splitDocument(readFixtureLines((fixtureRoot / "zh.lrc").string()), "zh.lrc", "zh");
    CHECK(document.path == "zh.lrc");
    CHECK(document.convention == LyricSplitConvention::WeakSpace);
    REQUIRE(document.votes.size() == 3);
    CHECK(document.votes[0].first == LyricSplitConvention::WeakSpace);
    CHECK(document.votes[1].first == LyricSplitConvention::ScriptTransition);
    CHECK(document.votes[2].first == LyricSplitConvention::StrongSlashSpaced);

    REQUIRE(document.results.size() == 4);
    // 清洗后的行（时间戳已剥）与结果配对保留。
    CHECK(document.results[0].first == "君の名前 你的名字");
    checkSplit(document.results[0].second,
               ExpectedSplit{.original = "君の名前", .translation = "你的名字",
                             .convention = LyricSplitConvention::WeakSpace, .confidence = "medium",
                             .reason = "weak"});
    checkSplit(document.results[1].second,
               ExpectedSplit{.original = "夢の中で", .translation = "在梦里",
                             .convention = LyricSplitConvention::WeakSpace, .confidence = "medium",
                             .reason = "weak"});
    // ★反例：日文原文与中文译文完全粘连（无分隔符）⇒ 整行作原文，中文译文被误判为原文。
    checkSplit(document.results[2].second,
               ExpectedSplit{.original = "傷ついても前を向く即使受伤也要向前", .translation = "",
                             .convention = LyricSplitConvention::None, .confidence = "none",
                             .reason = "validate-failed"});
    // 行内显式 ` / ` 强于文件弱约定（branch 5①）。
    checkSplit(document.results[3].second,
               ExpectedSplit{.original = "誰かの声", .translation = "某人的声音",
                             .convention = LyricSplitConvention::StrongSlashSpaced,
                             .confidence = "high", .reason = "strong"});
    // 译文侧确为 HAN（TARGET_CLASS[zh] 生效）：`你的名字` 共 4 个汉字。
    CHECK(profileOf(document.results[0].second.translation)[lyricLanguageIndex(
              LyricLanguage::Han)] == 4);
  }

  SUBCASE("zh 夹具换 target=en：约定/route 整体改变（证明 target 映射真的在起作用）") {
    const LyricDocumentSplit document =
        splitDocument(readFixtureLines((fixtureRoot / "zh.lrc").string()), "zh.lrc", "en");
    // target=en 时 ` ` 的 validateCutPure 要求右侧含 LATN ⇒ 弱边界 0 票，SCRIPT 反超。
    CHECK(document.convention == LyricSplitConvention::ScriptTransition);
    REQUIRE(document.results.size() == 4);
    // 同一行在 target=zh 下是「君の名前 / 你的名字」，此处变成「君の / 名前 你的名字」。
    checkSplit(document.results[0].second,
               ExpectedSplit{.original = "君の", .translation = "名前 你的名字",
                             .convention = LyricSplitConvention::ScriptTransition,
                             .confidence = "medium", .reason = "weak"});
  }

  SUBCASE("ja 夹具：译文为日文，TARGET_CLASS[ja]=JA 生效") {
    const LyricDocumentSplit document =
        splitDocument(readFixtureLines((fixtureRoot / "ja.lrc").string()), "ja.lrc", "ja");
    CHECK(document.convention == LyricSplitConvention::StrongSlashSpaced);
    REQUIRE(document.results.size() == 2);
    checkSplit(document.results[0].second,
               ExpectedSplit{.original = "星辰大海的尽头", .translation = "星の海の果てに",
                             .convention = LyricSplitConvention::StrongSlashSpaced,
                             .confidence = "high", .reason = "strong"});
    checkSplit(document.results[1].second,
               ExpectedSplit{.original = "所有的思念都随风", .translation = "すべての想いは風に",
                             .convention = LyricSplitConvention::StrongSlashSpaced,
                             .confidence = "high", .reason = "strong"});
    // 译文含假名 ⇒ classOfCodePoint 判为 Japanese（= TARGET_CLASS["ja"]）。
    CHECK(profileOf(document.results[0].second.translation)[lyricLanguageIndex(
              LyricLanguage::Japanese)] > 0);
    // 参考实现的 validateCut 硬性拒绝右侧含 JA/KO（不随 target 变），故 ja/ko 的弱/括号路径
    // 结构性不可用 —— ja 夹具必须靠显式强分隔符，这是继承自参考实现的行为，非本移植缺陷。
    checkSplit(splitLyricLine("星辰大海的尽头 星の海の果てに", LyricSplitConvention::WeakSpace,
                              "ja"),
               ExpectedSplit{.original = "星辰大海的尽头 星の海の果てに", .translation = "",
                             .convention = LyricSplitConvention::None, .confidence = "none",
                             .reason = "validate-failed"});
  }

  SUBCASE("ko 夹具：译文为韩文，TARGET_CLASS[ko]=KO 生效") {
    const LyricDocumentSplit document =
        splitDocument(readFixtureLines((fixtureRoot / "ko.lrc").string()), "ko.lrc", "ko");
    CHECK(document.convention == LyricSplitConvention::StrongSlashSpaced);
    REQUIRE(document.results.size() == 2);
    checkSplit(document.results[0].second,
               ExpectedSplit{.original = "星辰大海的尽头", .translation = "별빛 바다의 끝에서",
                             .convention = LyricSplitConvention::StrongSlashSpaced,
                             .confidence = "high", .reason = "strong"});
    checkSplit(document.results[1].second,
               ExpectedSplit{.original = "所有的思念都随风", .translation = "모든 그리움은 바람에",
                             .convention = LyricSplitConvention::StrongSlashSpaced,
                             .confidence = "high", .reason = "strong"});
    CHECK(profileOf(document.results[0].second.translation)[lyricLanguageIndex(
              LyricLanguage::Korean)] > 0);
  }

  SUBCASE("en 夹具：LATN 目标走括号路径；换 target=zh 则完全不切") {
    const LyricDocumentSplit splitForEnglish =
        splitDocument(readFixtureLines((fixtureRoot / "en.lrc").string()), "en.lrc", "en");
    CHECK(splitForEnglish.convention == LyricSplitConvention::WeakSpace);
    REQUIRE(splitForEnglish.results.size() == 2);
    checkSplit(splitForEnglish.results[0].second,
               ExpectedSplit{.original = "霞む夏の灯", .translation = "「the fading summer light」",
                             .convention = LyricSplitConvention::WeakSpace, .confidence = "high",
                             .reason = "bracket"});
    CHECK(profileOf(splitForEnglish.results[0].second.translation)[lyricLanguageIndex(
              LyricLanguage::Latin)] > 0);

    // target=zh 时「LATN 占多数的括号区」不再被认作译文（TARGET_CLASS[zh]=HAN），
    // 弱边界与 SCRIPT 皆落空 ⇒ 约定 None、整行不切。
    const LyricDocumentSplit splitForChinese =
        splitDocument(readFixtureLines((fixtureRoot / "en.lrc").string()), "en.lrc", "zh");
    CHECK(splitForChinese.convention == LyricSplitConvention::None);
    CHECK(splitForChinese.votes.empty());
    REQUIRE(splitForChinese.results.size() == 2);
    checkSplit(splitForChinese.results[0].second,
               ExpectedSplit{.original = "霞む夏の灯「the fading summer light」", .translation = "",
                             .convention = LyricSplitConvention::None, .confidence = "none",
                             .reason = "no-convention"});
  }
}

#endif  // SERIONA_LYRIC_SPLIT_LANG_FIXTURE_DIR

// ↓↓↓ 由 qa/gen_expectation_table_todo10.py 机器派生（勿手改）；原始输出见同名 .out.cpp ↓↓↓
TEST_CASE("lyric split design doc section 6.5 samples match reference implementation") {
  // 设计文档 §6.5 的 8 个样本。样本 6 与样本 8 在文档里是【过时期望】：
  //   · 样本 6 `Shake up Tonight(...)` 文档写「整行作原文」，但 ★6 已裁决改为切分（route=strong）；
  //   · 样本 8 `日文 中文`（整首单空格）文档写「由歌曲级约定推断启用」，实测该行【永不切分】：
  //     左半 `日文` 逐字符全 HAN（目标语言类），无 NON_TARGET_DECISIVE 证据 ⇒ validate_cut 必拒。
  // 两条均以【参考实现】为准，分歧见 divergence-ledger.md。
  struct Case {
    std::string_view text;
    LyricSplitConvention convention;
    ExpectedSplit expected;
  };
  const Case cases[] = {
      {"jAzLYEtN LYAsiance/.\uff08\u307f\u3093\u306a\u3068\u7406\u60f3\u3092\u7e4b\u3050\u8a69\u3092\u8b33\u3044\u307e\u3059\uff09 / \u6211\u5c06\u548f\u5531\u4e00\u9996\u8fde\u63a5\u5927\u5bb6\u4e0e\u7406\u60f3\u7684\u8bd7", LyricSplitConvention::StrongSlashSpaced, {"jAzLYEtN LYAsiance/.\uff08\u307f\u3093\u306a\u3068\u7406\u60f3\u3092\u7e4b\u3050\u8a69\u3092\u8b33\u3044\u307e\u3059\uff09", "\u6211\u5c06\u548f\u5531\u4e00\u9996\u8fde\u63a5\u5927\u5bb6\u4e0e\u7406\u60f3\u7684\u8bd7", LyricSplitConvention::StrongSlashSpaced, "high", "strong"}},
      {"haf en synk/.\uff08\u30cf\u30f3\u30c9\u30b7\u30a7\u30a4\u30af\u53d7\u9818\u3001\u597d\u610f\u3092\u8fd4\u9001\uff09 / \u63e1\u624b\u5df2\u63a5\u6536\uff0c\u56de\u8d60\u5584\u610f", LyricSplitConvention::StrongSlashSpaced, {"haf en synk/.\uff08\u30cf\u30f3\u30c9\u30b7\u30a7\u30a4\u30af\u53d7\u9818\u3001\u597d\u610f\u3092\u8fd4\u9001\uff09", "\u63e1\u624b\u5df2\u63a5\u6536\uff0c\u56de\u8d60\u5584\u610f", LyricSplitConvention::StrongSlashSpaced, "high", "strong"}},
      {"haf.\uff08\u53d7\u9818\uff09 / \u5df2\u63a5\u6536", LyricSplitConvention::StrongSlashSpaced, {"haf.\uff08\u53d7\u9818\uff09", "\u5df2\u63a5\u6536", LyricSplitConvention::StrongSlashSpaced, "high", "strong"}},
      {"\u571f\u3000\u808c\u3092\u898b\u305b\u3066\u306a\u304a / \u5927\u5730\u554a\uff0c\u5373\u4fbf\u88f8\u9732\u7740\u808c\u80a4", LyricSplitConvention::StrongSlashSpaced, {"\u571f\u3000\u808c\u3092\u898b\u305b\u3066\u306a\u304a", "\u5927\u5730\u554a\uff0c\u5373\u4fbf\u88f8\u9732\u7740\u808c\u80a4", LyricSplitConvention::StrongSlashSpaced, "high", "strong"}},
      {"\u82b1/\u958b\u304b\u305b\u308b\u82b1/\u82b1 / \u82b1\uff0f\u7efd\u653e\u7684\u82b1\uff0f\u82b1", LyricSplitConvention::StrongSlashSpaced, {"\u82b1/\u958b\u304b\u305b\u308b\u82b1/\u82b1", "\u82b1\uff0f\u7efd\u653e\u7684\u82b1\uff0f\u82b1", LyricSplitConvention::StrongSlashSpaced, "high", "strong"}},
      {"Shake up Tonight(\u30b7\u30a7\u30a4\u30ab\u30c3\u30d7\u30c8\u30a5\u30ca\u30a4\u30c8\u30a5) / Shake up Tonight(\u30b7\u30a7\u30a4\u30ab\u30c3\u30d7\u30c8\u30a5\u30ca\u30a4\u30c8\u30a5)", LyricSplitConvention::StrongSlashSpaced, {"Shake up Tonight(\u30b7\u30a7\u30a4\u30ab\u30c3\u30d7\u30c8\u30a5\u30ca\u30a4\u30c8\u30a5)", "Shake up Tonight(\u30b7\u30a7\u30a4\u30ab\u30c3\u30d7\u30c8\u30a5\u30ca\u30a4\u30c8\u30a5)", LyricSplitConvention::StrongSlashSpaced, "high", "strong"}},
      {"\u65e5\u6587\u4e2d\u6587", LyricSplitConvention::StrongSlashSpaced, {"\u65e5\u6587\u4e2d\u6587", "", LyricSplitConvention::None, "none", "validate-failed"}},
      // 样本 8：同一输入在 None 下
      {"\u65e5\u6587 \u4e2d\u6587", LyricSplitConvention::None, {"\u65e5\u6587 \u4e2d\u6587", "", LyricSplitConvention::None, "none", "no-convention"}},
      // 样本 8：同一输入在 ' / ' 下
      {"\u65e5\u6587 \u4e2d\u6587", LyricSplitConvention::StrongSlashSpaced, {"\u65e5\u6587 \u4e2d\u6587", "", LyricSplitConvention::None, "none", "validate-failed"}},
      // 样本 8：同一输入在 ' ' 下
      {"\u65e5\u6587 \u4e2d\u6587", LyricSplitConvention::WeakSpace, {"\u65e5\u6587 \u4e2d\u6587", "", LyricSplitConvention::None, "none", "validate-failed"}},
      // 样本 8：同一输入在 'SCRIPT' 下
      {"\u65e5\u6587 \u4e2d\u6587", LyricSplitConvention::ScriptTransition, {"\u65e5\u6587 \u4e2d\u6587", "", LyricSplitConvention::None, "none", "validate-failed"}},
  };
  std::set<std::string> distinctSamples;
  for (const auto& item : cases) {
    CAPTURE(item.text);
    checkSplit(splitLyricLine(item.text, item.convention, "zh"), item.expected);
    distinctSamples.insert(std::string{item.text});
  }
  // §6.5 共 8 个样本；样本 8 在 4 种约定下各断言一次 ⇒ 表内 11 行。
  CHECK(sizeof(cases) / sizeof(cases[0]) == 11U);
  CHECK(distinctSamples.size() == 8);
}

TEST_CASE("lyric split design doc section 3.6 multi-separator counterexamples") {
  // §3.6 两个反例：两行都含多个 ` / `，其中原文内部的那个出现在真正的分界之前。
  //   · Rrha 行相邻候选之间的空隙【只剩本分隔符与空白】⇒ 合并成一个分隔符段，
  //     取该段（= 最后一个通过验证的分界）⇒ 切出正确译文；
  //   · Afterburner 行的三段【各含内容】⇒ 触发 D10「多段并列整行作原文」，绝不回退。
  checkSplit(splitLyricLine("Rrha num wa ene revm /  / \u6211\u505a\u4e86\u4e00\u4e2a\u68a6", LyricSplitConvention::StrongSlashSpaced, "zh"),
             {"Rrha num wa ene revm", "\u6211\u505a\u4e86\u4e00\u4e2a\u68a6", LyricSplitConvention::StrongSlashSpaced, "high", "strong"});
  checkSplit(splitLyricLine("\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", LyricSplitConvention::StrongSlashSpaced, "zh"),
             {"\uff08Afterburner\uff09 / \u63fa\u308b\u304e\u306a\u3044 Spirit / \uff08Afterburner\uff09 / \u575a\u5b9a\u4e0d\u79fb\u7684Spirit", "", LyricSplitConvention::None, "none", "validate-failed"});
}

TEST_CASE("lyric split never treats typographic spaces or pure Japanese as boundaries") {
  // §3.5 T2：U+3000 是日文排版间距，绝不能当分界。单行（弱约定 + 强约定两级）与整首
  // 推断三级都必须拒绝。
  // WeakFullwidthSpace（最容易被误当分界的约定）。
  checkSplit(splitLyricLine("\u79c1\u306e\u304a\u6708\u69d8\u3000\u9006\u3055\u307e\u306e\u304a\u6708\u69d8", LyricSplitConvention::WeakFullwidthSpace, "zh"), {"\u79c1\u306e\u304a\u6708\u69d8\u3000\u9006\u3055\u307e\u306e\u304a\u6708\u69d8", "", LyricSplitConvention::None, "none", "validate-failed"});
  // StrongSlashSpaced（整首是强约定时该行也必须整行作原文）。
  checkSplit(splitLyricLine("\u79c1\u306e\u304a\u6708\u69d8\u3000\u9006\u3055\u307e\u306e\u304a\u6708\u69d8", LyricSplitConvention::StrongSlashSpaced, "zh"), {"\u79c1\u306e\u304a\u6708\u69d8\u3000\u9006\u3055\u307e\u306e\u304a\u6708\u69d8", "", LyricSplitConvention::None, "none", "validate-failed"});
  // 纯日文行（U+3000 排版间距）同样不得切分。
  checkSplit(splitLyricLine("\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", LyricSplitConvention::WeakFullwidthSpace, "zh"), {"\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", "", LyricSplitConvention::None, "none", "validate-failed"});
  // 歌曲级推断：纯日文件不得推出任何【单空格/全角空格/制表符】弱约定。
  const LyricSplitConventionVotes inferred = inferSplitConvention({"\u79c1\u306e\u304a\u6708\u69d8\u3000\u9006\u3055\u307e\u306e\u304a\u6708\u69d8", "\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f"}, "zh");
  CHECK(inferred.convention == LyricSplitConvention::None);
  for (const auto& vote : inferred.votes) {
    CHECK(vote.first != LyricSplitConvention::WeakSpace);
    CHECK(vote.first != LyricSplitConvention::WeakFullwidthSpace);
    CHECK(vote.first != LyricSplitConvention::WeakTab);
  }
  // 完整链路（splitDocument）同样不得推出单空格约定，且逐行整行作原文。
  const LyricDocumentSplit document =
      splitDocument({"\u79c1\u306e\u304a\u6708\u69d8\u3000\u9006\u3055\u307e\u306e\u304a\u6708\u69d8", "\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f"}, "pure-ja.lrc", "zh");
  CHECK(document.convention == LyricSplitConvention::None);
  REQUIRE(document.votes.size() == 1);
  CHECK(document.votes[0].first == LyricSplitConvention::ScriptTransition);
  CHECK(document.votes[0].second == 1);
  REQUIRE(document.results.size() == 2);
  checkSplit(document.results[0].second,
             ExpectedSplit{.original = document.results[0].first, .translation = "",
                           .convention = LyricSplitConvention::None, .confidence = "none",
                           .reason = "no-convention"});
  checkSplit(document.results[1].second,
             ExpectedSplit{.original = document.results[1].first, .translation = "",
                           .convention = LyricSplitConvention::None, .confidence = "none",
                           .reason = "no-convention"});
}

TEST_CASE("lyric split drops metadata lines before splitting") {
  // §6.3 P1：顺序必须是「先剥时间戳 → 再去行内 [tag:value] → 最后滤制作人员」，
  // 否则 `[00:00.000][by:x]` 会整行漏过过滤。单行与整链两级都钉住。
  CHECK_FALSE(cleanLine("[00:00.000][by:\u968f\u9047\u4e0d\u5b89_]").has_value());
  CHECK_FALSE(cleanLine("\u4f5c\u8bcd : xxx").has_value());
  CHECK_FALSE(cleanLine("[ti:\u793a\u4f8b\u6807\u9898]").has_value());
  // 整链：三条元数据行全部不进 results（清洗后行数 3 -> 2）。
  const LyricDocumentSplit document = splitDocument({"[ti:\u793a\u4f8b\u6807\u9898]", "[00:00.000][by:\u968f\u9047\u4e0d\u5b89_]", "\u4f5c\u8bcd : xxx", "\u539f\u6587 / \u8bd1\u6587", "\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f"}, "meta.lrc", "zh");
  REQUIRE(document.results.size() == 2);
  CHECK(document.results[0].first == "\u539f\u6587 / \u8bd1\u6587");
  CHECK(document.results[1].first == "\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f");
  CHECK(document.convention == LyricSplitConvention::None);
  REQUIRE(document.votes.size() == 1);
  CHECK(document.votes[0].first == LyricSplitConvention::StrongSlashSpaced);
  CHECK(document.votes[0].second == 1);
  checkSplit(document.results[0].second,
             ExpectedSplit{.original = "\u539f\u6587", .translation = "\u8bd1\u6587",
                           .convention = LyricSplitConvention::StrongSlashSpaced,
                           .confidence = "high", .reason = "strong"});
  checkSplit(document.results[1].second,
             ExpectedSplit{.original = "\u4e7e\u3044\u305f\u571f\u306b\u305d\u3063\u3068\u3000\u96e8\u7c92\u304c\u6d41\u308c\u3066\u304f", .translation = "",
                           .convention = LyricSplitConvention::None,
                           .confidence = "none", .reason = "no-convention"});
}

TEST_CASE("lyric split D38 fixes match the fourth round user verdict byte for byte") {
  // D38①（§6.2.7 缺陷 13）：`/` 全在注音括号内 ⇒ 改用括号后的空白（strong-weak-fallback）。
  // 两条的原文/译文与用户第四轮标注逐字节一致。
  checkSplit(splitLyricLine("\u8537\u8587\u3092\u60f3\u308f\u305b\u308b\u7eef\u8272\u306e\u300e\u53e3\u7ea2\u300f(\u30ed\u30fc\u30ba\u30ec\u30c3\u30c9\u30eb\u30fc\u30b8\u30e5 / )  \u4ee4\u4eba\u8054\u60f3\u8d77\u73ab\u7470\u7684\u7eef\u8272\u53e3\u7ea2", LyricSplitConvention::StrongSlashSpaced, "zh"),
             {"\u8537\u8587\u3092\u60f3\u308f\u305b\u308b\u7eef\u8272\u306e\u300e\u53e3\u7ea2\u300f(\u30ed\u30fc\u30ba\u30ec\u30c3\u30c9\u30eb\u30fc\u30b8\u30e5 / )", "\u4ee4\u4eba\u8054\u60f3\u8d77\u73ab\u7470\u7684\u7eef\u8272\u53e3\u7ea2", LyricSplitConvention::StrongSlashSpaced, "high", "strong-weak-fallback"});
  checkSplit(splitLyricLine("\u3042\u306e\u300e\u60b2\u9e23\u306f\u300f(\u3046\u305f\u3054\u3048\u304c / )\u300e\u8461\u8404\u9152\u300f   \u90a3\u60b2\u9e23\uff08\u6b4c\u58f0\uff09\u6709\u5982\u8461\u8404\u7f8e\u9152", LyricSplitConvention::StrongSlashSpaced, "zh"),
             {"\u3042\u306e\u300e\u60b2\u9e23\u306f\u300f(\u3046\u305f\u3054\u3048\u304c / )\u300e\u8461\u8404\u9152\u300f", "\u90a3\u60b2\u9e23\uff08\u6b4c\u58f0\uff09\u6709\u5982\u8461\u8404\u7f8e\u9152", LyricSplitConvention::StrongSlashSpaced, "high", "strong-weak-fallback"});
  // D38②（§6.2.7 缺陷 14）：`\` 纳入强分隔符末位。设计文档引用过的该行
  // （`…背伸びしてもいい\回首也可以 成长也可以`）修复后译文是【整段】而不是只剩后半句。
  checkSplit(splitLyricLine("\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044\\\u56de\u9996\u4e5f\u53ef\u4ee5 \u6210\u957f\u4e5f\u53ef\u4ee5", LyricSplitConvention::StrongBackslash, "zh"),
             {"\u80cc\u4f38\u3073\u3057\u3066\u3082\u3044\u3044", "\u56de\u9996\u4e5f\u53ef\u4ee5 \u6210\u957f\u4e5f\u53ef\u4ee5", LyricSplitConvention::StrongBackslash, "high", "strong"});
  // 整首推断：`\` 分隔的文件必须推断成 StrongBackslash（修复前会被推成空格）。
  const LyricSplitConventionVotes inferred = inferSplitConvention({"a\\b", "c\\d", "e\\f"}, "zh");
  CHECK(inferred.convention == LyricSplitConvention::StrongBackslash);
}

TEST_CASE("lyric split strong branch weak fallback keeps position order and two-part structure") {
  // S5 MED-1：强分支的括号内降级链末尾，弱边界候选必须【按位置升序】逐个 validateCutPure，
  // 且结果是【两段】（先 validateCutPure 得到切点，全败才 cutScript）。
  // 若把候选改【降序】或压成「直接 cutScript」，下面两条都会变红（已实测）。
  checkSplit(splitLyricLine("\u300c\u3042 / \u3044\u300d\u4e2d \u6587 \u5b57", LyricSplitConvention::StrongSlashSpaced, "zh"),
             {"\u300c\u3042 / \u3044\u300d\u4e2d", "\u6587 \u5b57", LyricSplitConvention::StrongSlashSpaced, "high", "strong-weak-fallback"});
  checkSplit(splitLyricLine("\u300c\u3042\uff5c\u3044\u300d\u4e2d \u6587abc", LyricSplitConvention::StrongFullwidthBar, "zh"),
             {"\u300c\u3042\uff5c\u3044\u300d\u4e2d", "\u6587abc", LyricSplitConvention::StrongFullwidthBar, "high", "strong-weak-fallback"});
}

TEST_CASE("lyric split weak branch probes only the spaced separator and skips bracket guess") {
  // LOW-1：None 分支【只】试带空格的 ` / `，绝不试裸 `/` —— 否则 `x/y` 会被切成 x | y。
  checkSplit(splitLyricLine("x/y", LyricSplitConvention::None, "zh"), {"x/y", "", LyricSplitConvention::None, "none", "no-convention"});
  // LOW-2：分支 5② —— 行内已含 ` / ` 时不再走括号结构猜测（否则 reason 会变 bracket/high）。
  checkSplit(splitLyricLine(" / a \u300c\u4e2d\u300d", LyricSplitConvention::WeakSpace, "zh"), {" / a", "\u300c\u4e2d\u300d", LyricSplitConvention::WeakSpace, "medium", "weak"});
}

// ↑↑↑ 机器派生片段结束 ↑↑↑

// ── todo 10：D20 括号配对性质（§6.3 P3）──────────────────────────────────────

TEST_CASE("lyric split keeps bracket pairs balanced across every convention") {
  // D20：切点永不落在配对括号内部 ⇒ 任何一次切分的两侧必须【各自】括号配对
  // （`「…」` 不得被拆成「前段带开括号、后段带闭括号」的不配对两半）。
  const LyricSplitResult bracketed = splitLyricLine("世界は初め   灰色で 「世界最初是灰色的」",
                                                    LyricSplitConvention::WeakSpace, "zh");
  checkSplit(bracketed, ExpectedSplit{.original = "世界は初め   灰色で",
                                      .translation = "「世界最初是灰色的」",
                                      .convention = LyricSplitConvention::WeakSpace,
                                      .confidence = "high", .reason = "bracket"});
  CHECK(bracketsAreBalanced(bracketed.original));
  CHECK(bracketsAreBalanced(bracketed.translation));

  // 性质：设计文档点名过的括号密集行 × 全部 11 个约定，两侧一律配对。
  const std::string_view probes[] = {
      "世界は初め   灰色で 「世界最初是灰色的」",
      "霞む夏の灯「朦胧的夏日灯火」",
      "おぼつかぬ足取り 「/略带动摇的步伐」",
      "高架橋（こうかきょう） 雨（あめ）降（ふ）らす神様（かみさま / ）/高架橋 降雨之神",
      "（Afterburner） / 揺るぎない Spirit / （Afterburner） / 坚定不移的Spirit",
      "「A」 / 「B」 / 「C」 / 「B」",
  };
  for (const auto& probe : probes) {
    for (const auto& entry : kExpectedTokens) {
      CAPTURE(probe);
      CAPTURE(entry.token);
      const LyricSplitResult result = splitLyricLine(probe, entry.convention, "zh");
      CHECK(bracketsAreBalanced(result.original));
      CHECK(bracketsAreBalanced(result.translation));
    }
  }
}

// ── todo 10：黄金夹具驱动回归（8 个 .lrc × golden-expected.tsv 的每一数据行）─────
// 夹具目录由编译定义 SERIONA_LYRIC_SPLIT_FIXTURE_DIR 注入（仿 SERIONA_LYRIC_SPLIT_LANG_FIXTURE_DIR）。
// 行格式（tools/lyric_split_regression/lyric_split_tool.py `run_dump_tsv`）：
//   相对路径 \t 1-based 清洗后行号 \t 文档级约定记号 \t reason \t confidence \t 原文 \t 译文
// 第 3/4/5/6/7 列由 `_escape_tsv` 转义（`\`→`\\`、TAB→`\t`、LF→`\n`），比对前必须逆转义。
// 第 3 列（约定记号）是【文档级】的，逐文件断言一次；其余四列逐行断言。

#ifdef SERIONA_LYRIC_SPLIT_FIXTURE_DIR

// `_escape_tsv` 的逆运算。必须单趟左→右：写成顺序 replace 会让字面 `\\t`（三字符）
// 被二次解码成 TAB，与转义端「先替反斜杠」的顺序不对称。
[[nodiscard]] std::string unescapeTsvColumn(std::string_view escaped) {
  std::string decoded;
  decoded.reserve(escaped.size());
  for (std::size_t index = 0; index < escaped.size(); ++index) {
    if (escaped[index] == '\\' && index + 1 < escaped.size()) {
      const char next = escaped[index + 1];
      if (next == '\\') {
        decoded.push_back('\\');
        ++index;
        continue;
      }
      if (next == 't') {
        decoded.push_back('\t');
        ++index;
        continue;
      }
      if (next == 'n') {
        decoded.push_back('\n');
        ++index;
        continue;
      }
    }
    decoded.push_back(escaped[index]);
  }
  return decoded;
}

[[nodiscard]] std::vector<std::string> splitTsvFields(std::string_view line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      fields.emplace_back(line.substr(start));
      return fields;
    }
    fields.emplace_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

TEST_CASE("lyric split golden fixture matches the reference dump line by line") {
  const std::filesystem::path fixtureRoot{SERIONA_LYRIC_SPLIT_FIXTURE_DIR};
  std::ifstream tsv{std::string{(fixtureRoot / "golden-expected.tsv").string()}, std::ios::binary};
  REQUIRE_MESSAGE(tsv.good(), "无法打开黄金期望文件");

  std::string header;
  REQUIRE(std::getline(tsv, header));
  CHECK(header == "# total_lines=29 files=8 unresolved_decode=0 decode_by_codec=utf-8-sig=8");

  struct Row {
    std::string file;
    std::size_t line = 0;
    std::string conventionToken;
    std::string reason;
    std::string confidence;
    std::string original;
    std::string translation;
  };
  std::vector<Row> rows;
  std::string raw;
  while (std::getline(tsv, raw)) {
    if (raw.empty()) {
      continue;
    }
    const std::vector<std::string> fields = splitTsvFields(raw);
    REQUIRE(fields.size() == 7);
    Row row;
    row.file = fields[0];
    row.line = static_cast<std::size_t>(std::stoul(fields[1]));
    row.conventionToken = unescapeTsvColumn(fields[2]);
    row.reason = unescapeTsvColumn(fields[3]);
    row.confidence = unescapeTsvColumn(fields[4]);
    row.original = unescapeTsvColumn(fields[5]);
    row.translation = unescapeTsvColumn(fields[6]);
    rows.push_back(std::move(row));
  }
  REQUIRE(rows.size() == 29);

  std::set<std::string> files;
  std::size_t index = 0;
  while (index < rows.size()) {
    const std::string file = rows[index].file;
    std::size_t end = index;
    while (end < rows.size() && rows[end].file == file) {
      ++end;
    }
    CAPTURE(file);
    CHECK(files.insert(file).second);  // 同一文件的数据行必须连续出现（表按文件排序）

    const std::vector<std::string> rawLines = readFixtureLines((fixtureRoot / file).string());
    REQUIRE_FALSE(rawLines.empty());
    const LyricDocumentSplit document = splitDocument(rawLines, file, "zh");
    // 第 3 列是文档级约定记号：不能拿每行的 effectiveConvention 比（降级行记的是实际
    // 生效的 other，如 strong-fallback 记 StrongSlash；未切分行记 None）。
    CHECK(conventionToken(document.convention) == rows[index].conventionToken);
    REQUIRE(document.results.size() == end - index);

    // 纯函数性（文档级）：同一输入重复调用必须逐字段一致。
    const LyricDocumentSplit repeated = splitDocument(rawLines, file, "zh");
    CHECK(repeated.convention == document.convention);
    CHECK(repeated.votes == document.votes);
    REQUIRE(repeated.results.size() == document.results.size());

    for (std::size_t offset = index; offset < end; ++offset) {
      const LyricSplitResult& actual = document.results[offset - index].second;
      CAPTURE(rows[offset].line);
      CHECK(rows[offset].line == offset - index + 1);
      CHECK(actual.reason == rows[offset].reason);
      CHECK(actual.confidence == rows[offset].confidence);
      CHECK(actual.original == rows[offset].original);
      CHECK(actual.translation == rows[offset].translation);
      const LyricSplitResult& again = repeated.results[offset - index].second;
      CHECK(again.original == actual.original);
      CHECK(again.translation == actual.translation);
      CHECK(again.reason == actual.reason);
    }
    index = end;
  }
  CHECK(files.size() == 8);
}

// ── todo 10（S6 三轮 F8）：冻结夹具目录守卫 ────────────────────────────────────
// 夹具目录是 todo 3 的**冻结**产物（「不得再新增 .lrc」）；此前只有 golden 用例隐式
// 依赖它（多一个 .lrc 会让逐行配对错位，但那是**间接**失败）。这里加**直接**守卫：
// 目录内容恰为 8 个 .lrc + golden-expected.tsv，且后者字节不变（md5 钉住）。

// 极简 MD5（RFC 1321）。测试内自持：只为把冻结文件 pin 成 md5，避免为此引入
// OpenSSL 等新依赖（三端供给约束）。数值取自 RFC 1321 的 K/S 常量表。
[[nodiscard]] std::uint32_t rotateLeftMd5(std::uint32_t value, unsigned shift) {
  return (value << shift) | (value >> (32u - shift));
}

[[nodiscard]] std::string md5Hex(std::string_view data) {
  constexpr std::array<std::uint32_t, 64> kRoundConstant{
      0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
      0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
      0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
      0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
      0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
      0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
      0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
      0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
      0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
      0xeb86d391u};
  constexpr std::array<unsigned, 64> kShift{
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
      14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16,
      23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

  std::vector<unsigned char> message(data.begin(), data.end());
  const std::uint64_t bitLength = static_cast<std::uint64_t>(message.size()) * 8u;
  message.push_back(0x80u);
  while (message.size() % 64u != 56u) {
    message.push_back(0u);
  }
  for (unsigned index = 0; index < 8; ++index) {
    message.push_back(static_cast<unsigned char>((bitLength >> (8u * index)) & 0xFFu));
  }

  std::uint32_t stateA = 0x67452301u;
  std::uint32_t stateB = 0xefcdab89u;
  std::uint32_t stateC = 0x98badcfeu;
  std::uint32_t stateD = 0x10325476u;
  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
      const unsigned char* bytes = &message[offset + index * 4];
      words[index] = static_cast<std::uint32_t>(bytes[0]) |
                     (static_cast<std::uint32_t>(bytes[1]) << 8u) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16u) |
                     (static_cast<std::uint32_t>(bytes[3]) << 24u);
    }
    std::uint32_t a = stateA;
    std::uint32_t b = stateB;
    std::uint32_t c = stateC;
    std::uint32_t d = stateD;
    for (std::size_t round = 0; round < 64; ++round) {
      std::uint32_t f = 0;
      std::size_t g = 0;
      if (round < 16) {
        f = (b & c) | (~b & d);
        g = round;
      } else if (round < 32) {
        f = (d & b) | (~d & c);
        g = (5 * round + 1) % 16;
      } else if (round < 48) {
        f = b ^ c ^ d;
        g = (3 * round + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * round) % 16;
      }
      const std::uint32_t sum = f + a + kRoundConstant[round] + words[g];
      a = d;
      d = c;
      c = b;
      b = b + rotateLeftMd5(sum, kShift[round]);
    }
    stateA += a;
    stateB += b;
    stateC += c;
    stateD += d;
  }

  constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(32);
  for (const std::uint32_t word : {stateA, stateB, stateC, stateD}) {
    for (unsigned index = 0; index < 4; ++index) {
      const unsigned byte = static_cast<unsigned>((word >> (8u * index)) & 0xFFu);
      hex.push_back(kHexDigits[byte >> 4u]);
      hex.push_back(kHexDigits[byte & 0x0Fu]);
    }
  }
  return hex;
}

TEST_CASE("lyric split frozen fixture directory guard") {
  const std::filesystem::path fixtureRoot{SERIONA_LYRIC_SPLIT_FIXTURE_DIR};
  const std::set<std::string> expectedLrc{
      "golden-lines.lrc", "route-backslash.lrc", "route-no-convention.lrc",
      "route-strong-bracket-fallback.lrc", "route-strong-fallback.lrc",
      "route-strong-weak-fallback.lrc", "route-validate-failed.lrc", "route-weak-bracket.lrc"};

  std::set<std::string> actualLrc;
  bool hasGoldenExpected = false;
  std::size_t fileCount = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(fixtureRoot)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = seriona::scanner::pathToUtf8(entry.path().filename());
    ++fileCount;
    if (name == "golden-expected.tsv") {
      hasGoldenExpected = true;
      continue;
    }
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".lrc") {
      actualLrc.insert(name);
    }
  }

  CHECK(actualLrc == expectedLrc);
  CHECK(hasGoldenExpected);
  CHECK(fileCount == expectedLrc.size() + 1);

  std::ifstream golden{fixtureRoot / "golden-expected.tsv", std::ios::binary};
  REQUIRE_MESSAGE(golden.good(), "无法打开 golden-expected.tsv");
  const std::string goldenBytes{std::istreambuf_iterator<char>(golden),
                                std::istreambuf_iterator<char>()};
  CHECK(md5Hex(goldenBytes) == "e1e7b0c83387cd7e9c5781b5226aa682");
}

#endif  // SERIONA_LYRIC_SPLIT_FIXTURE_DIR

}  // namespace seriona::control
