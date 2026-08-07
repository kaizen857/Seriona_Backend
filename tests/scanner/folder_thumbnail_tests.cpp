#include "folder_thumbnail_resolver.h"

#include <doctest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

using Thumb = std::optional<std::filesystem::path>;

[[nodiscard]] FolderThumbnailCandidate track(std::filesystem::path relativePath,
                                              std::string relativeDirectory,
                                              Thumb thumbnail) {
  return FolderThumbnailCandidate{std::move(relativePath), std::move(relativeDirectory), std::move(thumbnail)};
}

// seam 返回空：强制走 case 2 树内兜底。
[[nodiscard]] FolderThumbnailExportSeam emptySeam() {
  return [](const std::filesystem::path&) -> Thumb { return std::nullopt; };
}

TEST_CASE("folder thumbnail resolver prefers the export seam result over the tree fallback (case 1)") {
  const FolderThumbnailExportSeam seam = [](const std::filesystem::path& directory) -> Thumb {
    REQUIRE(directory == std::filesystem::path{"/music/Album"});
    return std::filesystem::path{"/artwork/exported_cover.png"};
  };
  const std::vector<FolderThumbnailCandidate> descendants{
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };

  const auto result = resolveFolderThumbnail("/music/Album", descendants, seam);

  CHECK(result.has_value());
  CHECK(result == std::filesystem::path{"/artwork/exported_cover.png"});
}

TEST_CASE("folder thumbnail resolver falls back to the in-tree ordering when the export seam returns empty (C2)") {
  const std::vector<FolderThumbnailCandidate> descendants{
      track("a.mp3", "", std::nullopt),
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
      track("z/c.mp3", "z", std::filesystem::path{"/thumb/c.png"}),
  };

  const auto result = resolveFolderThumbnail("/music/Album", descendants, emptySeam());

  // (filename, 相对目录) 字典序：a < b < c，第一首有缩略图者为 b.mp3。
  CHECK(result.has_value());
  CHECK(result == std::filesystem::path{"/thumb/b.png"});
}

TEST_CASE("folder thumbnail resolver falls back when the export seam throws (C2)") {
  const FolderThumbnailExportSeam throwingSeam = [](const std::filesystem::path&) -> Thumb {
    throw std::runtime_error("export boom");
  };
  const std::vector<FolderThumbnailCandidate> descendants{
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };

  CHECK_NOTHROW([&] {
    const auto result = resolveFolderThumbnail("/music/Album", descendants, throwingSeam);
    CHECK(result.has_value());
    CHECK(result == std::filesystem::path{"/thumb/b.png"});
  }());
}

TEST_CASE("folder thumbnail resolver skips case 1 when the seam is null (C2)") {
  const std::vector<FolderThumbnailCandidate> descendants{
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };

  const auto result = resolveFolderThumbnail("/music/Album", descendants, FolderThumbnailExportSeam{});

  CHECK(result.has_value());
  CHECK(result == std::filesystem::path{"/thumb/b.png"});
}

TEST_CASE("folder thumbnail resolver returns empty for an empty folder and for coverless descendants (C2)") {
  CHECK(!resolveFolderThumbnail("/music/Empty", {}, emptySeam()).has_value());

  const std::vector<FolderThumbnailCandidate> coverless{
      track("a.mp3", "", std::nullopt),
      track("z/b.mp3", "z", std::nullopt),
  };
  CHECK(!resolveFolderThumbnail("/music/Album", coverless, emptySeam()).has_value());

  // 导出异常 + 无封面后代：同样回退为空，不抛。
  const FolderThumbnailExportSeam throwingSeam = [](const std::filesystem::path&) -> Thumb {
    throw std::runtime_error("export boom");
  };
  CHECK_NOTHROW([&] {
    const auto result = resolveFolderThumbnail("/music/Album", coverless, throwingSeam);
    CHECK(!result.has_value());
  }());
}

TEST_CASE("folder thumbnail resolver orders descendants by (filename, relative directory) ascending (C4)") {
  // 嵌套 z/c.mp3 不抢先：filename 'b' < 'c'。
  // 同名跨子目录 tiebreaker：空相对目录 < 'sub'。
  // ASCII 大小写敏感：'B' < 'a'。
  const std::vector<FolderThumbnailCandidate> descendants{
      track("sub/track.mp3", "sub", std::filesystem::path{"/thumb/sub.png"}),
      track("top/track.mp3", "", std::filesystem::path{"/thumb/top.png"}),
      track("B.mp3", "", std::filesystem::path{"/thumb/B.png"}),
      track("a.mp3", "", std::filesystem::path{"/thumb/a.png"}),
      track("z/c.mp3", "z", std::filesystem::path{"/thumb/c.png"}),
      track("z/b.mp3", "z", std::filesystem::path{"/thumb/zb.png"}),
  };

  const auto result = resolveFolderThumbnail("/music/Album", descendants, emptySeam());

  // 排序序：B.mp3 < a.mp3 < b(嵌套 z/b.mp3) < c(嵌套 z/c.mp3) < track.mp3(顶层) < track.mp3(sub)。
  CHECK(result.has_value());
  CHECK(result == std::filesystem::path{"/thumb/B.png"});
}

