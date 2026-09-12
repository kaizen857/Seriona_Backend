// SerionaBackend 安装导出最小消费者：仅使用 find_package 提供的公共头与
// scanner 静态库符号做链接自检，成功后打印固定成功标记。
//
// 覆盖点：
// - scanner 模块符号（scanner_module.cpp 内部锚定 efsw 事件枚举）；
// - 生产 scanner 工厂 makeFileScannerService() 的最小构造路径；
// - 链接期对 SerionaBackend::seriona_scanner 导出接口的全部传递依赖
//   （efsw-static / TagReaderCore / SQLite3 / xxhash / spdlog / Threads 等）。
#include <seriona/scanner/file_scanner_service.h>
#include <seriona/scanner/scanner_module.h>

#include <cstdio>
#include <exception>
#include <memory>

int main() {
  if (!seriona::scanner::scannerModuleLinked()) {
    std::fprintf(stderr, "seriona export consumer failed: scanner module not linked\n");
    return 1;
  }

  std::shared_ptr<seriona::scanner::FileScannerService> service;
  try {
    service = seriona::scanner::makeFileScannerService();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "seriona export consumer failed: %s\n", error.what());
    return 1;
  }
  if (!service) {
    std::fprintf(stderr, "seriona export consumer failed: null scanner service\n");
    return 1;
  }

  service.reset();
  std::printf("seriona export consumer ok\n");
  return 0;
}
