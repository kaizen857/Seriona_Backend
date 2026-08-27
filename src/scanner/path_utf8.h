#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace seriona::scanner {

// 路径的规范文本表示：恒为 UTF-8。
//
// 背景：MSVC 的 path::string()/generic_string() 把宽字符路径按 ANSI 代码页（CP_ACP）
// 转换，字符不可表示时抛 std::system_error（ERROR_NO_UNICODE_TRANSLATION，
// "No mapping for the Unicode character exists in the target multi-byte code page."），
// 即使不抛也会产生乱码。generic_u8string() 恒为 UTF-8、永不因编码失败抛异常。
// POSIX（value_type=char，原生编码即 UTF-8）上二者字节完全一致，因此本函数
// 不改变 Linux 行为，仅修正 Windows 的窄转换语义。
//
// 使用约束：所有跨边界的路径文本（DB 键/值、trackId、日志、哈希输入、FFmpeg 路径
// 参数）必须经 pathToUtf8 / pathFromUtf8 往返，不得直接使用 generic_string()/string()。
[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

// 从 UTF-8 文本构造路径（DB 读回、命令入参、文本改写结果）。
// C++20 起 path{u8string} 按 UTF-8 → 原生编码转换；MSVC 上避免窄字符串构造的 ACP 解释。
[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
  return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
}

}  // namespace seriona::scanner
