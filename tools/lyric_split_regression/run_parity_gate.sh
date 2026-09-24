#!/usr/bin/env bash
# 歌词切分 Python↔C++ 语料一致性闸门（CTest 目标 seriona.lyric_split.corpus_parity，
# C++ 侧是 Seriona_Backend/CMakeLists.txt 顶部构建的 seriona_lyric_split_dump）。
#
# 退出码约定（与 tests/CMakeLists.txt 的 SKIP_RETURN_CODE 对齐）：
#   0  = 一致（两端 7 列 TSV 指纹逐字节相同）
#   1  = 失败：两端有差异（含语料哈希与清单不匹配、清单与 dump 行数不互证），
#        **或**语料已存在但 C++ dump 缺失/不可执行（dump 是闸门的一半，缺它只能判失败）
#   2  = 用法错误（参数过多）
#   77 = 跳过（恰有 4 个出口，与下方分支逐一对应）：
#        ①未提供语料根（argv[1] 空且 SERIONA_LYRICS_CORPUS 未设）
#        ②语料根不是目录 ③清单 corpus-manifest.tsv 缺失 ④语料目录下没有 .lrc
#        ——**跳过绝不等同于通过**（语料已确认存在后的 dump 缺失属 1，见上）
#
# 用法：run_parity_gate.sh [<corpus-root>] [<cpp-dump-executable>]
#   - <corpus-root> 省略或为空时回退到环境变量 SERIONA_LYRICS_CORPUS：该变量**运行时**
#     读取，故同一构建树里设/清它后直接 ctest 即可在「实跑」与「Skipped」之间切换。
#   - <cpp-dump-executable> 省略或为空时回退到 SERIONA_LYRIC_SPLIT_DUMP。
# C++ dump 的调用契约（与 Python 侧 CLI 同形）：
#   "<cpp-dump>" --root <corpus-root> --dump-tsv   # 7 列 TSV 指纹写到 stdout
set -u

EXIT_SAME=0
EXIT_DIFF=1
EXIT_USAGE=2
EXIT_SKIP=77

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$HERE/lyric_split_tool.py"
MANIFEST="$HERE/corpus-manifest.tsv"
TOP_N="${TOP_N:-10}"

if [ "$#" -gt 2 ]; then
  echo "用法：$(basename "$0") [<corpus-root>] [<cpp-dump-executable>]" >&2
  exit "$EXIT_USAGE"
fi

ROOT="${1:-}"
if [ -z "$ROOT" ]; then
  ROOT="${SERIONA_LYRICS_CORPUS:-}"
fi
CPP_DUMP="${2:-${SERIONA_LYRIC_SPLIT_DUMP:-}}"

if [ -z "$ROOT" ]; then
  echo "跳过：未提供语料根（argv[1] 为空且环境变量 SERIONA_LYRICS_CORPUS 未设置）"
  exit "$EXIT_SKIP"
fi

if [ ! -d "$ROOT" ]; then
  echo "语料缺失，跳过：$ROOT 不是目录"
  exit "$EXIT_SKIP"
fi
if [ ! -f "$MANIFEST" ]; then
  echo "语料清单缺失，跳过：$MANIFEST"
  exit "$EXIT_SKIP"
fi
scanned=$(find "$ROOT" -iname '*.lrc' | wc -l)
if [ "$scanned" -eq 0 ]; then
  echo "语料缺失，跳过：$ROOT 下没有 .lrc"
  exit "$EXIT_SKIP"
fi
# 到这里语料根已确认存在：C++ dump 缺失/不可执行是**失败**而非跳过
# （跳过只保留给「语料侧缺失」，否则闸门可以靠删掉 dump 静默通过）。
if [ -z "$CPP_DUMP" ]; then
  echo "失败：语料已存在，但未提供 C++ dump 可执行文件（argv[2] 或 SERIONA_LYRIC_SPLIT_DUMP）"
  exit "$EXIT_DIFF"
fi
if [ ! -x "$CPP_DUMP" ]; then
  echo "失败：语料已存在，但 C++ dump 不可执行或不存在：$CPP_DUMP"
  exit "$EXIT_DIFF"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

checked=0
missing=0
mismatch=0
: > "$TMP/hashdiff.txt"
: > "$TMP/missing.txt"
while IFS=$'\t' read -r rel want _lines; do
  case "$rel" in
    ''|'#'*) continue ;;
  esac
  checked=$((checked + 1))
  target="$ROOT/$rel"
  if [ ! -f "$target" ]; then
    missing=$((missing + 1))
    printf '%s\n' "$rel" >> "$TMP/missing.txt"
    continue
  fi
  got=$(xxhsum -H3 -- "$target" | awk '{print $1}')
  if [ "$got" != "$want" ]; then
    mismatch=$((mismatch + 1))
    printf '%s\t清单=%s\t磁盘=%s\n' "$rel" "$want" "$got" >> "$TMP/hashdiff.txt"
  fi
done < "$MANIFEST"

if [ "$missing" -gt 0 ] || [ "$mismatch" -gt 0 ]; then
  echo "语料与清单不一致：共 $checked 条，缺失 $missing，哈希不匹配 $mismatch"
  if [ -s "$TMP/missing.txt" ]; then
    echo "  缺失（前 $TOP_N 条）："
    head -n "$TOP_N" "$TMP/missing.txt" | sed 's/^/    /'
  fi
  if [ -s "$TMP/hashdiff.txt" ]; then
    echo "  哈希不匹配（前 $TOP_N 条）："
    head -n "$TOP_N" "$TMP/hashdiff.txt" | sed 's/^/    /'
  fi
  exit "$EXIT_DIFF"
