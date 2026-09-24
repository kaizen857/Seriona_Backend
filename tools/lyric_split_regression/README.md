# 歌词切分 · 开发期离线回归护栏与复核工具

> **开发期工具**：本目录**不参与产品构建**，**不随发行版分发**。
> 它没有任何 `CMakeLists.txt` 引用，也不会被安装或打包；只供开发者在真实曲库上做回归与人工复核。

对应设计文档 **D25（护栏已批准并落地）** 与 **P3（复核清单）**。算法口径与权威设计文档一致：
`spec/lyrics-original-translation-split-research-and-design-2026-09-19.md` §6.2 / §6.6。

## 用途

- 在**真实曲库**上把「算法切分边界」与「作者用 ` / ` 写出的边界」对照，作为**可重复的回归护栏**；
- 抽取**算法不确定或可能出错**的行，产出便于人工判定的清单，供人工核对后沉淀为测试用例。

## 四种模式

| 模式 | 用途 |
|---|---|
| `--regression` | 在真实曲库上度量「算法边界 vs 人工 ` / ` 边界」的精确率，作为可重复的回归护栏。退出码 `0`=通过 / `1`=未通过 / `2`=无可评估样本。 |
| `--review` | 抽取人工复核候选清单（markdown）。**文件级**类别（整首共用同一约定）只看样例行、对整首给一个结论；**行级**类别逐条判定。 |
| `--dump [过滤]` | 输出算法在**全部行**上的切分结果（每行原文/译文 + 判定依据）。`--max-rows N` 限制每文件行数；不加 `--out` 时打到标准输出。 |
| `--dump-tsv` | 输出 **7 列 TSV 行级指纹**，供 Python↔C++ 两端**逐字节**比对（见下）。不加 `--out` 时打到标准输出。 |

### `--dump-tsv` 的输出口径

数据行 7 列（TAB 分隔），语义如同表头顺序：

| # | 列 | 口径 |
|---|---|---|
| 1 | 相对路径 | 相对 `--root`，POSIX `/`，按字典序（Unicode 码点序）排序 |
| 2 | 行号 | `split_document` **清洗后**的 1-based 下标（**不是**原始文件行号） |
| 3 | 文档级约定 | `DocumentSplit.convention` 的规范记号：`S:`+强记号 / `W:`+弱记号 / `SCRIPT` / `-`（未推断出） |
| 4 | route/reason | 切分成功 = `SplitResult.reason`（`strong`/`strong-fallback`/`strong-bracket-fallback`/`strong-weak-fallback`/`bracket`/`weak`）；未切分 = `empty`/`qq-sentinel`/`no-convention`/`validate-failed` |
| 5 | 置信度 | `high` / `medium` / `none` |
| 6 | 原文 | `SplitResult.original`；未切分时 = 整行 |
| 7 | 译文 | `SplitResult.translation`；未切分时 = 空串 |

- 第 3/6/7 列做转义：**先 `\` → `\\`，再 TAB → `\t`、LF → `\n`**（顺序不可换）。
- **首行注释**（唯一权威定义，本行的格式即两端契约）：
  `# total_lines=<N> files=<M> unresolved_decode=<K> decode_by_codec=<codec>=<count>[,…]`
  —— 供闸门断言「两端看到同一批输入」（`decode_by_codec` 按解码尝试顺序列非零项）。
  ⚠️ 它**不**互证「解码文本一致」：`decode_by_codec` 相同**不代表**解出的文本相同
  （`gb18030 A3A0` 两端都报 `gb18030` 却解出 `U+3000` vs `U+E5E5`）。闸门契约为
  **输出行等价**（两端 7 列 TSV 逐字节相同），解码分歧若只落在被 `cleanLine` 丢弃的行上
  可**静默通过**（见 `evidence/task-9-…/divergence-ledger.md` 的 H1/F1 条目）。
- 用途/退出码：本模式**只读输出**，退出码 `0`；它**不改变**任何既有模式的输出。

### 语料一致性闸门 `run_parity_gate.sh`

`./run_parity_gate.sh <corpus-root> [<cpp-dump-executable>]` 按 `corpus-manifest.tsv` 校验文件哈希 →
跑 Python `--dump-tsv` → 跑 C++ dump（`<cpp-dump> --root <root> --dump-tsv`）→ `diff` → 打印
`total/same/diff`（差异时另打印 `removed`/`added` 与按文件聚合的前 N 条）。
**退出码约定**：`0`=一致；`1`=有差异/失败（含哈希不匹配、清单↔dump 行数不互证，**以及
「语料已存在但 C++ dump 未提供或不可执行」**）；`2`=用法错误（参数过多）；`77`=**跳过**，
**恰有 4 个出口**（与脚本逐一对应）：①未提供语料根（`argv[1]` 为空且 `SERIONA_LYRICS_CORPUS`
未设置）②语料根不是目录 ③清单 `corpus-manifest.tsv` 缺失 ④语料目录下没有 `.lrc`
—— **跳过绝不等同于通过**。
注意「dump 不可执行」是 **`1`（失败）而非 `77`**：语料既然在，缺 dump 就是环境/构建问题，
不能当作跳过而静默放行（与 `tests/CMakeLists.txt` 的 `SKIP_RETURN_CODE=77` 配合时，
只剩真正的「语料缺失」才 Skipped）。

