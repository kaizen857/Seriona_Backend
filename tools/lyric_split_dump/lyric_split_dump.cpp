// 歌词切分 C++ dump 指纹工具（开发期 Python↔C++ 语料一致性闸门的 C++ 侧）。
//
// 用途：把整个语料根下所有 `.lrc` 的逐行切分结果按与 Python 参考实现
//   tools/lyric_split_regression/lyric_split_tool.py --dump-tsv
// **逐字节相同**的 7 列 TSV 指纹写到 stdout，供 run_parity_gate.sh 两端 diff。
//
// 硬约束（改动前必读）：
//   1. 切分/清洗**必须复用** src/control/lyric_split.h 的 splitDocument —— 闸门要证明
//      「同一实现两端一致」，另写一套切分只会让闸门在比对两个不同实现。
//   2. 编码回退的**顺序与集合**必须与 Python `read_text_file_with_codec` 逐元素相同：
//      `utf-8-sig / utf-8 / gb18030 / big5 / shift_jis / utf-16`、首个成功即采信、
//      全失败则跳过并计数。**严格失败**用 ICU `UCNV_TO_U_CALLBACK_STOP`（默认的
//      SUBSTITUTE 会产出 U+FFFD 并继续，与 Python 抛 UnicodeDecodeError 的语义不符）。
//      ★「顺序/集合相同」**不等于**「编解码器语义相同」：ICU 的 gb18030 / Big5 /
//      Shift_JIS 与 Python 的同名 codec 在**部分字节序列**上给出不同结果（实测
//      `gb18030 A3A0`：Python U+E5E5 ↔ ICU U+3000；Big5/Shift_JIS 亦有映射差异）。
//      该差异已在 `evidence/task-9-.../divergence-ledger.md` 登记为**已裁决分歧**。
//      ★ 闸门契约是**输出行等价**（两端 7 列 TSV 逐字节相同），**不是**「解码逐字节
//      等价」：`decode_by_codec` 相同**不代表**解出的文本相同；解码差异若只落在
//      `cleanLine` 丢弃的行（或只落在两端同名的首行字段）上，闸门可**静默通过**。
//      反例：gb18030 `[ti:\xa3\xa0]`，两端均丢弃该行 ⇒ diff 空 ⇒ exit 0
//      （冻结语料实测两端**原始解码文本**逐字节相同 ⇒ 当前无已知未裁决的静默分歧；
//      见台账 F1 条目）。**不得**在此声称与 Python 逐字/逐字节等价；若要把该解码下沉
//      为产品读取路径，必须另行裁决（见 decode-decision.md 末尾给 todo 16 的提示）。
//   3. 行切分必须等价 Python `str.splitlines()`（CRLF 视为一个分隔符；VT/FF/FS/GS/RS/
//      NEL/LS/PS 也都是分隔符；结尾分隔符不产生尾随空行）。
//   4. 列 3/6/7 的转义**顺序不可换**：先 `\`→`\\`，再 TAB→`\t`，再 LF→`\n`。

#include "seriona/control/control_contracts.h"

#include "control/lyric_split.h"
#include "scanner/path_utf8.h"   // 路径文本进 TSV 前必须经 pathToUtf8（仓库硬约束）

#include <unicode/ucnv.h>
#include <unicode/ustring.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace control = seriona::control;
namespace scanner = seriona::scanner;

// 必须与 Python 参考实现的 TEXT_ENCODINGS（lyric_split_tool.py 的 `TEXT_ENCODINGS =`）
// 逐元素相同：首个成功即采信，顺序变了 `decode_by_codec=` 的首行字节就会变。
// `utf-8-sig` 在首位 ⇒ 无 BOM 的 UTF-8 也一律报 `utf-8-sig`（刻意的确定性口径）。
inline constexpr std::array<std::string_view, 6> kTextEncodings{
    "utf-8-sig", "utf-8", "gb18030", "big5", "shift_jis", "utf-16"};