TEST_CASE("folder thumbnail resolver keeps the winner stable under shuffled input order (C4/C8)") {
  const std::vector<FolderThumbnailCandidate> inOrder{
      track("a.mp3", "", std::nullopt),
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
      track("z/c.mp3", "z", std::filesystem::path{"/thumb/c.png"}),
  };
  const std::vector<FolderThumbnailCandidate> shuffled{
      track("z/c.mp3", "z", std::filesystem::path{"/thumb/c.png"}),
      track("a.mp3", "", std::nullopt),
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };

  const auto first = resolveFolderThumbnail("/music/Album", inOrder, emptySeam());
  const auto second = resolveFolderThumbnail("/music/Album", shuffled, emptySeam());

  CHECK(first.has_value());
  CHECK(second.has_value());
  CHECK(first == second);
  CHECK(first == std::filesystem::path{"/thumb/b.png"});
}

TEST_CASE("folder thumbnail resolver is deterministic across consecutive runs with CJK filenames (C8)") {
  const std::vector<FolderThumbnailCandidate> descendants{
      track("音乐/一.mp3", "音乐", std::filesystem::path{"/thumb/one.png"}),
      track("音乐/三.mp3", "音乐", std::filesystem::path{"/thumb/three.png"}),
      track("曲/界.mp3", "曲", std::filesystem::path{"/thumb/boundary.png"}),
  };

  const auto first = resolveFolderThumbnail("/音乐库", descendants, emptySeam());
  const auto second = resolveFolderThumbnail("/音乐库", descendants, emptySeam());

  CHECK(first.has_value());
  CHECK(second.has_value());
  CHECK(first == second);
}

TEST_CASE("folder thumbnail resolver isolates a throwing seam per folder (C7)") {
  std::size_t throwingFolderCalls = 0;
  const FolderThumbnailExportSeam seam = [&throwingFolderCalls](const std::filesystem::path& directory) -> Thumb {
    if (directory == std::filesystem::path{"/music/BrokenFolder"}) {
      ++throwingFolderCalls;
      throw std::runtime_error("export boom");
    }
    if (directory == std::filesystem::path{"/music/GoodFolder"}) {
      return std::filesystem::path{"/artwork/good.png"};
    }
    return std::nullopt;
  };

  // 抛异常文件夹：不抛、回退树内兜底。
  const std::vector<FolderThumbnailCandidate> brokenDescendants{
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };
  CHECK_NOTHROW([&] {
    const auto brokenResult = resolveFolderThumbnail("/music/BrokenFolder", brokenDescendants, seam);
    CHECK(brokenResult == std::filesystem::path{"/thumb/b.png"});
  }());

  // 其余文件夹照常：case 1 命中、case 2 兜底均不受影响。
  const auto goodResult = resolveFolderThumbnail("/music/GoodFolder", brokenDescendants, seam);
  CHECK(goodResult == std::filesystem::path{"/artwork/good.png"});

  const auto otherResult = resolveFolderThumbnail("/music/OtherFolder", brokenDescendants, seam);
  CHECK(otherResult == std::filesystem::path{"/thumb/b.png"});

  CHECK(throwingFolderCalls == 1U);
}

TEST_CASE("folder thumbnail resolver performs zero tag reads during resolution (C9)") {
  // 计数 seam 是 resolver 唯一的对外调用钩子：断言整个 resolve 期间恰好一次
  // 导出调用（case 1 尝试），且结果全部来自树内预解析数据——无任何 tag 重读/解码。
  std::size_t exportCalls = 0;
  const FolderThumbnailExportSeam countingSeam = [&exportCalls](const std::filesystem::path&) -> Thumb {
    ++exportCalls;
    return std::nullopt;  // case 1 空 → 走 case 2 树内兜底
  };
  const std::vector<FolderThumbnailCandidate> descendants{
      track("a.mp3", "", std::nullopt),
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
      track("c.mp3", "", std::filesystem::path{"/thumb/c.png"}),
      track("z/d.mp3", "z", std::filesystem::path{"/thumb/d.png"}),
      track("z/e.mp3", "z", std::filesystem::path{"/thumb/e.png"}),
  };

  const auto result = resolveFolderThumbnail("/music/Album", descendants, countingSeam);

  CHECK(result.has_value());
  CHECK(result == std::filesystem::path{"/thumb/b.png"});
  // 5 个后代也只触发 1 次导出调用：无逐歌曲读取（历史 41s/2.3GB 教训的可执行回归）。
  CHECK(exportCalls == 1U);
}

TEST_CASE("folder thumbnail resolver never invokes tag reads when case 1 wins (C9)") {
  std::size_t exportCalls = 0;
  const FolderThumbnailExportSeam countingSeam = [&exportCalls](const std::filesystem::path&) -> Thumb {
    ++exportCalls;
    return std::filesystem::path{"/artwork/exported.png"};
  };
  const std::vector<FolderThumbnailCandidate> descendants{
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };

  const auto result = resolveFolderThumbnail("/music/Album", descendants, countingSeam);

  CHECK(result == std::filesystem::path{"/artwork/exported.png"});
  CHECK(exportCalls == 1U);
}

TEST_CASE("folder thumbnail resolver skips the root directory entirely (C6)") {
  std::size_t exportCalls = 0;
  const FolderThumbnailExportSeam seam = [&exportCalls](const std::filesystem::path&) -> Thumb {
    ++exportCalls;
    return std::filesystem::path{"/artwork/exported.png"};
  };
  const std::vector<FolderThumbnailCandidate> descendants{
      track("b.mp3", "", std::filesystem::path{"/thumb/b.png"}),
  };

  const auto result = resolveFolderThumbnail("/music", descendants, seam, /*isRoot=*/true);

  CHECK(!result.has_value());
  CHECK(exportCalls == 0U);
}

}  // namespace
}  // namespace seriona::scanner
