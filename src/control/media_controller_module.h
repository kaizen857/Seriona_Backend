#pragma once

#include "seriona/control/control_contracts.h"
#include "seriona/control/media_controller.h"

#include <filesystem>
#include <string>

namespace seriona::control {

[[nodiscard]] MediaControllerDependencies makeDefaultMediaControllerDependencies();
[[nodiscard]] MediaControllerDependencies makeProductionMediaControllerDependencies(
    std::filesystem::path databasePath = {},
    std::filesystem::path coverExportDir = {});
void normalizeMediaControllerDependencies(MediaControllerDependencies& dependencies);

// 控制层内部 seam（非稳定契约，不进 inc/seriona/control/control_contracts.h）：
// 「当前曲目切分歌词」的目标语言与重算入口。公共命令面已有 `SetLyricsTargetLanguage`
// 与三条纠错命令（`UpsertLyricSplitCorrection` / `RemoveLyricSplitCorrection` /
// `ClearLyricSplitCorrections`），它们经控制事件循环驱动同一批内部入口；本 seam 让
// 测试可绕过命令直接驱动这些入口（照 tests/control/ 的既有先例），并提供「按当前语言
// 重算并重发布一次」这一独立入口。
struct MediaControllerInternalAccess {
  // 更新目标语言并按新语言重算+重发布；调用被投递到控制事件循环（内部 inline 测试
  // 模式同步执行）。
  static void setLyricsTargetLanguage(MediaController& controller, std::string targetLanguage);
  // 以当前目标语言重算并重发布一次全量快照（手工纠错增删后由调用方触发）。
  static void refreshTrackLyrics(MediaController& controller);
  [[nodiscard]] static std::string lyricsTargetLanguage(const MediaController& controller);
};

}
