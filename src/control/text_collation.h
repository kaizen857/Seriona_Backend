#pragma once

#include <string_view>

namespace seriona::control {

// 库内文本排序比较（字典序）。返回值语义同 strcmp：<0 / 0 / >0。
//
// 语义（由决策文档锁定，改动前先读 docs/playback-sort-order-skip-defect-design-decision-2026-09-19.md §8 决策④）：
//  - ICU collator `zh-u-co-pinyin`：中文按拼音；日文汉字按中文读音（已知限制，见文档）。
//    必须显式写 `-u-co-pinyin`：`zh` 默认虽为拼音，但 `zh-Hant` 默认是笔画序。
//  - 强度 UCOL_SECONDARY：忽略大小写（abc == ABC），区分重音（é != e）。
//  - 折叠后相等时以 UTF-8 字节序裁决，保证全序确定（否则相等键的相对顺序不确定）。
[[nodiscard]] int compareCollatedText(std::string_view left, std::string_view right);

// UTF-8 字节序比较（collation 相等时的 tie-breaker，亦是 ICU 不可用时的兜底）。
[[nodiscard]] int compareUtf8Bytes(std::string_view left, std::string_view right);

}
