#pragma once

#include "path_utf8.h"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace seriona::scanner {

// 文件夹缩略图缓存键 = "<root 字节数>:<root><relKey>"（长度前缀）。POSIX 文件名可含 '\n'
// 与 ':'，以 '\n' 作分隔符时 /m/a + b\nc 与 /m/a\nb + c 会碰撞（错误缩略图/误删行）；
// 长度前缀把 root 与 relKey 的边界编码进键，消除该歧义。键按字节处理（UTF-8 直存）。
[[nodiscard]] inline std::string folderThumbnailCacheKey(const std::filesystem::path& physicalRoot,
                                                         const std::string& relKey) {
  const auto rootText = pathToUtf8(physicalRoot.lexically_normal());
  return std::to_string(rootText.size()) + ":" + rootText + relKey;
}

// eraseCachedDirectory 的匹配谓词：从长度前缀还原 relKey 起始位置后做整段精确比较，
// 绝不做前缀/子串匹配——"album" 不得命中 "album2" 或 "xalbum"。
[[nodiscard]] inline bool folderThumbnailCacheEntryMatchesRelKey(std::string_view key, std::string_view relKey) {
  const auto separator = key.find(':');
  if (separator == std::string_view::npos || separator == 0U) {
    return false;
  }
  std::size_t rootBytes = 0;
  const auto* first = key.data();
  const auto* last = first + separator;
  const auto [parsed, parseError] = std::from_chars(first, last, rootBytes);
  if (parseError != std::errc{} || parsed != last) {
    return false;
  }
  const auto relStart = separator + 1 + rootBytes;
  return relStart <= key.size() && key.compare(relStart, std::string_view::npos, relKey) == 0;
}

// 擦除缓存中 relKey 部分精确等于目标的所有行，返回擦除行数。键的 root 部分不参与匹配：
// 调用点语义是"当前没有任何物理根映射该 relKey"（目录节点存在但无绝对路径歌曲），此时
// 各根的同 relKey 行都是陈旧行，一并清理。
inline std::size_t eraseFolderThumbnailCacheDirectory(std::unordered_map<std::string, std::string>& cache,
                                                      const std::string& relKey) {
  return std::erase_if(cache, [&](const auto& entry) {
    return folderThumbnailCacheEntryMatchesRelKey(entry.first, relKey);
  });
}

}
