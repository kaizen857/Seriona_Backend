#pragma once

#include "seriona/control/control_contracts.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seriona::control {

class MediaController {
public:
  explicit MediaController(MediaControllerDependencies dependencies, MediaControllerOptions options = {});
  ~MediaController();

  MediaController(const MediaController&) = delete;
  MediaController& operator=(const MediaController&) = delete;
  MediaController(MediaController&&) noexcept;
  MediaController& operator=(MediaController&&) noexcept;

  void start();
  void shutdown();
  MediaControllerCommandResult submitCommand(const MediaControlCommand& command);
  // 前端应用设置读写：与命令同模式（控制事件循环串行化），未启动时 get 返回
  // nullopt、set/remove 返回拒绝结果；存储不可用时同样安全降级。
  [[nodiscard]] std::optional<std::string> getAppSetting(const std::string& group, const std::string& key);
  MediaControllerCommandResult setAppSetting(std::string group, std::string key, std::string value);
  MediaControllerCommandResult removeAppSetting(std::string group, std::string key);
  MediaControllerCommandResult scanLibrary(std::vector<scanner::ScannerRoot> roots, scanner::ScanMode mode);
  SubscriptionHandle subscribePlayerState(PlayerStateSnapshotCallback callback);
  SubscriptionHandle subscribeLibraryState(LibraryStateSnapshotCallback callback);
  SubscriptionHandle subscribeDomainNotifications(ControlDomainNotificationCallback callback);
  // 均衡器状态订阅：注册后立即以当前生效快照回调一次（订阅即回调当前生效快照，
  // generation=0 表示从未生效的空快照），此后每次均衡器参数生效（generation 递增）
  // 再次回调。与 subscribePlayerState 同为初始快照 + 增量通知语义。
  SubscriptionHandle subscribeEqualizerState(EqualizerStateSnapshotCallback callback);
  // 频谱订阅：注册后立即以当前频谱快照回调一次；无生效频谱输出时回调空快照
  // （generation=0），此后每次频谱更新再次回调。
  SubscriptionHandle subscribeSpectrum(SpectrumSnapshotCallback callback);
  [[nodiscard]] PlayerStateSnapshot playerStateSnapshot() const;
  [[nodiscard]] LibraryStateSnapshot libraryStateSnapshot() const;
  [[nodiscard]] std::vector<audio::AudioDeviceFormat> enumeratePlaybackDevices() const;
  [[nodiscard]] audio::BackendEventSink audioEventSink();
  [[nodiscard]] scanner::ScannerEventSink scannerEventSink();
  void drainForTests();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<MediaController> makeMediaController(MediaControllerDependencies dependencies,
                                                                    MediaControllerOptions options = {});
[[nodiscard]] std::unique_ptr<MediaController> makeProductionMediaController(MediaControllerOptions options = {});
[[nodiscard]] std::unique_ptr<MediaController> makeProductionMediaController(
    MediaControllerOptions options,
    std::filesystem::path databasePath,
    std::filesystem::path coverExportDir);

}