// ICU 转换器名。**仅编码回退的顺序/集合**与 Python `TEXT_ENCODINGS` 逐元素对齐；
// ICU 的 gb18030 / Big5 / Shift_JIS 编解码表与 Python 同名 codec **不等价**（已裁决分歧。
// 闸门契约为**输出行等价**：该差异可表现为失败，也可在被 `cleanLine` 丢弃的行上**静默通过**
// —— 详见文件头硬约束 2 与 divergence-ledger.md 的 F1 条目）。
inline constexpr std::array<const char*, 4> kIcuNames{"UTF-8", "gb18030", "Big5", "Shift_JIS"};

// ── ICU 严格解码 ─────────────────────────────────────────────────────────────

// 用 UCNV_TO_U_CALLBACK_STOP 把 bytes 严格解码为 UTF-8；遇到非法/截断字节返回 false。
bool decodeStrict(std::string_view bytes, const char* icuName, std::string& utf8Out) {
  UErrorCode status = U_ZERO_ERROR;
  UConverter* converter = ucnv_open(icuName, &status);
  if (U_FAILURE(status)) {
    return false;
  }
  status = U_ZERO_ERROR;
  ucnv_setToUCallBack(converter, UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &status);
  if (U_FAILURE(status)) {
    ucnv_close(converter);
    return false;
  }

  const auto srcLen = static_cast<int32_t>(bytes.size());
  status = U_ZERO_ERROR;
  const int32_t need = ucnv_toUChars(converter, nullptr, 0, bytes.data(), srcLen, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    ucnv_close(converter);
    return false;
  }
  status = U_ZERO_ERROR;
  std::vector<UChar> utf16(static_cast<std::size_t>(need) + 1);
  const int32_t written = ucnv_toUChars(converter, utf16.data(), static_cast<int32_t>(utf16.size()),
                                        bytes.data(), srcLen, &status);
  ucnv_close(converter);
  if (U_FAILURE(status)) {
    return false;
  }

  status = U_ZERO_ERROR;
  int32_t utf8Need = 0;
  u_strToUTF8(nullptr, 0, &utf8Need, utf16.data(), written, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    return false;
  }
  status = U_ZERO_ERROR;
  utf8Out.assign(static_cast<std::size_t>(utf8Need), '\0');
  u_strToUTF8(utf8Out.data(), utf8Need, nullptr, utf16.data(), written, &status);
  return U_SUCCESS(status);
}

// 字节序列是否以给定的 2 字节前缀开头。
bool hasPrefix2(std::string_view bytes, unsigned char first, unsigned char second) {
  return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == first &&
         static_cast<unsigned char>(bytes[1]) == second;
}

// utf-16 分支：Python `bytes.decode('utf-16')` 有 BOM 则按 BOM 判定并**剥掉** BOM，
// 无 BOM 则按**本机字节序**。ICU 的 "UTF-16" 无 BOM 时默认大端（与 Python 不符），
// 故按 BOM/本机序显式选用 UTF-16LE / UTF-16BE（见 decode-decision.md）。
bool decodeUtf16(std::string_view bytes, std::string& utf8Out) {
  std::string_view body = bytes;
  const char* converter = nullptr;
  if (hasPrefix2(body, 0xFF, 0xFE)) {
    body.remove_prefix(2);
    converter = "UTF-16LE";
  } else if (hasPrefix2(body, 0xFE, 0xFF)) {
    body.remove_prefix(2);
    converter = "UTF-16BE";
  } else if constexpr (std::endian::native == std::endian::big) {
    converter = "UTF-16BE";
  } else {
    converter = "UTF-16LE";
  }
  return decodeStrict(body, converter, utf8Out);
}

