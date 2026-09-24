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
  —— 供闸门断言「两端看到同一批输入」并能互证**解码**一致（`decode_by_codec` 按解码尝试顺序列非零项）。
- 用途/退出码：本模式**只读输出**，退出码 `0`；它**不改变**任何既有模式的输出。

### 语料一致性闸门 `run_parity_gate.sh`

`./run_parity_gate.sh <corpus-root> [<cpp-dump-executable>]` 按 `corpus-manifest.tsv` 校验文件哈希 →
跑 Python `--dump-tsv` → 跑 C++ dump（`<cpp-dump> --root <root> --dump-tsv`）→ `diff` → 打印
`total/same/diff`（差异时另打印 `removed`/`added` 与按文件聚合的前 N 条）。
**退出码约定**：`0`=一致 / `1`=有差异（含哈希不匹配、清单↔dump 行数不互证）/ `2`=用法错误 /
`77`=**跳过**（语料缺失、清单缺失、未提供或不可执行 C++ dump）—— **跳过绝不等同于通过**。

`corpus-manifest.tsv`：头部注释含所用哈希命令（`xxhsum -H3`）+ 每条 `相对路径(POSIX)\tXXH3 哈希\t清洗后行数`，
**只含路径/哈希/行数，不含歌词正文**。C++ dump 工具在后续 todo 落地；本目录的 CTest 目标亦在其后注册。

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
