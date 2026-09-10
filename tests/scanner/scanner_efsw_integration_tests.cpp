// efsw 集成测试目标骨架（任务 5 建立；任务 9 填充真实场景）。
//
// 任务 9 计划场景（真实 efsw 监视器；deps.watcherFactory 留空，由 orchestrator 回填生产工厂）：
//   1. 移入文件 → 精准增量（无整根重扫、无 ScanError、快照与缓存一致）。
//   2. 移入目录（含嵌套子树/既有文件）→ 子树枚举 + 合并。
//   3. 移入目录内后续写入被实时捕获（watch 覆盖新子树）。
//   4. 移出（回归保持）。
//   5. 根内移动（回归保持）。
//   6. 尖峰风暴（快速批量创建/移动）。
//   7. 停止/析构竞态。
//
// 当前仅含一个占位用例（scanner service 可构造），不含业务断言；旧 wtr 集成测试
// （scanner_wtr_integration_tests.cpp）由任务 9 一并替换。

#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include <doctest.h>

#include <memory>

namespace seriona::scanner {
namespace {

TEST_CASE("efsw integration placeholder constructs scanner service") {
  test::TempScannerRoot temp{"scanner-efsw-integration"};
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = nullptr,
      .watcherFactory = nullptr,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr});
  REQUIRE(service != nullptr);
}

} // namespace
} // namespace seriona::scanner