// 按 TEXT_ENCODINGS 顺序严格尝试，首个成功即采信。
// 返回 nullopt 表示全失败（调用方跳过该文件并计入 unresolved_decode）。
std::optional<std::pair<std::string, std::string_view>> decodeWithFallback(
    std::string_view bytes) {
  // ★ utf-16 BOM 预判直达，与 Python **精确**等价（证明见 evidence/task-9/qa/f4-equivalence.out.txt）。
  //   Python 侧（实测 32 组 FF FE/FE FF 输入）：五个前序 codec 一律拒绝这类首字节 ⇒ Python
  //   必落**末步** utf-16，且回退结果 == 直接 `decode('utf-16')`（含 utf-16 自身失败 ⇒ 两端
  //   同判 unresolved）。末步性质 ⇒ 「预判直达」与「按序尝试」在**所有**此类输入上同结果
  //   （含仅 2 字节 BOM、奇数长度尾部、未配对代理）。
  //   必要性（C++ 侧与 Python 不同）：ICU 的 Big5 视 0xFF 为合法前导字节，`FF FE` + **ASCII
  //   载荷**能整段被 Big5「成功」解出（实测 `FF FE 5B 00 74 00 …` → Big5 ACCEPT，乱码）；
  //   不预判则这些文件**永远到不了** utf-16（原缺陷 F4：`FF FE` 文件报 `decode_by_codec=big5=1`）。
  //   **判别性取决于载荷**：载荷含 CJK（如 `FF FE A2 30 …`）时 Big5 因配对非法而拒收，无预判
  //   也照样落到 utf-16 —— 故回归夹具特意用 `FF FE` + 纯 ASCII 的 u16le.lrc 作判别样本。
  if (hasPrefix2(bytes, 0xFF, 0xFE) || hasPrefix2(bytes, 0xFE, 0xFF)) {
    std::string decoded;
    if (decodeUtf16(bytes, decoded)) {
      return std::pair{std::move(decoded), kTextEncodings[5]};
    }
    return std::nullopt;
  }

  // utf-8-sig：剥掉开头的 UTF-8 BOM（若有），其余与 utf-8 相同。
  constexpr std::string_view utf8Bom("\xEF\xBB\xBF", 3);
  for (std::string_view encoding : kTextEncodings) {
    std::string decoded;
    if (encoding == kTextEncodings[0]) {
      std::string_view body = bytes;
      if (body.size() >= utf8Bom.size() && body.substr(0, utf8Bom.size()) == utf8Bom) {
        body.remove_prefix(utf8Bom.size());
      }
      if (decodeStrict(body, kIcuNames[0], decoded)) {
        return std::pair{std::move(decoded), encoding};
      }
    } else if (encoding == kTextEncodings[1]) {
      if (decodeStrict(bytes, kIcuNames[0], decoded)) {
        return std::pair{std::move(decoded), encoding};
      }
    } else if (encoding == kTextEncodings[2]) {
      if (decodeStrict(bytes, kIcuNames[1], decoded)) {
        return std::pair{std::move(decoded), encoding};
      }
    } else if (encoding == kTextEncodings[3]) {
      if (decodeStrict(bytes, kIcuNames[2], decoded)) {
        return std::pair{std::move(decoded), encoding};
      }
    } else if (encoding == kTextEncodings[4]) {
      if (decodeStrict(bytes, kIcuNames[3], decoded)) {
        return std::pair{std::move(decoded), encoding};
      }
    } else {
      // utf-16：见 decodeUtf16 的 BOM/本机序语义。此分支只在**无** BOM 前缀时才可能被
      // 走到（有 BOM 的输入已在函数开头的预判里直达并返回）。
      if (decodeUtf16(bytes, decoded)) {
        return std::pair{std::move(decoded), encoding};
      }
    }
  }
  return std::nullopt;
}

// ── Python str.splitlines() 等价 ─────────────────────────────────────────────

bool isPythonLineBreak(char32_t cp) {
  return cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x1C || cp == 0x1D ||
         cp == 0x1E || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
}

// 解一个码点（输入来自 ICU，保证是合法 UTF-8）。
std::pair<char32_t, std::size_t> decodeCodePoint(std::string_view text, std::size_t index) {
  const auto lead = static_cast<unsigned char>(text[index]);
  std::size_t length = 1;
  char32_t cp = lead;
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    cp = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    cp = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    cp = lead & 0x07U;
  } else {
    return {cp, 1};
  }
  for (std::size_t offset = 1; offset < length && index + offset < text.size(); ++offset) {
    cp = (cp << 6U) | (static_cast<unsigned char>(text[index + offset]) & 0x3FU);
  }
  return {cp, length};
}

// 等价 CPython 的 str.splitlines()：CRLF 视作单个分隔符、结尾分隔符不产生尾行。
std::vector<std::string> pythonSplitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  std::size_t index = 0;
  while (index < text.size()) {
    const auto [cp, length] = decodeCodePoint(text, index);
    if (!isPythonLineBreak(cp)) {
      index += length;
      continue;
    }
    const std::size_t breakAt = index;
    index += length;
    if (cp == 0x0D && index < text.size()) {
      const auto [next, nextLength] = decodeCodePoint(text, index);
      if (next == 0x0A) {
        index += nextLength;
      }
    }
    lines.emplace_back(text.substr(start, breakAt - start));
    start = index;
  }
  if (start < text.size()) {
    lines.emplace_back(text.substr(start));
  }
  return lines;
}