`corpus-manifest.tsv`：头部注释含所用哈希命令（`xxhsum -H3`）+ 每条 `相对路径(POSIX)\tXXH3 哈希\t清洗后行数`，
**只含路径/哈希/行数，不含歌词正文**。

### C++ 侧 dump 工具 `seriona_lyric_split_dump`

闸门需要与 Python 端**逐字节相同**的 C++ 实现，其源在
`tools/lyric_split_dump/lyric_split_dump.cpp`（`SERIONA_BUILD_TESTS=ON` 时构建，
输出 `build/seriona_lyric_split_dump`；开发期工具，不进发行版）。

```
seriona_lyric_split_dump --root <corpus-root> --dump-tsv [--target <zh|ja|ko|en>]
```

- **必须同时给 `--root` 与 `--dump-tsv`**，否则打印用法并退出 `2`。
- `--target` 取值与 Python 端 `TARGET_CLASS` 的键一致，**仅 `zh/ja/ko/en`**；其它值打印用法并
  退出 `2`（与 Python `parser.error` 对齐）；缺省 `zh`。
- 它**复用** `src/control/lyric_split.h` 的 `splitDocument`（同一份切分实现），只自持解码回退
  与 TSV 序列化；解码回退的**顺序/集合**与 Python `TEXT_ENCODINGS` 逐元素相同，但 ICU 编解码表
  与 Python 同名 codec **不等价**（已登记分歧，见 `evidence/task-9-…/divergence-ledger.md`）。
- CTest：`seriona.lyric_split.corpus_parity` 在语料存在时跑闸门；另有
  `seriona.lyric_split.dump_utf16_bom` 用 `tests/fixtures/lyric_split_utf16/` 离线钉住
  UTF-16 BOM 解码（不依赖语料）。

## 用法

```bash
# 扫描并回归
python3 lyric_split_tool.py --root ~/Music --regression

# 抽取复核清单
python3 lyric_split_tool.py --root ~/Music --review --out review.md

# 查看某曲的逐行运算结果
python3 lyric_split_tool.py --root ~/Music --dump '霞む夏の灯'

# 全库 7 列 TSV 指纹（供两端逐字节比对）
python3 lyric_split_tool.py --root ~/Music --dump-tsv --out ~/fingerprint.tsv

# 语料一致性闸门（哈希校验 → 两端 dump → diff）
./run_parity_gate.sh ~/Music /path/to/cpp-dump

# 同时做
python3 lyric_split_tool.py --root ~/Music --regression --review --out review.md
```

`--root` 必填；不指定任何模式时工具走 `parser.error` 并以非 0 退出（参数校验）。

## 门槛（`--regression`）

- **判定精确率 ≥ 98%**（`--min-agree`，默认 `98.0`）
- **不一致率 ≤ 0.1%**（`--max-disagree`，默认 `0.1`）

精确率口径为 `一致 / (一致 + 不一致)`，**只统计可作为客观对照的样本**。

## 为什么必须打印 `proxy_invalid` 与 `uncovered`

护栏的对照集来自机械抽取的 ` / ` 边界，这个代理**自身会畸形**、也**有覆盖不到的行**。
若不把这两类显式打印出来，「精确率 100%」会被误读为「全库干净」——而这恰恰是护栏最危险的自欺（D25：**护栏必须能看见缺陷，而不只是断言通过**）。

- **`proxy_invalid`（代理边界无效）**：首个 ` / ` 落在配对括号内部、或译文以分隔符字符开头等**代理自身畸形**的行。
  其机械抽取的「人工边界」并不代表作者意图（会把注音区 `（くも / ）` 切成译文以 `）` 开头）。
  这类行**不可作对照样本**，否则会把算法的**正确**切分误判为不一致（实测 9 + 17 行假阳性）。**单独报告、排除出对照集。**
- **`uncovered`（对照盲区）**：**没有 ` / ` 标注**的行，连客观 oracle 都没有，因此**不计入任何指标**。
  该类行只能靠「切点落在括号内」这一客观判据与**人工复核**覆盖。实测盲区规模达 **5,530 行**——
  隐藏其中的真实错切（如 31 行）正是由用户人工复核发现的。
  **把盲区规模本身打印进报告**，才能让「精确率」自带适用范围。

此外 `--regression` 还会单独打印**「切点落在括号内」计数**（违反 D20 的客观判据）。
该计数**不计入门槛**（如何处理尚待裁决 P10），但必须显式可见。

## 目录

```
lyric_split_regression/
├── lyric_split_tool.py     # 工具本体（开发期，已入库）
├── corpus-manifest.tsv     # 语料清单：相对路径 + XXH3 哈希 + 清洗后行数（不含正文）
├── run_parity_gate.sh      # Python↔C++ 语料一致性闸门（退出码 0/1/2/77）
├── README.md               # 本文件
├── .gitignore              # 忽略 __pycache__/、*.pyc
└── spec/                   # 规格与人工判定的仓库内副本（见 spec/README.md）
    ├── lyrics-original-translation-split-research-and-design-2026-09-19.md
    ├── human-verdicts.tsv
    └── README.md
```
