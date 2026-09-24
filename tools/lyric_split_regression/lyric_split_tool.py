#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""歌词原文/译文切分：离线回归护栏 + 人工复核候选抽取器。

用途（对应设计文档 decisions D25 / P3）：
  1. --regression  在真实曲库上度量「算法边界 vs 人工 ` / ` 边界」的一致率，
                   作为可重复的回归护栏（门槛：一致率 >= 98%、不一致 <= 0.1%）。
  2. --review      抽取【算法不确定或可能出错】的行，输出为便于人工判定的清单，
                   供人工核对后把结论沉淀为测试用例（P3）。

本工具是【开发期工具】，不参与产品构建、不随发行版分发。
算法与 `docs/lyrics-original-translation-split-research-and-design-2026-09-19.md` §6.2 保持一致。

用法：
    扫描并回归：   python3 lyric_split_tool.py --root ~/Music --regression
    抽取复核清单： python3 lyric_split_tool.py --root ~/Music --review --out review.md
    查看运算结果： python3 lyric_split_tool.py --root ~/Music --dump '霞む夏の灯'
    同时做：       python3 lyric_split_tool.py --root ~/Music --regression --review --out review.md
"""
from __future__ import annotations

import argparse
import collections
import dataclasses
import os
import re
import sys
from typing import Iterable, Iterator

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

TIMESTAMP_RE = re.compile(r"^(?:\[\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?\])+")
INLINE_TAG_RE = re.compile(r"\[[a-zA-Z_]+:[^\]]*\]")
METADATA_RE = re.compile(r"^\s*\[[a-zA-Z_]+:", re.IGNORECASE)
# 制作人员行：不是歌词正文，必须在切分前丢弃
CREDIT_RE = re.compile(
    r"^\s*(作词|作曲|编曲|歌|唱|原唱|原曲|発売日|収録|ミックス|歌詞|翻译|訳|"
    r"词|曲|演唱|制作|混音|录音|母带|By|BY|by|Arr|Arrange|Lyrics|Vocals|Vocal|Music|Mix|Master)\s*[:：]"
)

OPENERS = set("（(「【〔〈《『［[{＜<")
CLOSERS = set("）)」】〕〉》』］]}＞>")

# 强分隔符：歌词作者显式书写意图。含半角 `/`——它在相当数量的歌词里被直接
# 当分隔符使用（`惚れた腫れたの馬鹿騒ぎ/愛意被潑冷水的超蠢騷動`），优先级低于
# 带空格的 ` / `，由歌曲级约定决定该文件用的是哪一种。
# 末尾的反斜杠同理，且**必须留在末位**：默认没有歌词用它（仅 1 个文件 31 行实测），
# 但它会出现在正文里（`(Kill you\)`）。置于末位 ⇒ 只在前面的约定都不成立时才有
# 机会得票，故正文里的反斜杠不会抢走 ` / ` 的票（见 infer_convention 的「首个匹配即得票」）。
STRONG_SEPARATORS = [" / ", "｜", "|", "／", "/", "\\"]
# 弱边界：空白类。多为排版间距（尤其全角空格），须由「歌曲级约定」背书。
# 刻意【不含双空格】：双空格边界在任何情况下也都是单空格边界，而单空格配合
# 「最左 + 右侧须纯目标语言」能落到正确的括注边界；把双空格列入反而会在排版文本
# （如塔语 `PARADIGM SHIFT  ━━ 「…」`）上抢先匹配到作者的对齐空格。
WEAK_BOUNDARIES = ["\t", "　", " "]

# 一行出现这么多次显式分隔符，说明是多段并列（`原文A / 原文B / 译文`）而非
# 「原文 / 译文」，结构已经歧义；按 D10 放弃切分，不赌一个。
MAX_SEPARATOR_OCCURRENCES = 2
# SCRIPT 与括号路径的切点处可能残留的分隔类字符，须从两侧剥离。
# 刻意**不含反斜杠**：作者会把转义写进译文（` / \t人生中 大家都愚蠢地可爱` 里的 `\t`
# 是制表符的转义写法），一概剥离会把它毁成 `t人生中…`（实测 131 行）。代价是
# `…★／ / \放马过来！…/` 这类用反斜杠包住译文的行会保留首尾装饰（2 行，仅外观）。
SEPARATOR_TRAIL_CHARS = ("/", "／", "|", "｜")
# 弱约定文件里逐行额外尝试的唯一显式分隔符（见 split_line 的说明）。
SPACED_SEPARATOR = " / "

# 候选类型（用于歌曲级约定推断，顺序即优先级）
CONVENTIONS = STRONG_SEPARATORS + WEAK_BOUNDARIES + ["SCRIPT"]

# 目标语言 -> 其语言类。默认中文。
TARGET_CLASS = {"zh": "HAN", "ja": "JA", "ko": "KO", "en": "LATN"}
# 能证明「这是另一种语言」的决定性语言类（汉字不是决定性特征）
NON_TARGET_DECISIVE = {"JA", "LATN", "KO", "CYRL"}
# 弱边界右侧出现这些脚本即判为「仍是原文」：三者与汉字同处 CJK 视觉区，混排时
# 无法靠肉眼区分原文/译文边界，比拉丁字母更危险。拉丁类不算阻塞（译文常夹英文）。
MIXED_SCRIPT_BLOCKERS = {"JA", "KO", "CYRL"}
# 括号兜底要求括号内容中目标语言类占比达到该值，避免把日文原文的注音括号
# （如 `（くも）`）误当作译文容器。


# ---------------------------------------------------------------------------
# 语言类判定（对应设计文档 §6.2 阶段 B）
# ---------------------------------------------------------------------------

def class_of_cp(ch: str) -> str | None:
    """返回码点的语言类；None 表示中性（空白/标点/数字），将黏附到相邻强段。"""
    o = ord(ch)
    if 0x3040 <= o <= 0x309F:                              # 平假名
        return "JA"
    if 0x30A0 <= o <= 0x30FF or 0x31F0 <= o <= 0x31FF:      # 片假名
        return "JA"
    if 0xFF66 <= o <= 0xFF9D:                              # 半角片假名
        return "JA"
    if 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF:      # 汉字（中日共享）
        return "HAN"
    if 0xF900 <= o <= 0xFAFF or 0x3005 <= o <= 0x3007:      # 兼容汉字 / 々〆〇
        return "HAN"
    if 0xAC00 <= o <= 0xD7AF or 0x1100 <= o <= 0x11FF:      # 谚文
        return "KO"
    if (0x41 <= o <= 0x5A) or (0x61 <= o <= 0x7A) or (0xC0 <= o <= 0x24F):
        return "LATN"
    if 0x0400 <= o <= 0x04FF:
        return "CYRL"
    return None


def profile(text: str) -> collections.Counter:
    """统计文本中各语言类的字符数。"""
    counter: collections.Counter = collections.Counter()
    for ch in text:
        cls = class_of_cp(ch)
        if cls:
            counter[cls] += 1
    return counter


# ---------------------------------------------------------------------------
# 清洗（对应 §6.2 阶段 A）
# 陷阱 P1：必须先剥离时间戳，再去行内 [tag:value]，否则 `[00:00.000][by:x]` 会漏过过滤。
# ---------------------------------------------------------------------------

def clean_line(raw: str) -> str | None:
    """剥离时间戳与行内元数据，丢弃制作人员行/空行。返回 None 表示该行不是歌词正文。"""
    stripped = raw.strip()
    if not stripped:
        return None
    body = TIMESTAMP_RE.sub("", stripped).strip()
    if not body:
        return None
    body = INLINE_TAG_RE.sub("", body).strip()
    if not body:
        return None
    if METADATA_RE.match(body) or CREDIT_RE.match(body):
        return None
    return body


# ---------------------------------------------------------------------------
# 候选切点枚举（对应 §6.2 阶段 C）
# ---------------------------------------------------------------------------

def candidate_positions(text: str, kind: str) -> list[int]:
    """返回 kind 指示的候选切点位置（切点在位置之前，即 [0,pos) 为原文）。"""
    positions: list[int] = []
    if kind in STRONG_SEPARATORS or kind in WEAK_BOUNDARIES:
        start = 0
        while True:
            hit = text.find(kind, start)
            if hit < 0:
                break
            positions.append(hit + len(kind))
            start = hit + 1
    elif kind == "SCRIPT":
        # 脚本过渡：非目标决定性语言类 -> 目标语言类
        prev: str | None = None
        for index, ch in enumerate(text):
            cls = class_of_cp(ch)
            if cls is None:
                continue
            if prev in NON_TARGET_DECISIVE and cls == "HAN":
                positions.append(index)
            prev = cls
    return positions


def bracket_regions(text: str) -> list[int]:
    """返回最外层配对括号区的起始位置，从右到左排列。

    译文常被 `「」`/`（）`/`【】` 整体包裹，此时正确切点是括号【之前】。
    仅取最外层区（内层嵌套由外层代表），并在遇到错配的结束括号时忽略之，
    避免畸形行造成错位。
    """
    stack: list[int] = []
    starts: list[int] = []
    for index, ch in enumerate(text):
        if ch in OPENERS:
            stack.append(index)
        elif ch in CLOSERS and stack:
            start = stack.pop()
            if not stack:
                starts.append(start)
    starts.reverse()
    return starts


def bracket_span(text: str, start: int) -> int | None:
    """返回以 start 处开括号起始的最外层括号区的结束位置（不含），无配对返回 None。"""
    stack: list[int] = []
    for index in range(start, len(text)):
        if text[index] in OPENERS:
            stack.append(index)
        elif text[index] in CLOSERS and stack:
            stack.pop()
            if not stack:
                return index + 1
    return None


def validate_cut_pure(text: str, cut: int, target: str = "zh",
                      guarded: bool = True) -> tuple[str, str] | None:
    """validate_cut 的严格版本：右侧必须【以目标语言开头】且不含与之混排的非目标脚本。

    弱边界（空白）本身不携带书写意图，因此仅靠「右侧含目标语言类」不足以确认
    切点——原语言一侧的空格也会满足该条件。追加两条后，切点必然落在语言转换处：
      * 右侧第一个【可分类】字符必须是目标语言类（跳过引号/括号等无类字符）。
        这挡掉了在拉丁原文内部切分：`We know...someday.\我们知道。` 的候选空格右侧
        以 `s` 开头，被拒；正确的反斜杠分隔处右侧以 `我` 开头，通过。
      * 右侧不得含假名/谚文/西里尔 —— 这三个脚本与汉字同为 CJK 视觉区，含之即说明
        右侧仍是原文（`答無きQuestion` 的 `き` 即是），是「最左」策略的主要护栏。
    **拉丁类不在此列**：译文夹英文/数字极常见（`受到邀请 Return to Bloom`、
    `朦胧的5khz`），一律拒绝会白丢这些行。
    """
    got = validate_cut(text, cut, target)
    if got is None:
        return None
    right = got[1]
    head = next((ch for ch in right if class_of_cp(ch) is not None), None)
    if head is None or class_of_cp(head) != TARGET_CLASS[target]:
        return None
    if any(profile(right)[kind] for kind in MIXED_SCRIPT_BLOCKERS):
        return None
    if guarded and cut_inside_bracket(text, cut):
        return None
    return got


def _accept(left: str, right: str, convention: str, route: str) -> SplitResult:
    """两侧相同也照切：作者写了 `A / A` 就说明该行确有此结构，用户实测判定为应切分。

    不做 self-identical 拒绝会把 903 行的覆盖从「放弃」变为「正确」（§6.6 实测）。

    `route` 记入 `reason`（`strong` / `strong-fallback` / `bracket` / `weak`），是复核清单
    区分「按显式分隔符切」与「按结构性推断切」的唯一依据（D36 新增逻辑需要被看见）。
    """
    return SplitResult(left, right, convention, "high" if route != "weak" else "medium", route)


def _trim_edges(text: str, cut: int, kind: str | None = None) -> tuple[str, str]:
    """在切点处剥离分隔符与空白，返回 (左, 右)。

    kind 给出被匹配的分隔符时只剥离它本身 —— 不能剥离「所有分隔符」，否则
    `＼なので～す／ / ＼的~说／` 里作为内容的 `／` 会被误删。
    kind 为 None（SCRIPT/括号路径）时改剥切点处残留的分隔类字符。

    分隔符与空白须**交替**剥离直到稳定，不能先剥所有分隔符再统一 strip：
    `原文 /  / 译文` 折叠后的切点在第二个分隔符之后，`原文 /  /` 以空白结尾，
    一次性剥离会停在 `原文 /`（残留一根分隔符）。交替剥离则剥净整段。
    """
    left, right = text[:cut], text[cut:]
    tokens = (kind,) if kind else SEPARATOR_TRAIL_CHARS
    while True:
        before = (left, right)
        for token in tokens:
            if not token:
                continue
            while right.startswith(token):
                right = right[len(token):]
            while left.endswith(token):
                left = left[: -len(token)]
        left, right = left.rstrip(), right.lstrip()
        if (left, right) == before:
            break
    return left, right


def validate_cut(text: str, cut: int, target: str = "zh") -> tuple[str, str] | None:
    """双向验证（对应 §6.2 阶段 C），用于弱边界与括号路径。

    右侧必须：含目标语言类，且【不含】假名/谚文（否则是日文/韩文，不是中文译文）。
    左侧必须：含至少一个非目标的决定性语言类（证明原文是另一种语言）。
    """
    target_cls = TARGET_CLASS.get(target)
    if target_cls is None:
        return None
    left, right = _trim_edges(text, cut)
    if not left or not right:
        return None
    right_profile = profile(right)
    if right_profile[target_cls] == 0:
        return None
    if right_profile["JA"] or right_profile["KO"]:
        return None
    left_profile = profile(left)
    if not any(left_profile[k] for k in NON_TARGET_DECISIVE):
        return None
    return left, right


def _separator_runs(text: str, positions: list[int], kind: str) -> list[tuple[int, int]]:
    """把切点序列合并成「分隔符段」，返回每段的 (起点, 终点)。

    源文件里 `原文 /  / 译文`（中间段为空）很常见 —— 译文缺失的字段被写成了空串。
    空段不承载内容，不构成一个分段，故两条分隔符实际只起一条分隔作用，须并成一段。
    相邻两切点之间若只剩本分隔符与空白，即属同一段。

    返回整段而非单个切点，是因为**重叠分隔符**（` / / ` 里第二条 ` / ` 与第一条共用
    一个空格）无法靠边界剥离清净：从 `…storm. / / ` 尾部剥掉一个 ` / ` 后剩 `…storm. /`，
    已不再是完整的分隔符记号。按段整体切除则一步到位。

    必须合并而非放宽 MAX_SEPARATOR_OCCURRENCES：`/ [A] / [B] / [C] /` 这类**每段都有
    内容**的多段并列仍须整行作原文（D32），只有空段才合并。
    """
    if not positions:
        return []
    runs: list[list[int]] = [[positions[0]]]
    for pos in positions[1:]:
        if text[runs[-1][-1]:pos].strip(kind).strip() == "":
            runs[-1].append(pos)
        else:
            runs.append([pos])
    spans: list[tuple[int, int]] = []
    for run in runs:
        start, end = run[0] - len(kind), run[-1]
        while end < len(text) and text[end].isspace():
            end += 1
        spans.append((start, end))
    return spans


def _cut_explicit(text: str, kind: str, guarded: bool = True) -> tuple[str, str] | None:
    """显式分隔符切分（取最右一个有效切点，符合 D5「译文取最后一段」）。

    **不要求脚本证据**：`言葉 / 这句话` 这类纯汉字原文与纯汉字译文在脚本属性上
    完全同类，但作者已用分隔符明确指出切分位置，脚本证据在此不应成为门槛 ——
    这正是「汉字→汉字盲区」在实践中大部分可解的原因。

    `guarded=True`（执行期）**先剔除落在括号内部的候选**（D20：括号必须归属其内容）：
      * `. / ）` 这类出现在注音 `（假名 / 罗马音）` 里的分隔符是**读音装饰**，不是
        原文/译文分界；不剔除的话，真实分界（括号外的裸 `/`）会被误判为「多段并列」。
      * 段的计数也必须在剔除之后进行 —— 计数的是**可用**分隔，不是字面出现次数。
    `guarded=False`（计票期）保留全部候选，只回答「作者是否用这种写法分隔」。
    """
    positions = candidate_positions(text, kind)
    if guarded:
        positions = [pos for pos in positions if not cut_inside_bracket(text, pos)]
    runs = _separator_runs(text, positions, kind)
    if len(runs) >= MAX_SEPARATOR_OCCURRENCES:
        return None
    for start, end in sorted(runs, reverse=True):
        left, right = text[:start].rstrip(), text[end:].lstrip()
        # 译文侧再剥一层分隔符噪声：源文件常把分隔符写得不对称（`原文 / /译文`、
        # `原文 /译文`），切点之后紧跟的裸 `/` 是记号残留而非译文内容。实测 127 行
        # 译文因此以 `/` 开头。原文侧**不**做此剥离——`＼なので～す／` 这类标题里的
        # `／` 是内容（见 _trim_edges 的说明）。
        right = right.lstrip("".join(SEPARATOR_TRAIL_CHARS)).lstrip()
        if left and right:
            return left, right
    return None


def _cut_script(text: str, target: str = "zh", guarded: bool = True) -> tuple[str, str] | None:
    """脚本过渡切点：非目标决定性语言类 → 目标语言类的最后一次转换。

    跳过落在括号内部的过渡点：`… jouee.(君ひとりでは手に余る重圧)` 的「假名→汉字」
    过渡发生在**日文注音括号内部**，切在那里会劈开括号（D20），且那也不是译文。
    """
    previous: str | None = None
    cut: int | None = None
    for index, ch in enumerate(text):
        cls = class_of_cp(ch)
        if cls is None:
            continue
        if (
            previous in NON_TARGET_DECISIVE
            and cls == "HAN"
            and not (guarded and cut_inside_bracket(text, index))
        ):
            cut = index
        previous = cls
    if cut is None:
        return None
    left, right = _trim_edges(text, cut)
    if not left or not right:
        return None
    if any(profile(right)[kind] for kind in NON_TARGET_DECISIVE):
        return None
    return left, right


def _is_target_bracket(text: str, start: int, target: str = "zh") -> bool:
    """括号区内容是否【就是译文】。

    **不用「目标语言字符占比」判据**：译文里常夹英文（`「I Miss You 一刻也停不下来」`
    占比仅 0.47），而日文注音括号的占比也可能不低（`(君ひとりでは手に余る重圧)` 0.42），
    两者用占比无法区分。真正的区别是**语言种类**：

      * 译文括号：含目标语言类，且**不含**假名/谚文/西里尔 —— 夹英文不影响
      * 注音括号：含假名（`(君ひとりでは手に余る重圧)`、`（くも / ）`）⇒ 是日文，不是译文

    西里尔同理（俄语原文的注音括号），一并排除。
    """
    end = bracket_span(text, start)
    if end is None:
        return False
    counts = profile(text[start:end])
    if counts[TARGET_CLASS[target]] == 0:
        return False
    return not any(counts[kind] for kind in ("JA", "KO", "CYRL"))


def _cut_bracket(text: str, start: int, target: str = "zh") -> tuple[str, str] | None:
    """把括号区起点当切点：仅当括号内容【就是译文】时才接受（否则是原文的注音括号）。"""
    if not _is_target_bracket(text, start, target):
        return None
    left, right = _trim_edges(text, start)
    if not left or not right:
        return None
    return left, right


def _find_any_bracket(text: str, target: str = "zh", trailing_only: bool = False) -> tuple[str, str] | None:
    """按最右优先尝试各最外层括号区；trailing_only 时只接受延伸到行尾的括号区。"""
    for start in bracket_regions(text):
        end = bracket_span(text, start)
        if end is None:
            continue
        if trailing_only and text[end:].strip():
            continue
        got = _cut_bracket(text, start, target)
        if got:
            return got
    return None


# ---------------------------------------------------------------------------
# 歌曲级约定推断 + 逐行切分（对应 §6.2 阶段 D/E）
# 这是本方案的核心：歧义在歌曲级消解，而非逐行赌。
# ---------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class SplitResult:
    original: str
    translation: str
    convention: str | None      # 生效的约定；None = 未切分
    confidence: str             # 'high' | 'medium' | 'none'
    reason: str                 # 未切分/降级的原因，便于复核清单分类


def _accepts(line: str, kind: str, target: str = "zh") -> bool:
    """该行是否支持把 kind 视为本文件的切分约定。

    必须与 split_line 的受理条件一致，否则会出现「票数来自一种判据、执行却用
    另一种」的漂移：约定被推断出来，实际却切不出来。

    **唯一的例外是 D20 括号守卫**（故一律 `guarded=False`）：计票问的是「作者用了哪种
    分隔符写法」，而非「本行切在哪里」。作者把分隔符写进注音括号（`（くも / ）`）时，
    该行依然是本文件用 `/` 的证据；若放守卫进计票，这类文件的约定会整体推断失败、
    整首译文丢失（实测 39 行）。守卫只在执行期生效（§6.2.6）。
    """
    if kind in STRONG_SEPARATORS:
        return _cut_explicit(line, kind, guarded=False) is not None
    if kind in WEAK_BOUNDARIES:
        return any(validate_cut_pure(line, pos, target, guarded=False)
                   for pos in candidate_positions(line, kind))
    return _cut_script(line, target, guarded=False) is not None


def infer_convention(lines: Iterable[str], target: str = "zh") -> tuple[str | None, dict[str, int]]:
    """从整首歌词的聚合证据推断本文件采用的切分约定。

    返回 (约定名 或 None, 各候选的得票数)。门槛：绝对数 >= 2 且占行数 >= 40%。
    """
    lines = list(lines)
    votes: collections.Counter = collections.Counter()
    for line in lines:
        for kind in CONVENTIONS:
            if _accepts(line, kind, target):
                votes[kind] += 1
                break
    total = len(lines)
    if total == 0:
        return None, {}
    for kind in CONVENTIONS:
        if votes[kind] >= 2 and votes[kind] / total >= 0.4:
            return kind, dict(votes)
    return None, dict(votes)


def split_line(text: str, convention: str | None, target: str = "zh") -> SplitResult:
    """按已推断的约定切分单行。

    受理条件随约定而不同（这是本算法的关键，不可统一成一套判据）：
      * **切点永不落在配对括号内部**（D20）。注音括号 `（假名 / 罗马音）` 里的分隔符、
        `「/译文」` 里的 `/` 都会被剔除，不参与切分。
      * 强分隔符（` / `、`/` 等）：作者显式书写意图，直接采信，**不要求脚本证据**，
        取【最右】一个切点（D5）。若本行的候选**全部**落在括号内部（说明分隔符是
        读音装饰），允许改用「括号外的其它强分隔符」或「内容即译文的括号区起点」；
        此外一律按 D10 整行作原文，**不回退**到括号/空白猜测——那会用结构性猜测
        覆盖作者的显式意图。
      * 包裹行尾段的括号区起点优先于弱边界：译文可能内含拉丁词
        （`「I Miss You 一刻也停不下来」`），此时「右侧纯目标语言」不成立，只有括号能定位。
      * 弱边界（空白）：取【最左】一个「右侧为纯目标语言」的切点，因为译文是行尾
        那段连续的目标语言区，从右往左取会切进译文内部。
      * SCRIPT：非目标语言类 → 目标语言类的最后一次转换。
    """
    if not text:
        return SplitResult(text, "", None, "none", "empty")
    # QQ 音乐用 `//` 整行表示「此行无译文」
    if text.strip() == "//":
        return SplitResult(text, "", None, "high", "qq-sentinel")
    if convention is None:
        # 整首推断不出约定（如显式 ` / ` 行占比不足 40% 的弱判定文件），**不代表本行不可切**：
        # 行内写着 ` / ` 就是作者的显式表态，按 D30 直接采信，不受文件级门槛牵累。
        # 实测 `ELYSIAN/05._一梦红尘.lrc` 等 3 个文件因此白丢 27 行译文。
        # 只试带空格的 ` / `（同 D31）：裸 `/` 会命中 `SOL_FAGE/1x10` 类原文记法。
        got = _cut_explicit(text, SPACED_SEPARATOR)
        if got is not None:
            return _accept(got[0], got[1], SPACED_SEPARATOR, "strong")
        return SplitResult(text, "", None, "none", "no-convention")

    if convention in STRONG_SEPARATORS:
        got = _cut_explicit(text, convention)
        if got is not None:
            return _accept(got[0], got[1], convention, "strong")
        # 本行的分隔符候选【全部落在括号内部】⇒ 它是读音/装饰符，不是原文/译文分界：
        #   `（假名 / 罗马音）` 里的 ` / `、`「/译文」` 里的 `/`
        # 此时允许改用两条结构性证据（两者都不会切开括号）：
        #   1) 括号外的其它强分隔符 —— `…（くも / ）/粗澀…` 的真正分界是括号外的裸 `/`
        #   2) 内容即译文的括号区起点 —— `おぼつかぬ足取り 「/略带动摇的步伐」`
        # **不可放宽为「只要切不出就回退」**：因「多段并列」（候选在括号外）被否决时
        # 必须整行作原文（D28/D10），那类行的 `/` 是真正的分隔符，只是分段过多。
        candidates = candidate_positions(text, convention)
        if candidates and all(cut_inside_bracket(text, pos) for pos in candidates):
            for other in STRONG_SEPARATORS:
                if other == convention:
                    continue
                got = _cut_explicit(text, other)
                if got is not None:
                    return _accept(got[0], got[1], other, "strong-fallback")
            got = _find_any_bracket(text, target, trailing_only=True)
            if got is not None:
                return _accept(got[0], got[1], convention, "strong-bracket-fallback")
            # 3) 本行其实**没有可用的强分隔符**（候选全在注音括号内）⇒ 文件级约定对本行
            #    零信息量，允许续走弱边界 / SCRIPT 路径。用户标注的 2 行即靠此修好：
            #      `蔷薇を想わせる绯色の『口红』(ローズレッドルージュ / )  令人联想起…`
            #      `あの『悲鸣は』(うたごえが / )『葡萄酒』   那悲鸣（歌声）有如葡萄美酒`
            #    两条的 `/` 都在注音括号里，真正的分界是括号后的空白。
            #    **这与 D10「强分隔符验证不过则整行作原文」不冲突**：D10 约束的是候选
            #    【在括号外】而被否决的行（那类行的 `/` 是真的分隔符，只是分段过多，
            #    实测 53 行，仍整行作原文）；此处候选【全部】在括号内，证明它们不是
            #    分隔符而是读音装饰，故不构成「作者已表态」。
            # 弱边界优先于 SCRIPT：SCRIPT 只看「非目标语言→目标语言」的过渡，
            # 遇到汉字夹假名（原文本身含汉字）容易在日语内部乱切。
            for pos in sorted({p for wb in WEAK_BOUNDARIES for p in candidate_positions(text, wb)}):
                got = validate_cut_pure(text, pos, target)
                if got is not None:
                    return _accept(got[0], got[1], convention, "strong-weak-fallback")
            got = _cut_script(text, target)
            if got is not None:
                return _accept(got[0], got[1], convention, "strong-weak-fallback")
        return SplitResult(text, "", None, "none", "validate-failed")

    if convention in WEAK_BOUNDARIES or convention == "SCRIPT":
        # 行内出现 ` / ` 时，它的证据强于本文件的弱约定：`11. 花のように.lrc` 的约定
        # 是空白，但个别行写着 `唄：花たん（ユリカ） / 演唱：花たん（百合香）`，若让
        # 弱约定路径先走括号，会被切成 `…演唱：花たん` / `（百合香）`。
        # **只尝试带空格的 ` / `**：裸 `/` 会命中原文记法里的斜杠
        # （`SOL_FAGE/1x10`），而裸 `/` 若真是该文件的约定，会经约定推断走强分隔符路径。
        got = _cut_explicit(text, SPACED_SEPARATOR)
        if got is not None:
            return _accept(got[0], got[1], SPACED_SEPARATOR, "strong")
        # 行内已含显式 ` / ` 时，**不再**走「延伸到行尾的括号区」这一结构性猜测：
        # `11. 花のように.lrc` 的 `「A」 / 「B」 / 「C」 / 「B」` 若走括号路径，会把
        # B 之前的片段全部塞进原文（原文被污染成 `「A」 / 「B」 / 「C」 /`）。
        # 显式结构优先于结构性猜测（D28/D32），此类行交给下面的弱边界路径处理。
        if SPACED_SEPARATOR not in text:
            got = _find_any_bracket(text, target, trailing_only=True)
            if got is not None:
                return _accept(got[0], got[1], convention, "bracket")
        if convention in WEAK_BOUNDARIES:
            for pos in candidate_positions(text, convention):
                got = validate_cut_pure(text, pos, target)
                if got is not None:
                    return _accept(got[0], got[1], convention, "weak")
        else:
            got = _cut_script(text, target)
            if got is not None:
                return _accept(got[0], got[1], convention, "weak")

    got = _find_any_bracket(text, target)
    if got is not None:
        return _accept(got[0], got[1], convention, "bracket")

    return SplitResult(text, "", None, "none", "validate-failed")


@dataclasses.dataclass
class DocumentSplit:
    path: str
    convention: str | None
    votes: dict[str, int]
    results: list[tuple[str, SplitResult]]      # (清洗后的行, 结果)


def split_document(raw_lines: Iterable[str], path: str = "", target: str = "zh") -> DocumentSplit:
    """整首：清洗 -> 推断约定 -> 逐行切分。"""
    cleaned = [c for c in (clean_line(line) for line in raw_lines) if c]
    convention, votes = infer_convention(cleaned, target)
    results = [(line, split_line(line, convention, target)) for line in cleaned]
    return DocumentSplit(path=path, convention=convention, votes=votes, results=results)


# ---------------------------------------------------------------------------
# 曲库遍历
# ---------------------------------------------------------------------------

# 解码尝试顺序（首个成功即采信）。两端必须一致——`--dump-tsv` 的
# `decode_by_codec=` 就是按这个顺序列出非零项，顺序变了首行注释就会变。
TEXT_ENCODINGS = ("utf-8-sig", "utf-8", "gb18030", "big5", "shift_jis", "utf-16")


def read_text_file_with_codec(path: str) -> tuple[str, str] | None:
    """同 read_text_file，但一并返回命中的编码名。

    `--dump-tsv` 的首行注释要打印 `decode_by_codec=`，供 Python/C++ 两端互证
    「看到的是同一批输入且解码一致」。两端必须**按同一顺序、首个成功即采信**；
    该顺序就是下面的 TEXT_ENCODINGS（注意 `utf-8-sig` 在前，故无 BOM 的 UTF-8
    也一律报 `utf-8-sig`——这是刻意的确定性口径，不是缺陷）。
    """
    try:
        blob = open(path, "rb").read()
    except OSError:
        return None
    for encoding in TEXT_ENCODINGS:
        try:
            return blob.decode(encoding), encoding
        except UnicodeDecodeError:
            continue
    return None


def read_text_file(path: str) -> str | None:
    """按常见编码尝试解码；解码失败返回 None（不猜测内容）。"""
    got = read_text_file_with_codec(path)
    return got[0] if got is not None else None


def iter_lrc_files(root: str) -> Iterator[str]:
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".lrc"):
                yield os.path.join(dirpath, name)


# ---------------------------------------------------------------------------
# 模式 1：回归护栏（D25）
# ---------------------------------------------------------------------------

def cut_inside_bracket(text: str, cut: int) -> bool:
    """切点是否落在最外层配对括号区的【内部】——即切开了配对括号（违反 D20）。

    这是独立于「人工 ` / ` 标注」的客观判据：产出半边括号不配对的原文
    （`（cEzLYA hymmnos` / `.）`）必然错误，与标注是否一致无关。
    """
    for start in bracket_regions(text):
        end = bracket_span(text, start)
        if end is not None and start < cut < end:
            return True
    return False


def run_regression(root: str, target: str, threshold_agree: float, threshold_disagree: float) -> int:
    files = 0
    undecodable = 0
    total_lines = 0
    agreement = disagreement = refused = 0
    bracket_interior = 0
    proxy_invalid = 0
    uncovered = 0
    disagreements: list[tuple[str, str, str, str]] = []

    for path in sorted(iter_lrc_files(root)):
        text = read_text_file(path)
        if text is None:
            undecodable += 1
            continue
        files += 1
        doc = split_document(text.splitlines(), path, target)
        for line, result in doc.results:
            total_lines += 1
            # 全量行都统计，不受下面「仅 ` / ` 标注行」对照范围的限制
            if result.translation and line.endswith(result.translation) and \
                    cut_inside_bracket(line, len(line) - len(result.translation)):
                bracket_interior += 1
            # 仅对「人工已用 ' / ' 标注过」的行做对照
            marker = line.find(" / ")
            if marker < 0:
                uncovered += 1
                continue
            # 代理边界本身畸形 ⇒ 该行【不可作为对照样本】，两种可客观检出的形态：
            #  1) 首个 ` / ` 落在配对括号内部：机械抽取会把注音区（`（くも / ）`）切成
            #     两半（译文以 `）` 开头），并不代表作者意图。
            #  2) 译文以分隔符字符开头（`A /  / B` 取首个 ` / ` 得译文=`/ B`）：源文件把
            #     缺失字段写成了空串，故该行有两个 ` / `，取首个必然切错。
            # 两种都是代理【自身】的畸形，与算法怎么切无关，故不构成循环论证。
            # 不排除则会把算法的**正确**切分误判为不一致（实测 9 + 17 行）。
            if cut_inside_bracket(line, marker + 3) or line[marker + 3:].lstrip()[:1] in SEPARATOR_TRAIL_CHARS:
                proxy_invalid += 1
                continue
            human_original = line[:marker].strip()
            human_translation = line[marker + 3:].strip()
            if not human_translation:
                continue
            if not result.translation:
                refused += 1
            elif result.original.strip() == human_original and result.translation.strip() == human_translation:
                agreement += 1
            else:
                disagreement += 1
                if len(disagreements) < 20:
                    disagreements.append((path, line, result.original, result.translation))

    checked = agreement + disagreement + refused
    print(f"文件            : {files}（无法解码 {undecodable}）")
    print(f"歌词行总数      : {total_lines}")
    print(f"参与对照的行    : {checked}（人工 ' / ' 标注且译文非空）")
    print(f"  · 代理边界无效: {proxy_invalid}  (切在括号内 / 译文以分隔符开头，不可作对照样本)")
    # 披露盲区规模：对照集只含「带 ' / ' 标注」的行，其余行没有客观 oracle，
    # 只能靠上一条括号判据与人工复核覆盖（`リプレイマシン` 的 31 行错切即藏在此盲区内，
    # 由用户人工复核发现）。数字必须打印出来，否则「精确率 100%」会被误读为全库干净。
    print(f"  · 对照盲区    : {uncovered}  (无 ' / ' 标注 ⇒ 无客观 oracle，仅由上一条括号判据覆盖)")
    if checked == 0:
        print("无可对照样本，无法评估。")
        return 2
    print(f"  与人工边界一致: {agreement}  ({100.0 * agreement / checked:.2f}%)")
    print(f"  不一致        : {disagreement}  ({100.0 * disagreement / checked:.2f}%)")
    print(f"  保守放弃      : {refused}  ({100.0 * refused / checked:.2f}%)")
    decided = agreement + disagreement
    rate = 100.0 * agreement / decided if decided else 0.0
    print(f"  判定精确率    : {rate:.2f}%  (一致/(一致+不一致))")
    print(f"  切点落在括号内: {bracket_interior}  (违反 D20；不计入门槛，见 §6.2.6)")

    if disagreements:
        print("\n不一致样例（可能是算法更正确，需人工确认）：")
        for path, line, got_original, got_translation in disagreements[:10]:
            print(f"  [{os.path.basename(path)[:40]}]")
            print(f"     原始: {line[:110]}")
            print(f"     算法: 原文={got_original[:50]!r} 译文={got_translation[:50]!r}")

    passed = rate >= threshold_agree and (100.0 * disagreement / checked) <= threshold_disagree
    print(f"\n门槛：精确率 >= {threshold_agree}% 且 不一致率 <= {threshold_disagree}%")
    print("回归结果：", "通过 ✔" if passed else "未通过 ✘")
    return 0 if passed else 1


# ---------------------------------------------------------------------------
# 模式 2：人工复核候选抽取（P3）
# ---------------------------------------------------------------------------

# 复核类别
REVIEW_CATEGORIES = [
    ("structural-cut", "切分出自 D36/D37【降级或结构推断】（改用括号外的其它强分隔符 / 括号区兜底）—— 算法变更处，逐条审核"),
    ("backslash-convention", "整首以反斜杠 `\\` 为约定（新增约定，此前要么整行不切、要么切在译文内部）—— 请确认译文完整"),
    ("stripped-separator", "原文后多一根裸 `/`（写作 `原文 / /译文`）—— 残留 `/` 已剥离，请确认译文正确"),
    ("blank-segment", "分隔符含空段（写作 `原文 /  / 译文`，缺失字段为空串）—— 已按「空段不构成分段」折叠，请确认"),
    ("weak-convention", "整首采用【弱约定】(空白类) 且无 ' / ' 可交叉验证 —— 最需要人工确认"),
    ("blindspot", "『汉字→汉字』且两侧不同 —— 原理性盲区，算法不切分"),
    ("refused-multilingual", "含多种语言但算法拒绝切分 —— 可能是漏切"),
    ("low-confidence", "算法切分但置信度为 medium —— 边界处无显式分隔符"),
    ("no-convention", "整首未推断出约定，但行内含多语言 —— 可能是漏切"),
]


def _separator_shape(line: str) -> str | None:
    """识别源文件里两种畸形分隔写法，用于把 D37 的修复摆进复核清单。

    两者都表现为「` / ` 之后还有一根 `/`」，区别在中间是否夹空格：
      * `原文 / /译文`  → `stripped`（D37④ 剥离那根残留 `/`）
      * `原文 /  / 译文` → `blank`（D37② 空段不构成分段，折叠为一条）
    """
    marker = line.find(" / ")
    if marker < 0:
        return None
    rest = line[marker + 3:].lstrip()
    if rest[:1] not in SEPARATOR_TRAIL_CHARS:
        return None
    return "blank" if rest[1:2] in (" ", "\t", "\u3000") else "stripped"


def classify_for_review(doc: DocumentSplit, line: str, result: SplitResult) -> str | None:
    line_profile = profile(line)
    languages = {k for k, v in line_profile.items() if v}
    has_marker = " / " in line

    # 必须排在最前：这类行的切点不是由本文件的约定决定的，若让下面的 weak-convention
    # 先命中，它们会被并进「整首弱约定」的文件级结论里而失去逐条复核的机会
    # （D36 的 D/E 两类正落在弱约定文件中）。
    if result.translation and result.reason in ("strong-fallback", "strong-bracket-fallback", "strong-weak-fallback"):
        return "structural-cut"
    # 同样须先于文件级类别：反斜杠是新纳入的约定，整首的输出因此整体变了，
    # 是本轮最需要逐条确认的改动（改动前这些行要么无译文、要么译文被截断成半句）。
    if result.translation and doc.convention == "\\":
        return "backslash-convention"
    # 同样须先于文件级类别：D37④ 把译文前那根错位的 `/` 剥掉了，输出本身已看不出
    # 曾经剥过，只能回到原始行判断，否则这项修复永远无法被人工确认。
    if result.translation:
        shape = _separator_shape(line)
        if shape == "stripped":
            return "stripped-separator"
        if shape == "blank":
            return "blank-segment"
    # 整首弱约定且没有 ' / ' 交叉验证
    if doc.convention in WEAK_BOUNDARIES and not has_marker:
        return "weak-convention"
    # 汉字→汉字盲区（两侧不同）
    if result.reason in ("no-convention", "validate-failed") and has_marker:
        marker = line.find(" / ")
        left, right = line[:marker].strip(), line[marker + 3:].strip()
        if left != right and profile(left).get("HAN") and profile(right).get("HAN"):
            lp, rp = profile(left), profile(right)
            if not (lp["JA"] or lp["KO"]) and not (rp["JA"] or rp["KO"]):
                return "blindspot"
    # 拒绝切分但含多语言
    if not result.translation and len(languages) >= 2:
        return "refused-multilingual"
    # 低置信切分
    if result.translation and result.confidence == "medium":
        return "low-confidence"
    # 未推断出约定但整行多语言
    if doc.convention is None and len(languages) >= 2:
        return "no-convention"
    return None


# 按【文件】判定才可复核的类别：整首共用同一约定，逐行看是浪费人力
FILE_SCOPED_CATEGORIES = {"weak-convention", "no-convention"}
# 按【行】判定的类别：单行独立问题
LINE_SCOPED_CATEGORIES = {"blindspot", "refused-multilingual", "low-confidence"}


@dataclasses.dataclass
class ReviewFile:
    path: str
    convention: str | None
    votes: dict[str, int]
    sampled: list[tuple[str, SplitResult]]
    hit_count: int


def run_review(root: str, target: str, out_path: str, limit_per_category: int,
               samples_per_file: int) -> int:
    """抽取人工复核清单。

    关键设计：`weak-convention` 与 `no-convention` 是**整首性质**（用户洞察：一首歌要么整首
    单空格分隔、要么整首不是），因此按【文件】汇总并附少量样例行 —— 把上万行压缩成百余个
    待判文件。其余类别是单行问题，按【行】列出。
    """
    file_buckets: dict[str, dict[str, ReviewFile]] = collections.defaultdict(dict)
    line_buckets: dict[str, list[tuple[str, str, SplitResult]]] = collections.defaultdict(list)
    files = 0

    for path in sorted(iter_lrc_files(root)):
        text = read_text_file(path)
        if text is None:
            continue
        files += 1
        doc = split_document(text.splitlines(), path, target)
        for line, result in doc.results:
            category = classify_for_review(doc, line, result)
            if category is None:
                continue
            if category in FILE_SCOPED_CATEGORIES:
                entry = file_buckets[category].get(path)
                if entry is None:
                    entry = ReviewFile(path, doc.convention, doc.votes, [], 0)
                    file_buckets[category][path] = entry
                entry.hit_count += 1
                if len(entry.sampled) < samples_per_file:
                    entry.sampled.append((line, result))
            else:
                line_buckets[category].append((path, line, result))

    out: list[str] = [
        "# 歌词切分 · 人工复核清单",
        "",
        f"来源曲库：`{root}`（{files} 个 LRC）",
        f"目标语言：`{target}`",
        "",
        "> 用途：人工判定下列候选是否正确，把结论沉淀为测试用例（设计文档 P3）。",
        "> **文件级**类别（整首共用同一约定）请只看样例行、对整首给一个结论；",
        "> **行级**类别请逐条判定。",
        "",
        "| 类别 | 单位 | 命中数 |",
        "|---|---|---|",
    ]
    counts: list[tuple[str, str, int]] = []
    for key, description in REVIEW_CATEGORIES:
        if key in FILE_SCOPED_CATEGORIES:
            counts.append((description, "文件", len(file_buckets.get(key, {}))))
        else:
            counts.append((description, "行", len(line_buckets.get(key, []))))
    for description, unit, count in counts:
        out.append(f"| {description.split(' —— ')[0]} | {unit} | {count} |")
    out.append("")

    for key, description in REVIEW_CATEGORIES:
        if key in FILE_SCOPED_CATEGORIES:
            entries = list(file_buckets.get(key, {}).values())
            if not entries:
                continue
            # 命中行数越多 → 越可能是整首一致的约定（越可能正确）；升序排列让最可疑的在前
            entries.sort(key=lambda e: (e.hit_count, e.path))
            out.append(f"## {description}")
            out.append("")
            out.append(f"命中 **{len(entries)}** 个文件"
                       f"（下列展示前 {min(len(entries), limit_per_category)} 个，各附 {samples_per_file} 条样例）")
            out.append("")
            for entry in entries[:limit_per_category]:
                rel = os.path.relpath(entry.path, root)
                out.append(f"### `{rel}`")
                out.append(f"- 推断约定：`{entry.convention}`；命中 {entry.hit_count} 行；"
                           f"得票 {entry.votes}")
                for line, result in entry.sampled:
                    if result.translation:
                        out.append(f"  - `{line}`")
                        out.append(f"    → 原文=`{result.original}` / 译文=`{result.translation}`")
                    else:
                        out.append(f"  - `{line}`  → 整行原文（原因 `{result.reason}`）")
                out.append(f"- 人工判：☐ 整首约定正确 ☐ 错误 → 实际情况：______________")
                out.append("")
        else:
            items = line_buckets.get(key, [])
            if not items:
                continue
            out.append(f"## {description}")
            out.append("")
            out.append(f"命中 **{len(items)}** 条（下列展示前 {min(len(items), limit_per_category)} 条）")
            out.append("")
            for path, line, result in items[:limit_per_category]:
                rel = os.path.relpath(path, root)
                out.append(f"- `{rel}`")
                out.append(f"  - 原始行：`{line}`")
                if result.translation:
                    out.append(f"  - 算法判：原文=`{result.original}` 译文=`{result.translation}`"
                               f"（置信 {result.confidence}）")
                else:
                    out.append(f"  - 算法判：**整行原文，无译文**（原因 `{result.reason}`）")
                out.append("  - 人工判：☐ 正确 ☐ 错误 → 正确切分：______________")
            out.append("")

    report = "\n".join(out)
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(report)
    print(f"复核清单已写入：{out_path}")
    for description, unit, count in counts:
        print(f"  {description.split(' —— ')[0]:22s} {count:6d} {unit}")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def run_dump(root: str, target: str, pattern: str, out_path: str | None, max_rows: int) -> int:
    """输出算法在【全部行】上的切分结果。

    与 --review 的区别：--review 只列「算法不确定、需要人工判定」的候选（问题清单），
    本模式列出每个文件每一行的实际切分（结果清单），用于直接检视运算结果。
    `pattern` 非空时按路径子串过滤，避免全库输出过大。
    """
    header: list[str] = []
    body: list[str] = []
    files = 0
    total_rows = 0
    total_split = 0

    for path in sorted(iter_lrc_files(root)):
        if pattern and pattern not in path:
            continue
        text = read_text_file(path)
        if text is None:
            continue
        files += 1
        doc = split_document(text.splitlines(), path, target)
        rows = doc.results
        split_n = sum(1 for _line, result in rows if result.translation)
        total_rows += len(rows)
        total_split += split_n

        body.append(f"## `{os.path.relpath(path, root)}`")
        body.append("")
        body.append(f"- 推断约定：{doc.convention!r}；得票 {doc.votes}")
        body.append(f"- 切分 {split_n} / 整行作原文 {len(rows) - split_n}（共 {len(rows)} 行）")
        body.append("")
        shown = rows if max_rows <= 0 else rows[:max_rows]
        for line, result in shown:
            body.append(f"  - `{line}`")
            if result.translation:
                body.append(f"    → 原文=`{result.original}` / 译文=`{result.translation}`"
                            f"    ［{result.convention}/{result.reason}，{result.confidence}］")
            else:
                body.append(f"    → 整行作原文（{result.reason}）")
        if max_rows > 0 and len(rows) > max_rows:
            body.append(f"  - ……（其余 {len(rows) - max_rows} 行省略）")
        body.append("")

    header.append("# 歌词切分 · 算法运算结果")
    header.append("")
    header.append(f"来源曲库：`{root}`（匹配 {files} 个 LRC）")
    header.append(f"目标语言：`{target}`")
    summary = (f"合计 {total_rows} 行：切分 {total_split}，整行作原文 {total_rows - total_split}")
    if pattern:
        summary += f"；路径过滤 `{pattern}`"
    header.append(summary)
    header.append("")

    rendered = "\n".join(header + body) + "\n"
    if out_path:
        with open(out_path, "w", encoding="utf-8") as handle:
            handle.write(rendered)
        print(f"运算结果已写入：{out_path}（{files} 个文件，{total_rows} 行）")
    else:
        sys.stdout.write(rendered)
    return 0


# ---------------------------------------------------------------------------
# 模式 4：TSV 指纹（--dump-tsv）——Python/C++ 两端逐字节比对的地基
# ---------------------------------------------------------------------------

def _escape_tsv(text: str) -> str:
    """列 3/6/7 的转义：`\\`→`\\\\`、TAB→`\\t`、LF→`\\n`。

    **顺序不可换**：必须先替反斜杠，否则字面 `\\t`（作者写的转义写法）会被二次转义成
    `\\\\t`。`--dump-tsv` 是跨语言线格式，C++ 端必须逐字复刻本函数。
    """
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def _convention_notation(convention: str | None) -> str:
    """列 3 的规范记号：强分隔符 `S:`+记号、弱边界 `W:`+记号、`SCRIPT`、未推断出写作 `-`。

    记号本身含反斜杠（`\\`）或制表符时，再由 `_escape_tsv` 转义（`S:\\\\`、`W:\\t`）。
    """
    if convention is None:
        return "-"
    if convention == "SCRIPT":
        return "SCRIPT"
    if convention in STRONG_SEPARATORS:
        return "S:" + convention
    if convention in WEAK_BOUNDARIES:
        return "W:" + convention
    return convention


def run_dump_tsv(root: str, target: str, out_path: str | None) -> int:
    """按行输出 7 列 TSV 指纹：相对路径 / 1-based 清洗后行号 / 文档级约定 /
    route(reason) / 置信度 / 原文 / 译文。

    列 6/7 未切分时分别是「整行」与空串。行按相对路径（POSIX `/`、Unicode 码点序）
    排序；首行注释给出 total_lines / files / unresolved_decode / decode_by_codec，
    供闸门断言两端看到同一批输入。
    """
    entries: list[tuple[str, str]] = []
    for path in iter_lrc_files(root):
        entries.append((os.path.relpath(path, root).replace(os.sep, "/"), path))
    entries.sort(key=lambda item: item[0])

    rows: list[str] = []
    files = 0
    total_lines = 0
    unresolved_decode = 0
    codec_counts: collections.Counter = collections.Counter()

    for rel, path in entries:
        got = read_text_file_with_codec(path)
        if got is None:
            unresolved_decode += 1
            continue
        text, codec = got
        codec_counts[codec] += 1
        files += 1
        doc = split_document(text.splitlines(), path, target)
        for index, (_line, result) in enumerate(doc.results, start=1):
            total_lines += 1
            rows.append("\t".join((
                rel,
                str(index),
                _escape_tsv(_convention_notation(doc.convention)),
                _escape_tsv(result.reason),
                _escape_tsv(result.confidence),
                _escape_tsv(result.original),
                _escape_tsv(result.translation),
            )))

    codec_part = ",".join(f"{name}={codec_counts[name]}"
                          for name in TEXT_ENCODINGS if codec_counts[name])
    header = (f"# total_lines={total_lines} files={files} "
              f"unresolved_decode={unresolved_decode} decode_by_codec={codec_part}")
    rendered = "\n".join([header] + rows) + "\n"

    if out_path:
        with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(rendered)
        print(f"TSV 指纹已写入：{out_path}（{files} 个文件，{total_lines} 行）")
    else:
        sys.stdout.write(rendered)
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="歌词原文/译文切分：离线回归护栏 + 人工复核候选抽取。")
    parser.add_argument("--root", required=True, help="曲库根目录（递归查找 .lrc）")
    parser.add_argument("--target", default="zh", choices=sorted(TARGET_CLASS), help="目标语言（默认 zh）")
    parser.add_argument("--regression", action="store_true", help="运行回归护栏（度量与人工边界的精确率）")
    parser.add_argument("--review", action="store_true", help="抽取人工复核候选清单")
    parser.add_argument("--dump", nargs="?", const="", default=None,
                        help="输出算法在全部行上的切分结果（可选：路径子串过滤，如 --dump '霞む夏の灯'）")
    parser.add_argument("--dump-tsv", action="store_true",
                        help="输出 7 列 TSV 行级指纹（供 Python/C++ 两端逐字节比对；默认写标准输出）")
    parser.add_argument("--max-rows", type=int, default=0, help="--dump 每个文件最多输出的行数（0=全部）")
    parser.add_argument("--out", default=None,
                        help="输出路径；--review 默认 lyric-review.md，--dump/--dump-tsv 默认写到标准输出")
    parser.add_argument("--limit", type=int, default=40, help="每类复核候选展示条数")
    parser.add_argument("--samples", type=int, default=3, help="文件级复核类别每个文件附的样例行数")
    parser.add_argument("--min-agree", type=float, default=98.0, help="回归门槛：判定精确率下限(%%)")
    parser.add_argument("--max-disagree", type=float, default=0.1, help="回归门槛：不一致率上限(%%)")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.root):
        print(f"错误：目录不存在 {args.root}", file=sys.stderr)
        return 2
    if not (args.regression or args.review or args.dump is not None or args.dump_tsv):
        parser.error("至少指定 --regression、--review、--dump 或 --dump-tsv")

    status = 0
    if args.regression:
        status = max(status, run_regression(args.root, args.target, args.min_agree, args.max_disagree))
    if args.review:
        if args.regression:
            print()
        status = max(status, run_review(args.root, args.target,
                                        args.out or "lyric-review.md",
                                        args.limit, args.samples))
    if args.dump is not None:
        status = max(status, run_dump(args.root, args.target, args.dump, args.out, args.max_rows))
    if args.dump_tsv:
        status = max(status, run_dump_tsv(args.root, args.target, args.out))
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
