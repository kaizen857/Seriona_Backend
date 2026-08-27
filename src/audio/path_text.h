#ifndef SERIONA_AUDIO_PATH_TEXT_H
#define SERIONA_AUDIO_PATH_TEXT_H

// audio 模块内部的路径文本不变量：所有暴露给 FFmpeg / 日志 / 公共 API 的
// 路径文本一律为 UTF-8。Windows 上 std::filesystem::path::string()/generic_string()
// 按 ANSI 代码页转换，遇到不可表示字符会抛异常或产生乱码，禁止在路径文本
// 通道使用；POSIX 上本实现与 string() 字节级一致。

#include <filesystem>
#include <string>
#include <string_view>

namespace seriona::audio {

[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view text) {
  return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

}  // namespace seriona::audio

#endif
