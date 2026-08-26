#include "folder_thumbnail_resolver.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

// (filename, relativeDirectory, filePath) 字节序全序比较。
// ASCII 大小写敏感（'B' < 'a'）；filePath 最终 tiebreak 保证同键候选排序完全确定
// （无 unordered 容器序依赖，同输入两次调用结果一致）。
[[nodiscard]] bool candidateLess(const FolderThumbnailCandidate& lhs, const FolderThumbnailCandidate& rhs) {
  const auto lhsName = lhs.filePath.filename().generic_u8string();
  const auto rhsName = rhs.filePath.filename().generic_u8string();
  if (lhsName != rhsName) {
    return lhsName < rhsName;
  }
  if (lhs.relativeDirectory != rhs.relativeDirectory) {
    return lhs.relativeDirectory < rhs.relativeDirectory;
  }
  return lhs.filePath.generic_u8string() < rhs.filePath.generic_u8string();
}

}  // namespace

std::optional<std::filesystem::path> resolveFolderThumbnail(
    const std::filesystem::path& physicalDirectory,
    const std::vector<FolderThumbnailCandidate>& descendants,
    const FolderThumbnailExportSeam& seam,
    bool isRoot) {
  // 根目录不处理：恒空、不调 seam、不做兜底。
  if (isRoot) {
    return std::nullopt;
  }

  // case 1：经导出 seam 对文件夹自身目录调 TagReader 导出（只查该目录，无递归）。
  // seam 为 null 跳过；异常吞掉回退 case 2 —— 单文件夹失败不阻断扫描。
  if (seam) {
    try {
      auto exported = seam(physicalDirectory);
      if (exported && !exported->empty()) {
        return exported;
      }
    } catch (...) {
      // 隔离：导出失败与导出为空同等对待。
    }
  }

  // case 2：纯树内兜底 —— 后代按 (filename, relativeDirectory) 字典序升序，
  // 取第一首 thumbnailPath 非空者。零 tag 重读、零解码（数据全部来自已构建的树）。
  if (descendants.empty()) {
    return std::nullopt;
  }
  std::vector<const FolderThumbnailCandidate*> ordered;
  ordered.reserve(descendants.size());
  for (const auto& candidate : descendants) {
    ordered.push_back(&candidate);
  }
  std::ranges::sort(ordered, [](const FolderThumbnailCandidate* lhs, const FolderThumbnailCandidate* rhs) {
    return candidateLess(*lhs, *rhs);
  });
  for (const auto* candidate : ordered) {
    if (candidate->thumbnailPath && !candidate->thumbnailPath->empty()) {
      return *candidate->thumbnailPath;
    }
  }
  return std::nullopt;
}

}