fi
echo "清单哈希校验：$checked/$checked 条通过"

if ! python3 "$TOOL" --dump-tsv --root "$ROOT" > "$TMP/py.tsv"; then
  echo "Python --dump-tsv 执行失败"
  exit "$EXIT_DIFF"
fi

if ! "$CPP_DUMP" --root "$ROOT" --dump-tsv > "$TMP/cpp.tsv"; then
  echo "C++ dump 执行失败：$CPP_DUMP"
  exit "$EXIT_DIFF"
fi

manifest_lines=$(awk -F'\t' '!/^#/{s+=$3} END{print s+0}' "$MANIFEST")
dump_lines=$(sed -n '1p' "$TMP/py.tsv" | sed -n 's/.*total_lines=\([0-9][0-9]*\).*/\1/p')
if [ "$manifest_lines" != "$dump_lines" ]; then
  echo "清单与 dump 不互证：清单清洗后行数=$manifest_lines，Python dump 首行 total_lines=$dump_lines"
  exit "$EXIT_DIFF"
fi
echo "清单↔dump 行数互证：$manifest_lines 行"

# 解码失败文件数与总行数：从 Python dump 的**首行注释**（todo 3 定义的唯一权威格式）解析，
# 显式打印供人核对（只加打印，不改判定逻辑/退出码）。
header_unresolved=$(sed -n '1p' "$TMP/py.tsv" \
  | sed -n 's/.*unresolved_decode=\([0-9][0-9]*\).*/\1/p')
header_total_lines=$(sed -n '1p' "$TMP/py.tsv" \
  | sed -n 's/.*total_lines=\([0-9][0-9]*\).*/\1/p')
echo "解码失败文件数=$header_unresolved；总行数=$header_total_lines"

# 数据行计数：剥掉首行注释后用 `wc -l` 数**行**。**不要**用 `grep -vc '^#'` —— 数据行
# 第 1 列（相对路径）未转义，以 `#` 开头的文件名会被误当注释而漏计。
total=$(sed '1d' "$TMP/py.tsv" | wc -l | tr -d ' ')

# LC_ALL=C：让 diff 的告警文本（尤其 `Binary files … differ`）与语言环境无关，
# 报告与「二进制差异」检测都稳定可复现。
if LC_ALL=C diff -u "$TMP/py.tsv" "$TMP/cpp.tsv" > "$TMP/diff.txt"; then
  echo "两端一致：total=$total same=$total removed=0 added=0"
  exit "$EXIT_SAME"
fi

# 差异计数改用 `diff` 自身的格式化输出（每侧每行输出一个标记行再 `wc -l`），
# **不依赖**对 `diff` 输出做 `grep` 行首匹配：以 `-`/`+`/`#` 开头的**数据行**不会被
# 漏计或误判。`-a`（--text）让含 NUL 的二进制载荷也能给出逐行计数，避免出现
# 「有差异却说 removed=0 added=0」的自相矛盾（判定门槛不变：仍以 diff 为空为唯一通过条件）。
removed=$(diff -a --unchanged-line-format= --old-line-format=$'x\n' --new-line-format= \
  "$TMP/py.tsv" "$TMP/cpp.tsv" | wc -l | tr -d ' ')
added=$(diff -a --unchanged-line-format= --old-line-format= --new-line-format=$'x\n' \
  "$TMP/py.tsv" "$TMP/cpp.tsv" | wc -l | tr -d ' ')
# 计数口径是**文件全部行**（含首行注释），故基准也用文件行数而非 total（数据行）；
# 这样 same+removed=py_lines、same+added=cpp_lines 恒成立，报告不会自相矛盾。
# （首行注释本身可能变 —— 总行数/解码失败数/编解码分布 —— 它是真实差异，必须计入。）
py_lines=$(wc -l < "$TMP/py.tsv" | tr -d ' ')
cpp_lines=$(wc -l < "$TMP/cpp.tsv" | tr -d ' ')
same=$((py_lines - removed))
if [ "$same" -lt 0 ]; then
  same=0
fi
echo "两端有差异：total=$total py_lines=$py_lines cpp_lines=$cpp_lines same=$same removed=$removed added=$added"

# 二进制载荷：`diff -u` 默认只报「Binary files … differ」，不给可裁决的行。此时显式提示，
# 并说明上面的计数来自 `diff -a`（逐行），以免读者以为报告缺失。
if grep -q '^Binary files .* differ' "$TMP/diff.txt"; then
  echo "提示：差异含二进制载荷（diff 默认报「Binary files … differ」，无逐行样例）；"
  echo "      上面的 removed/added 由 diff -a（--text）逐行统计得出。"
fi

echo "按文件聚合的差异行数（两侧合并，前 $TOP_N）："
diff -a --unchanged-line-format= --old-line-format='%L' --new-line-format='%L' \
  "$TMP/py.tsv" "$TMP/cpp.tsv" | cut -f1 | sort | uniq -c | sort -rn \
  | head -n "$TOP_N" | sed 's/^/    /'
echo "差异样例（前 $TOP_N 行）："
head -n "$TOP_N" "$TMP/diff.txt" | sed 's/^/    /'
exit "$EXIT_DIFF"
