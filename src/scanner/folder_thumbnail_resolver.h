#pragma once

// scanner-internal：不进 inc/seriona 稳定边界，不依赖 TagReader/Qt 类型。
// seam 是纯 path→path 的 std::function，生产侧由 tag_reader_metadata_adapter 装配。

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

// Directory 节点的一个后代 Track，数据全部来自已构建好的播放列表树。
// thumbnailPath 在歌曲扫描阶段已解析（SongMetadata::thumbnailPath），
// resolver 只读这些字段——零 tag 重读、零解码。
struct FolderThumbnailCandidate {
  // 歌曲文件路径（树内相对路径形态）；filename() 作为排序主键。
  std::filesystem::path filePath;
  // 歌曲所在目录相对被解析文件夹的路径：generic（正斜杠）形态、无结尾分隔符，
  // 直接子项为空串。仅用于同名文件时的并列键。
  std::string relativeDirectory;
  // 树内已解析的缩略图路径（歌曲扫描结果），空表示该歌曲无缩略图。
  std::optional<std::filesystem::path> thumbnailPath;
};

// 导出 seam：文件夹路径 -> 导出缩略图路径（case 1）。
// 生产侧接 TagReader::ExportFolderCover（ThumbnailOnly+Ignore）；
// seam 为 null 时跳过 case 1。
using FolderThumbnailExportSeam =
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)>;

// 解析单个 Directory 节点的 node-level 缩略图。
// case 1：seam(physicalDirectory) 返回非空路径则采用（只查该目录自身，由 TagReader API 语义保证）；
// case 2：否则按 (filename, relativeDirectory) 字典序升序（字节序、ASCII 大小写敏感）
//         取第一首 thumbnailPath 非空的后代歌曲路径（全深度递归由后代列表覆盖）。
// 根目录恒空：不调 seam、不做 case 2。
// 确定性：全序比较（含 filePath 最终 tiebreak），无 unordered 容器序依赖，同输入两次结果一致。
// 永不抛出：seam 异常被吞掉并回退 case 2 / 空，单文件夹失败不阻断扫描。
[[nodiscard]] std::optional<std::filesystem::path> resolveFolderThumbnail(
    const std::filesystem::path& physicalDirectory,
    const std::vector<FolderThumbnailCandidate>& descendants,
    const FolderThumbnailExportSeam& seam,
    bool isRoot = false);

}
