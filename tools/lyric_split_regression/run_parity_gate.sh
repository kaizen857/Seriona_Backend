#!/usr/bin/env bash
# 歌词切分 Python↔C++ 语料一致性闸门（todo 3 的地基；CTest 目标在 todo 9 注册）。
#
# 退出码约定（与 CTest 的 SKIP_RETURN_CODE 对齐）：
#   0  = 一致
#   1  = 有差异（含语料哈希与清单不匹配、清单与 dump 行数不互证）
#   2  = 用法错误（缺少语料根参数）
#   77 = 跳过（语料缺失 / 清单缺失 / 尚未提供 C++ dump）——**跳过绝不等同于通过**
#
# 用法：run_parity_gate.sh <corpus-root> [<cpp-dump-executable>]
# C++ dump 的调用契约（todo 9 必须满足，与 Python 侧 CLI 同形）：
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

ROOT="${1:-}"
CPP_DUMP="${2:-}"

if [ -z "$ROOT" ]; then
  echo "用法：$(basename "$0") <corpus-root> [<cpp-dump-executable>]" >&2
  exit "$EXIT_USAGE"
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
if [ -z "$CPP_DUMP" ]; then
  echo "跳过：未提供 C++ dump 可执行文件（argv[2]）；C++ 侧工具在 todo 9 才存在"
  exit "$EXIT_SKIP"
fi
if [ ! -x "$CPP_DUMP" ]; then
  echo "跳过：C++ dump 不可执行或不存在：$CPP_DUMP"
  exit "$EXIT_SKIP"
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

total=$(grep -vc '^#' "$TMP/py.tsv")
if diff -u "$TMP/py.tsv" "$TMP/cpp.tsv" > "$TMP/diff.txt"; then
  echo "两端一致：total=$total same=$total removed=0 added=0"
  exit "$EXIT_SAME"
fi

# 差异计数必须同时反映两侧：`-` 是 Python 侧独有行、`+` 是 C++ 侧独有行。
# 只数 `-` 会在「C++ 多出行」时把 same 算多（退出码不受影响，但打印数字误导）。
removed=$(grep -c '^-[^-]' "$TMP/diff.txt")
added=$(grep -c '^+[^+]' "$TMP/diff.txt")
same=$((total - removed))
if [ "$same" -lt 0 ]; then
  same=0
fi
echo "两端有差异：total=$total same=$same removed=$removed added=$added"
echo "按文件聚合的差异行数（两侧合并，前 $TOP_N）："
grep -E '^(-|\+)' "$TMP/diff.txt" | grep -vE '^(---|\+\+\+)' | sed 's/^[-+]//' \
  | cut -f1 | sort | uniq -c | sort -rn | head -n "$TOP_N" | sed 's/^/    /'
echo "差异样例（前 $TOP_N 行）："
head -n "$TOP_N" "$TMP/diff.txt" | sed 's/^/    /'
exit "$EXIT_DIFF"
