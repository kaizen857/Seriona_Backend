#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::scanner {

enum class LrcParseErrorCode {
  IoFailure,
  FileTooLarge,
  TooManyLines,
  InvalidTimestamp,
  // G5a：纯文本歌词的 6 步编码回退全部失败（文件被跳过并计数，不崩溃）。
  // 追加于末尾：既有枚举值的 ordinal 不变。
  UnsupportedEncoding,
};

struct LrcParseOptions {
  std::size_t maxBytes{1024U * 1024U};
  std::size_t maxLines{10'000U};
};

struct LrcParseError {
  LrcParseErrorCode code{LrcParseErrorCode::InvalidTimestamp};
  std::filesystem::path path;
  std::size_t line{0};
  std::size_t column{0};
  std::string message;
  std::string detail;
};

struct LrcParseResult {
  std::vector<LyricLine> lines{};
  std::vector<LrcParseError> errors{};
};

[[nodiscard]] LrcParseResult parseLrcText(std::string text,
                                          const LrcParseOptions& options = {},
                                          std::optional<std::filesystem::path> path = std::nullopt);
[[nodiscard]] LrcParseResult parseLrcFile(const std::filesystem::path& path,
                                          const LrcParseOptions& options = {});

// ── G5a：纯文本歌词（无时间戳）──────────────────────────────────────────────
//
// 编码按设计文档 §3.1 与参考实现 tools/lyric_split_regression/lyric_split_tool.py:610-621
// （`TEXT_ENCODINGS`）的 6 步回退顺序：utf-8-sig → utf-8 → gb18030 → big5 → shift_jis →
// utf-16，严格解码、首个成功即采信、全失败返回 nullopt（调用方跳过该文件并计数，不崩溃）。
// 该顺序不在任何 `AGENTS.md` 里。`utf-8-sig` 在首位 ⇒ 无 BOM 的 UTF-8 也命中同一分支。
//
// ⚠️ 只保证「顺序与集合」与参考实现一致；ICU 与 Python 同名 codec 在部分字节序列上并不等价
// （已登记差异）。不得据此声称与 Python 等价。
[[nodiscard]] std::optional<std::string> decodeLyricsBytes(std::string_view bytes);

// 解析已解码（UTF-8）的纯文本歌词：逐行产出无时间戳 LyricLine。时间戳为 unsynced 哨兵
// （< 0，与 parseLrcText 的 G4 无时间戳行同一哨兵、同一判据）。
[[nodiscard]] LrcParseResult parsePlainTextLyrics(std::string text, const LrcParseOptions& options = {},
                                                  std::optional<std::filesystem::path> path = std::nullopt);

// 读取文件字节 → 6 步回退解码 → parsePlainTextLyrics。解码全失败 ⇒ 空 lines + 一条
// UnsupportedEncoding 错误（被跳过并计数），不抛异常、不崩溃。
[[nodiscard]] LrcParseResult parsePlainTextLyricsFile(const std::filesystem::path& path,
                                                      const LrcParseOptions& options = {});

// ── G5b：SubRip（.srt）歌词 ────────────────────────────────────────────────
//
// 只识别 SRT 的结构：序号行（纯数字行）+ `hh:mm:ss,mmm --> hh:mm:ss,mmm` 时间轴行 + 文本块
// （块间以空行分隔，块内可多行；解析器另把时间轴行也当块边界，使缺空行分隔的文件仍能分块）。
// 时间戳取**起始时间**（结束时间校验合法后丢弃）。序号行与时间轴行**不产出正文**；畸形块
// （无时间轴 / 时间轴格式错 / 只有序号）跳过并按需记录错误，不崩溃、不把时间轴行当正文。
//
// 多行文本按 D33/§7/§8.9.5 **保留为多行**（每行一个 LyricLine，同一 timestamp = 该块起始时间）：
// 该三处均未规定多行块应合并还是保留 ⇒ 判为欠定，取「信息不丢失」的一侧；判据与取舍见证据目录。
// 结果按 timestamp 稳定排序（与 parseLrcText 同构），同一时间戳的多行保持块内文件顺序，与 D22
// 「组内第 1 行 = 原文、第 2 行 = 译文」的配对约定契合。
//
// 文本块内的 HTML 标签（`<i>`/`</i>`/`<b>`/`<font ...>` 等）**剥离标签、保留文本**；文法严格保守
// （标签名以 ASCII 字母开头、属性不得含 `<`），比较运算 `<`、未配对 `<`、`<3:4>`/`<00:12.34>`
// 原样保留。SRT 路径**不**应用 `.lrc` 的 `<mm:ss.xx>` 逐字标签剥离（SRT 无逐字约定），**不**应用
// G6 行内元数据判据（SRT 文本块是内容，不是 LRC 标签面）。
//
// 编码回退复用 decodeLyricsBytes（与 `.txt` 同一条 6 步顺序）。全失败 ⇒ 空 lines + 一条
// UnsupportedEncoding 错误（被跳过并计数），与 parsePlainTextLyricsFile 同构。
[[nodiscard]] LrcParseResult parseSrtLyrics(std::string text, const LrcParseOptions& options = {},
                                            std::optional<std::filesystem::path> path = std::nullopt);
[[nodiscard]] LrcParseResult parseSrtLyricsFile(const std::filesystem::path& path,
                                                const LrcParseOptions& options = {});

// ── G5c：Advanced SubStation Alpha（.ass / .ssa）歌词 ────────────────────────
//
// 只解析 `[Events]` 段内的 `Dialogue:` 行，字段顺序由该段的 `Format:` 行**按名**决定（**不硬编码
// 任何列号**；`[V4+ Styles]` / `[V4 Styles]` 的 `Format:` 是样式列，忽略）。`Dialogue:` 按
// Format 的字段数做 N-1 次逗号切分、**末列吃下剩余全部内容**，故 `Text` 内部的逗号不会被截断。
// 段名 / 键名 / 字段名按 **ASCII 大小写不敏感**比较。`Comment:` 行与其他段（含段外同名行）跳过。
//
// 时间戳是 ASS 的 `H:MM:SS.cc`（centisecond，2 位；`:`/`.` 固定；小时 **1–2 位**），与 SRT 的
// `hh:mm:ss,mmm` **不同**（**不接受**逗号毫秒）；只解析并取用 **`Start`**（`End` 列**不被读取**）。该文法把时间戳
// 上界限定在 359,999,990 ms（编译期 static_assert 证明）⇒ 无溢出，故不使用 SRT 路径的运行时上界检查。
//
// 覆盖块 `{\...}`（特效/定位/行内注释）整块剥离，并复用 SRT 的保守 `<...>` 标签剥离（`a < b`、
// `<3:4>`、未配对 `<` 原样保留）。`\p<n>`（n≥1）进入矢量绘图模式 ⇒ 其后负载（图形指令，非
// 可见文字）**丢弃**至 `\p0` 或行尾；标签名以 `os`/`bo` 开头者（`\pos`/`\pbo`，libass 在 `\p`
// 之前匹配二者）**不改变**绘图态，其余形态按 libass `argtoi32`（空参/非数字 ⇒ 0）**关闭**绘图态。
// `\p` 词法镜像 libass 标签词法器：`\` 之后**先跳 `' '`/`'\t'`**（故 `\ p1` 被识别为 `\p1`）；
// 标签实参优先取**括号参数表的第一个非空参数**（libass 先解析括号表 ⇒ `\p(1)` ⇒ 开、`\p1(0)` ⇒
// 读 `0` ⇒ 关），否则取标签名之后的文本；数字解析镜像 `strtoll`（跳过前导 ASCII 空白，再接受
// **一个**可选 `+`/`-` 符号 ⇒ `\p 1`、`\p+1` 开；`\p-1` 因负值被钳 0 ⇒ 关）。
// **未覆盖**：本实现只镜像上述两条词法（`\` 后空白 + 括号实参）与 libass 唯一的标签递归点
// （`\t` 的参数内部再解析；libass 侧仅当实参个数 ∈ [1,4] 且实参内含 `\` 时生效，`ass_parse.c:709`/`:713`）；
// libass 标签词法的其余深层细节（其它标签括号内的 `\p` 一项已覆盖）未逐一镜像 —— 见证据
// `README.md` 的残余差异清单与 libass 0.17.5 渲染探针对照表。
// 字符级转义：`\N` 硬换行 ⇒ **切成多行、同一 timestamp = 该 Dialogue 的 Start**（与 SRT 多行块
// 决策同构：信息不丢失 + 契合 D22 + 契合 todo 11 的稳定序）；`\n` 软换行 ⇒ **一个普通空格**
// （Aegisub 规格：仅 wrap style 2 断行，其余模式即空格）；`\h` 硬空格 ⇒ **U+00A0**（libass
// `ass_parse.c` 对 `\h` 返回 `NBSP`，`#define NBSP 0xa0`）；`\{` / `\}` ⇒ 字面 `{` / `}`
// （libass `ass_parse.c:1139-1146`；故 `\{a\}` 不被误当覆盖块删除）；其余 `\x` 原样保留。覆盖块
// 剥离后为空的 Dialogue 不产出。结果按 timestamp 稳定排序、**不去重**（与 parseSrtLyrics 一致；
// 与 parseLrcText 不同——后者排序后仍执行 `std::ranges::unique`）。
//
// 编码回退复用 decodeLyricsBytes（与 `.txt`/`.srt` 同一条 6 步顺序）。全失败 ⇒ 空 lines + 一条
// UnsupportedEncoding 错误（被跳过并计数），与 parseSrtLyricsFile 同构。
[[nodiscard]] LrcParseResult parseAssLyrics(std::string text, const LrcParseOptions& options = {},
                                            std::optional<std::filesystem::path> path = std::nullopt);
[[nodiscard]] LrcParseResult parseAssLyricsFile(const std::filesystem::path& path,
                                                const LrcParseOptions& options = {});

// ── G5d：TTML / DFXP（.ttml / .dfxp）歌词 ────────────────────────────────────
//
// 严格子集的手写扫描器（**不是**通用 XML 处理器：无 DTD、无命名空间解析、无结构校验），仓库无任何
// XML 依赖、且不引入 `std::regex`。产出单位是 `<p>`：时间戳取 `begin`，文本取该 `<p>` 内全部文本内容
// （含嵌套 `<span>` 与未知内联元素，按文档顺序拼接）。命名空间按 **local name** 匹配（忽略前缀）。
//
// 文本处理：`<br>` / `<br/>` 是硬换行 ⇒ 切成多个 LyricLine、**共享同一 timestamp**（与 SRT 多行块、
// ASS `\N` 同构：信息不丢失 + 契合 D22 配对 + 稳定序）。XML 实体按 5 个预定义名 + 十进制/十六进制
// 数字字符引用还原；**未定义实体**（如 `&nbsp;`）与不构成实体的孤立 `&` **原样保留文字**（不报错、
// 不丢文本）。CDATA 内容按字面取用、不还原实体。ASCII 空白（空格/制表/换行/回车）连续段折叠为
// 单个空格并去首尾（依据 TTML2 默认 `xml:space="default"`；不区分 `xml:space="preserve"`）。
//
// `begin` 文法（W3C TTML2 §12.3.1 time-expression）：接受 clock-time `hh:mm:ss[.fraction]`
// （hours ≥2 位、minutes/seconds 恰 2 位且 ≤59）与 offset-time `count[.fraction](h|m|s|ms)`；
// **拒绝**帧形式 `h:m:s:ff`（帧→毫秒需 `ttp:frameRate`，猜帧率会静默给出错误时间戳）、`f`/`t`
// 度量、无度量裸数、两段式 `mm:ss`、`wallclock()`、带符号值（负值无法用「<0 ⟺ unsynced」哨兵表达）。
// `<p>` 缺 `begin` ⇒ 按 unsynced 哨兵产出且**文本照常保留**；`begin` 存在但畸形 ⇒ 记一条
// `InvalidTimestamp` 并**跳过该 `<p>`**（与 SRT 畸形时间轴、ASS 畸形 Start 同处置）。
//
// 结果按 timestamp 稳定排序、**不去重**（与 parseSrtLyrics / parseAssLyrics 一致）。
//
// `maxLines` 限制被处理的**源文件行数**（行号 1 起计；越限即停止扫描并记一条 `TooManyLines`），
// 不是产出行数。停止点落在某个 `<p>` 内部时，该块已收到的文本仍会产出，其后的源码不再处理
// ⇒ 产出行数可多于 `maxLines`。产出内存由 `maxBytes` 兜住：
// **N 个产出行至少需要 `5N-1` 字节**输入 —— 首行 4 字节（`<p>` 3 字节 + ≥1 字节文本），其后每行
// +5 字节（**裸 `<br>`** 4 字节 + ≥1 字节文本；`<br>` 与 `<br/>` 同样切分，且 `<p>` 外不产出）。
// 故 `N ≤ floor((maxBytes+1)/5)`：默认 1 MiB ⇒ 上限 **209,715** 行（实测最大同为 209,715）。
// 注意**不是** `maxBytes/5`（那会低估首行：`"<p>a"` 仅 4 字节却产 1 行），也**不是** `maxBytes`/8
// ——首行之后每行的最小单元是裸 `<br>`（5 字节/行），不是 `<p>x</p>`（8 字节）。
//
// `<translation>` 的配对**不在本任务范围**（范围由计划钉死为仅 `p`/`span` 与 `begin`；见证据
// `README.md` §3，留待 todo 38 回填）⇒ 本解析器不读取、不配对、不产出译文行，也不做时间继承。
//
// 编码回退复用 decodeLyricsBytes（与 `.txt`/`.srt`/`.ass` 同一条 6 步顺序）。全失败 ⇒ 空 lines +
// 一条 UnsupportedEncoding 错误（被跳过并计数），与 parseAssLyricsFile 同构。
//
// ⚠️ 本仓库与新代码均**无** TTML 参考实现或渲染真值（语料 `.ttml`/`.dfxp` = 0），唯一对齐对象是
// W3C 规格本身；不得声称与某实现或播放器一致。
[[nodiscard]] LrcParseResult parseTtmlLyrics(std::string text, const LrcParseOptions& options = {},
                                             std::optional<std::filesystem::path> path = std::nullopt);
[[nodiscard]] LrcParseResult parseTtmlLyricsFile(const std::filesystem::path& path,
                                                 const LrcParseOptions& options = {});

// 按扩展名把侧车路径分派到对应解析器：.lrc/.srt/.ass/.ttml/.txt。非侧车扩展名返回空结果。
// 只做「分派 + 解析」：`.txt` 的「非空且清洗后 ≤200 行」与同 basename 判据由扫描链的侧车决议层
// 施加（见 path_utils.h 的 acceptsPlainTextLyricsSidecar / resolveLyricsSidecarPath），不在此处。
[[nodiscard]] LrcParseResult parseLyricsSidecarFile(const std::filesystem::path& path,
                                                    const LrcParseOptions& options = {});

}