// ── TSV 序列化 ───────────────────────────────────────────────────────────────

// 与 Python `_escape_tsv` 逐字节等价。单趟替换等价于「先 `\`→`\\`、再 TAB→`\t`、
// 再 LF→`\n`」的串联（后两步插入的反斜杠不会反过来被前一步处理）。
std::string escapeTsv(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

// ── 曲库遍历 ─────────────────────────────────────────────────────────────────

bool hasLrcSuffix(const fs::path& path) {
  const std::string name = scanner::pathToUtf8(path.filename());
  if (name.size() < 4) {
    return false;
  }
  std::string tail = name.substr(name.size() - 4);
  for (char& ch : tail) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return tail == ".lrc";
}

struct CorpusEntry {
  std::string relativePath;
  fs::path absolutePath;
};

// 递归收集 `.lrc`，相对路径用 `/` 分隔，按相对路径字典序排序（与 Python
// iter_lrc_files + run_dump_tsv 的 entries.sort 口径一致）。
//
// **必须复刻 `os.walk` 的 filenames 语义，不能按「是否普通文件」筛选**：`os.walk` 把
// `entry.is_dir()`（**跟随**符号链接）为假的每个条目都放进 `filenames`，随后
// `read_text_file_with_codec` 的 `open()` 若抛 `OSError` 就计入 `unresolved_decode`。
// 故断链符号链接 / FIFO / socket 等**非普通文件**的 `*.lrc` 也必须被收集（由
// `readFileBytes` 打开失败后走 `++unresolvedDecode`），只有**目录**才不入 filenames。
// （曾按 `is_regular_file` 筛选 ⇒ 断链 `.lrc` 被静默丢弃，与 Python 的
//  `unresolved_decode` 分歧。见证据 t1-before-fix.out.txt。）
std::vector<CorpusEntry> collectLrcFiles(const fs::path& root) {
  std::vector<CorpusEntry> entries;
  // `skip_permission_denied` 让不可读目录**跳过其子树后继续**遍历，对齐
  // `os.walk(onerror=None)`（无此选项时 recursive_directory_iterator 遇不可读目录会
  // 直接**终止整个遍历**，表现为文件列表被静默截断 —— 见证据 m2-before/after.out.txt）。
  // 注意**不能**依赖 `increment(ec)` 之后在循环体里纠错：出错时 increment 会把迭代器
  // 一并置为 end，`it != end` 先为假 ⇒ 循环体分支不可达（曾是死代码）。
  const fs::directory_options options = fs::directory_options::skip_permission_denied;
  std::error_code iteratorError;
  fs::recursive_directory_iterator it(root, options, iteratorError);
  const fs::recursive_directory_iterator end;
  while (it != end) {
    const fs::directory_entry entry = *it;
    // 查询状态的 error_code 必须与遍历用的 iteratorError **分开**：断链符号链接的
    // `is_directory` 返回 false 并置 ENOENT，若把它写回 iteratorError 会污染下一次
    // `increment`。非目录 ⇒ 属 filenames；recursive_directory_iterator 不跟随目录
    // 符号链接，与 `os.walk(followlinks=False)` 一致。
    std::error_code statusError;
    if (!entry.is_directory(statusError) && hasLrcSuffix(entry.path())) {
      entries.push_back(
          CorpusEntry{scanner::pathToUtf8(entry.path().lexically_relative(root)), entry.path()});
    }
    iteratorError.clear();
    it.increment(iteratorError);
    if (iteratorError) {
      // 其它遍历错误（非权限类）：increment 已终止遍历，这里显式收尾。
      // 不可读目录已由 skip_permission_denied 覆盖，故不构成 M2 的截断路径。
      break;
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const CorpusEntry& lhs, const CorpusEntry& rhs) {
              return lhs.relativePath < rhs.relativePath;
            });
  return entries;
}

std::optional<std::string> readFileBytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int runDumpTsv(const fs::path& root, std::string_view targetLanguage) {
  const std::vector<CorpusEntry> entries = collectLrcFiles(root);

  std::vector<std::string> rows;
  std::array<std::size_t, kTextEncodings.size()> codecCounts{};
  std::size_t files = 0;
  std::size_t totalLines = 0;
  std::size_t unresolvedDecode = 0;

  for (const CorpusEntry& entry : entries) {
    const std::optional<std::string> bytes = readFileBytes(entry.absolutePath);
    if (!bytes.has_value()) {
      ++unresolvedDecode;
      continue;
    }
    const auto decoded = decodeWithFallback(*bytes);
    if (!decoded.has_value()) {
      ++unresolvedDecode;
      continue;
    }
    const std::string_view codec = decoded->second;
    for (std::size_t index = 0; index < kTextEncodings.size(); ++index) {
      if (kTextEncodings[index] == codec) {
        ++codecCounts[index];
        break;
      }
    }
    ++files;

    const std::vector<std::string> rawLines = pythonSplitLines(decoded->first);
    const control::LyricDocumentSplit document =
        control::splitDocument(rawLines, scanner::pathToUtf8(entry.absolutePath), targetLanguage);
    const std::string convention =
        escapeTsv(control::conventionToken(document.convention));
    for (std::size_t index = 0; index < document.results.size(); ++index) {
      const control::LyricSplitResult& result = document.results[index].second;
      ++totalLines;
      rows.push_back(entry.relativePath + "\t" + std::to_string(index + 1) + "\t" + convention +
                     "\t" + escapeTsv(result.reason) + "\t" + escapeTsv(result.confidence) + "\t" +
                     escapeTsv(result.original) + "\t" + escapeTsv(result.translation));
    }
  }

  std::string codecPart;
  for (std::size_t index = 0; index < kTextEncodings.size(); ++index) {
    if (codecCounts[index] == 0) {
      continue;
    }
    if (!codecPart.empty()) {
      codecPart += ",";
    }
    codecPart += std::string(kTextEncodings[index]) + "=" + std::to_string(codecCounts[index]);
  }

  std::string output = "# total_lines=" + std::to_string(totalLines) +
                       " files=" + std::to_string(files) +
                       " unresolved_decode=" + std::to_string(unresolvedDecode) +
                       " decode_by_codec=" + codecPart + "\n";
  for (const std::string& row : rows) {
    output += row;
    output += "\n";
  }
  std::fwrite(output.data(), 1, output.size(), stdout);
  return 0;
}

// Python 侧 `--target` 的 choices 就是 TARGET_CLASS 的键（`zh/ja/ko/en`）。
bool isSupportedTarget(std::string_view target) {
  return target == "zh" || target == "ja" || target == "ko" || target == "en";
}

}  // namespace

