# spec/ — 唯一权威依据与人工判定的仓库内副本

> 本目录把**此前只存在于非 Git 目录**的权威材料纳入版本管理。
> 背景：工作区根 `/home/kaizen857/Seriona/`（以及 `/home/kaizen857/`）**不是 Git 仓库**，
> 因此根级的 `docs/`、`review*.md`、`bracket-interior-72-report.md` **无法被提交**；
> 一次误删或移动，规格与全部人工判定会同时消失。故在此留副本。

## 内容

| 文件 | 说明 |
|---|---|
| `lyrics-original-translation-split-research-and-design-2026-09-19.md` | **权威依据的仓库内副本**。设计文档 1764 行；算法口径的最终来源。与根级 `docs/` 原件逐字节一致。 |
| `human-verdicts.tsv` | 从 5 份根级人工复核清单抽取、**脱敏**后的结构化判定（206 条，见下）。 |
| `README.md` | 本文件：口径、脱敏政策、可复现性记录。 |

设计文档副本 SHA-256（与原件一致）：
`81ed4e05d58279f2f04d7d762de69d9b54eec7fadf6885efd3e0ccc799ac9111`

## `human-verdicts.tsv` 口径

列：`line_hash  expected  actual  round  filled  blank  still_pending  note`

- `line_hash` = `sha256(被核对曲目路径)[:16]`；
- `expected` = 该条目**算法推断的约定**（算法输出，非歌词）；
- `actual` = 「实际情况：」行原文（判定标签；空模板置空）；
- `round` ∈ `review | round1 | round2 | round3 | round4`；
- `filled`/`blank` 为互斥标记（`filled+blank == 1`）；
- `still_pending=是` 表示该条仍待复核。

### ★ 只有 `filled` 行可作 Oracle

`blank` 行只是空模板（`实际情况：______________`），**标「待复核 / 不可用作 Oracle」**，不得当作已确认结论。

## 三条实测事实（可复现）

对 5 份根级复核清单的**实测**结果（命令见下），是本目录整理口径的依据：

1. **`☑` / `☒` 勾选框命中数全为 0** —— `☐ 正确 / ☐ 错误` 这类勾选框**从未被勾选过**。
   ⇒ **不得**把「勾选状态」当判定来源，**不得**宣称「标注已勾选通过」。
   命令：`grep -o '☑\|☒' <file> | wc -l` → 5 份文件合计 **0**。
2. **`☐` 合计 1676 个**（逐文件 434 / 302 / 320 / 252 / 368）。
   命令：`grep -o '☐' <file> | wc -l`。
3. **`实际情况：` 字段合计 206 条，其中仅 74 条已填、132 条仍是空模板**。
   逐文件「已填 / 空」= `review.md` 0/43 · `round1` 40/0 · `round2` 0/40 · `round3` 1/39 · `round4` 33/10。
   命令（逐文件）：
   ```bash
   total=$(grep -c '实际情况：' FILE)
   blank=$(grep -c '实际情况：______________' FILE)
   echo "$FILE 已填=$((total-blank)) 空=$blank"
   ```

> `round1` 指根级 `review-annotated-2026-09-19.md`；`round2/3/4` 同名前缀。

## 脱敏政策

- **只保留**「被核对的期望」（算法推断的约定）与「实际情况」**判定标签**；
- **剔除**判定条目随后引用的**曲目正文样例行**（逐条 `- \`原文\``、`→ 原文=… / 译文=…` 等）——
  这些行可能含**私有曲库歌词正文**，一律**不写入**本目录任何文件；
- 因此 `human-verdicts.tsv` **不含任何歌词正文**，也不含 `☐/☑` 勾选框。

### ★ Oracle 粒度披露（脱敏的副作用，用 Oracle 前必读）

脱敏**保留了**「判定结论（`actual`）+ 被核对的期望（`expected`）」，**剔除了**判定条目中引用的
**曲目正文样例行**。因此本文件的 Oracle 粒度是「**判定标签级**」——即「这一条人工判为对/错、
其期望约定是什么」，而**不是**「可直接逐字比对的原文/译文文本级」。后果：

- 可用它核对「某行被人工判为正确 / 错误」这一结论，以及该行当时的算法推断约定；
- **不能**用它还原被判定的具体歌词文本，也不能把 `expected` 当作「该行应有的切分结果文本」直接比对；
- 需要文本级 ground truth 时，只能回到 5 份根级复核清单原件（**不提交**，且含私有正文）。

**脱敏抽查（用设计文档 §6.5 已公开引用过的行作样本）**：
以 §6.5 已公开的样例 `Shake up Tonight(シェイカップトゥナイトゥ) / …` 为探针——
该串在 5 份源复核清单中各出现 1 次，但在 `human-verdicts.tsv` 中出现 **0 次**（正文样例行已被剔除）。
（**不得**用私有曲库正文做此抽查。）

可复现命令与实测结果（2026-09-24，本机）：

```bash
# 5 份源清单各 1 次（正字，含 `イ`）
grep -c 'シェイカップトゥナイトゥ' review.md \
  review-annotated-2026-09-19.md review-annotated-round2-2026-09-19.md \
  review-annotated-round3-2026-09-19.md review-annotated-round4-2026-09-19.md
# → 1 / 1 / 1 / 1 / 1
# 脱敏副本中 0 次
grep -c 'シェイカップトゥナイトゥ' spec/human-verdicts.tsv        # → 0
# 设计文档正文 2 次
grep -c 'シェイカップトゥナイトゥ' spec/lyrics-original-translation-split-research-and-design-2026-09-19.md
# → 2
# 错字（漏 `イ`）在任何文件均 0 命中 —— 探针必须用正字
grep -rc 'シェカップ' spec/human-verdicts.tsv                    # → 0
```

## 可复现性：易失脚本已丢失

设计文档**附录 A 最后一行**警告：原型与数据脚本位于 `/tmp`，**易失**
（`/tmp/opencode/lyricres/survey{,2,3,4}.py`、`proto{,2,3}.py` —— §3/§4 全部实测数字的来源）。

**实测确认：这些脚本已丢失**（`ls /tmp/opencode/lyricres/` → 目录不存在；
`find /tmp -maxdepth 5 -name 'survey*.py' -o -name 'proto*.py'` → 0 命中）。
按计划走「已丢失」分支，**如实记录**：

> **来源脚本已丢失，§3/§4 数字不可复算。**

不得假装 §3/§4 的实测数字可复算；如需重跑，必须按 §6.2 重新实现原型。

## 不做的事

- 本目录**不参与产品构建**：没有任何 `CMakeLists.txt` 引用它（`rg -n "lyric_split_regression" Seriona_Backend/CMakeLists.txt` 应为空）。
- 不在此「顺便修」复核清单里的任何历史结论；`实际情况` 字段内容只做脱敏剔除，不改写。
