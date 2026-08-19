#pragma once

#include "seriona/app/runtime_paths.h"

#include <spdlog/common.h>

namespace seriona::app {

void initializeApplicationLogging(const RuntimePaths& runtimePaths);

// 运行时设置全局日志等级：默认 logger 与全部已注册 named logger 及其 sink 同步
// 生效；线程安全（spdlog 保证），可从任意线程（如前端 UI 线程）调用。level 仅接受
// [trace, off] 有效区间，越界值被忽略（等级保持现状）。
void setLogLevel(spdlog::level::level_enum level);

}