int main(int argc, char** argv) {
  std::string root;
  std::string targetLanguage = "zh";  // 与 Python run_dump_tsv 的 --target 默认一致
  bool dumpTsv = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--root" && index + 1 < argc) {
      root = argv[++index];
    } else if (argument == "--target" && index + 1 < argc) {
      targetLanguage = argv[++index];
    } else if (argument == "--dump-tsv") {
      dumpTsv = true;
    } else {
      std::fprintf(stderr, "未知参数：%s\n用法：%s --root <corpus-root> --dump-tsv\n", argv[index],
                   argv[0]);
      return 2;
    }
  }

  if (!isSupportedTarget(targetLanguage)) {
    std::fprintf(stderr, "错误：--target 只接受 zh/ja/ko/en，收到 %s\n用法：%s --root <corpus-root> --dump-tsv\n",
                 targetLanguage.c_str(), argv[0]);
    return 2;
  }

  if (root.empty() || !dumpTsv) {
    std::fprintf(stderr, "用法：%s --root <corpus-root> --dump-tsv\n", argv[0]);
    return 2;
  }
  if (!fs::is_directory(root)) {
    std::fprintf(stderr, "错误：语料根不是目录 %s\n", root.c_str());
    return 2;
  }

  const fs::path rootPath = fs::absolute(fs::path(root)).lexically_normal();
  return runDumpTsv(rootPath, targetLanguage);
}
