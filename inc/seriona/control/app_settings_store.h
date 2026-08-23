#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace seriona::control {

enum class AppSettingsErrorCode {
  InvalidGroup,
  InvalidKey,
  StorageError,
};

class AppSettingsError : public std::runtime_error {
public:
  AppSettingsError(AppSettingsErrorCode code, std::string message);

  [[nodiscard]] AppSettingsErrorCode code() const noexcept { return code_; }

private:
  AppSettingsErrorCode code_;
};

struct AppSettingsStoreConfig {
  std::filesystem::path databasePath;
};

// 一条前端应用设置：group+key 复合主键；value 为不透明字符串，
// 前端负责 QVariant ↔ 字符串的编解码，后端不做任何解释。
struct AppSettingsEntry {
  std::string group;
  std::string key;
  std::string value;
};

// 前端应用设置的键值存储（输出设置/导航状态/曲目统计）。
// 与 FolderSortSettingsStore 同库不同表；实现必须线程安全。
class AppSettingsStore {
public:
  virtual ~AppSettingsStore() = default;

  // 覆盖写入（不存在则插入，存在则更新）。
  virtual void set(std::string group, std::string key, std::string value) = 0;
  // 读取单条；无记录返回 nullopt。
  [[nodiscard]] virtual std::optional<std::string> get(const std::string& group, const std::string& key) const = 0;
  // 删除单条；不存在时静默成功。
  virtual void remove(const std::string& group, const std::string& key) = 0;
  // 列出一个组内全部条目（按 key 排序）。
  [[nodiscard]] virtual std::vector<AppSettingsEntry> listByGroup(const std::string& group) const = 0;
};

[[nodiscard]] std::unique_ptr<AppSettingsStore> makeSQLiteAppSettingsStore(AppSettingsStoreConfig config);

}
