// 仅 Linux 编译：PipeWire 原生设备采样率/位深格式枚举。
//
// miniaudio 的 pipewire/pulse 后端都无法完整枚举设备格式（pulse 只填
// 1 个 nativeDataFormat，pipewire 只报 f32 + core clockRate），设备真实的
// 格式能力只能从 libpipewire 原生 SPA EnumFormat 参数读取。本文件用
// pw_thread_loop + registry 发现 Audio/Sink 节点，逐个 pw_node_enum_params
// 拉取 EnumFormat，解析 Audio:format / Audio:rate 的 Choice:Enum（或
// Choice:Range）列表，映射为 AudioSampleFormat 能力。
//
// 线程模型：每次 enumerate() 新建独立 context/连接（不持有全局连接），
// 可在任意工作线程调用（enumeratePlaybackDevices 运行在 audio worker 线程）。
// pw_thread_loop 的事件回调在 loop 锁内执行，主线程经 timed_wait 轮询
// 共享状态，无数据竞争。

#if defined(__linux__) && !defined(__APPLE__)

#include "seriona/audio/device/audio_device_format_enumerator.h"

#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace seriona::audio {
namespace detail {
namespace {

// 采样率 Choice:Range 场景补全的标准采样率表（无 allowed-rates 的节点
// 常以 min/max 范围上报，实际可用采样率包含这些常见值）。
constexpr std::array<std::uint32_t, 4> kStandardRates{44100, 48000, 96000, 192000};

// 节点枚举"完成"的静默期：服务端结果流结束后不再有参数事件，
// 静默超过该时长即认为该节点枚举完毕。
constexpr auto kEnumSilenceWindow = std::chrono::milliseconds{200};

// SPA_AUDIO_FORMAT → AudioSampleFormat 映射。只保留滤镜链（ffmpeg
// toPackedAvSampleFormat）支持的四种格式：Int16/Int24/Int32/Float32；
// 其余（U8/F64/planar 变体等）返回 nullopt 跳过，等价于与滤镜链求交。
// 注意 SPA_AUDIO_FORMAT_S16 等平台无关值与 _BE 同值，故用 if 链而非
// switch（case 重复会编译失败）。
std::optional<AudioSampleFormat> mapSpaAudioFormat(std::uint32_t spaFormat) {
  if (spaFormat == SPA_AUDIO_FORMAT_S16 || spaFormat == SPA_AUDIO_FORMAT_S16_LE ||
      spaFormat == SPA_AUDIO_FORMAT_S16_BE) {
    return AudioSampleFormat::Int16;
  }
  if (spaFormat == SPA_AUDIO_FORMAT_S24_32 || spaFormat == SPA_AUDIO_FORMAT_S24_32_LE ||
      spaFormat == SPA_AUDIO_FORMAT_S24_32_BE || spaFormat == SPA_AUDIO_FORMAT_S24 ||
      spaFormat == SPA_AUDIO_FORMAT_S24_LE || spaFormat == SPA_AUDIO_FORMAT_S24_BE) {
    // S24_32（32 位容器 24 位有效）与 3 字节 S24 都映射为 Int24。
    return AudioSampleFormat::Int24;
  }
  if (spaFormat == SPA_AUDIO_FORMAT_S32 || spaFormat == SPA_AUDIO_FORMAT_S32_LE ||
      spaFormat == SPA_AUDIO_FORMAT_S32_BE) {
    return AudioSampleFormat::Int32;
  }
  if (spaFormat == SPA_AUDIO_FORMAT_F32 || spaFormat == SPA_AUDIO_FORMAT_F32_LE ||
      spaFormat == SPA_AUDIO_FORMAT_F32_BE) {
    return AudioSampleFormat::Float32;
  }
  return std::nullopt;
}

// 对 pod 取值（普通值或 Choice 的每个候选值）逐个回调，回调收到裸整数值。
// Choice 的值区是连续的裸值（无 spa_pod 头，child.size 为单值字节数），
// 不能对值区用 spa_pod_get_id/get_int；普通 pod 按类型取值。
template <typename Fn>
void collectPodValues(const struct spa_pod* pod, Fn&& fn) {
  if (pod == nullptr) {
    return;
  }
  if (spa_pod_is_choice(pod)) {
    const auto* choice = reinterpret_cast<const struct spa_pod_choice*>(pod);
    const auto nValues = SPA_POD_CHOICE_N_VALUES(choice);
    const auto valueSize = SPA_POD_CHOICE_VALUE_SIZE(choice);
    const auto* values = SPA_POD_CHOICE_VALUES(choice);
    if (valueSize != sizeof(std::uint32_t)) {
      return;
    }
    for (std::uint32_t i = 0; i < nValues; ++i) {
      std::uint32_t rawValue = 0;
      std::memcpy(&rawValue, SPA_PTROFF(values, static_cast<std::size_t>(i) * valueSize, void),
                  sizeof(rawValue));
      fn(rawValue);
    }
    return;
  }

  std::uint32_t value = 0;
  if (pod->type == SPA_TYPE_Id) {
    if (spa_pod_get_id(pod, &value) != 0) {
      return;
    }
  } else if (pod->type == SPA_TYPE_Int) {
    std::int32_t intValue = 0;
    if (spa_pod_get_int(pod, &intValue) != 0) {
      return;
    }
    value = static_cast<std::uint32_t>(intValue);
  } else {
    return;
  }
  fn(value);
}

// 收集 Audio:format 属性值（Id 类型，Choice:Enum 场景取全部候选）。
void collectFormatValues(const struct spa_pod* pod, std::vector<AudioSampleFormat>& out) {
  collectPodValues(pod, [&out](std::uint32_t spaFormat) {
    const auto mapped = mapSpaAudioFormat(spaFormat);
    if (mapped.has_value()) {
      out.push_back(*mapped);
    }
  });
}

// 收集 Audio:rate 属性值（Int 类型）。Choice:Enum 场景逐个收集全部枚举值；
// Choice:Range 场景收集默认值 + min/max 边界，并补标准采样率表中落在
// [min, max] 内的值（等价于节点允许的常见采样率）。
void collectRateValues(const struct spa_pod* pod, std::vector<std::uint32_t>& out) {
  if (pod == nullptr) {
    return;
  }

  bool isRange = false;
  if (spa_pod_is_choice(pod)) {
    isRange = reinterpret_cast<const struct spa_pod_choice*>(pod)->body.type == SPA_CHOICE_Range;
  }

  std::vector<std::uint32_t> collected;
  collectPodValues(pod, [&collected](std::uint32_t rate) {
    // 过滤非常规值（如 1 / INT32_MAX 占位）：音频采样率合理范围。
    if (rate >= 1000U && rate <= 1'000'000U) {
      collected.push_back(rate);
    }
  });

  if (isRange && collected.size() >= 3U) {
    // SPA_CHOICE_Range 值序：default, min, max。
    const auto minRate = collected[1];
    const auto maxRate = collected[2];
    for (const auto standardRate : kStandardRates) {
      if (standardRate >= minRate && standardRate <= maxRate) {
        collected.push_back(standardRate);
      }
    }
  }

  for (const auto rate : collected) {
    out.push_back(rate);
  }
}

// 单个 Audio/Sink 节点的枚举状态。由 pw_node_events.param 回调填充。
// 注意：pipewire 服务端的枚举结果流（含 cached 路径）只发参数事件、
// 通常不发 param == nullptr 结束事件——节点"完成"以收到参数事件后的
// 静默期判定（见 kEnumSilenceWindow），param == nullptr 时立即完成兜底。
struct NodeEnumState {
  std::string deviceId;
  std::string deviceName;
  std::vector<AudioSampleFormat> formats;
  std::vector<std::uint32_t> rates;
  struct pw_node* node{nullptr};
  struct spa_hook nodeListener{};
  int seq{0};
  std::chrono::steady_clock::time_point lastParamAt{};
  bool done{false};
};

// 一次 enumerate() 会话的共享状态。回调在 pw_thread_loop 锁内执行，
// 主线程经 timed_wait 轮询读取，无数据竞争。
struct EnumSession {
  std::vector<std::unique_ptr<NodeEnumState>> nodes;
  struct pw_registry* registry{nullptr};
  struct spa_hook registryListener{};
  struct spa_hook coreListener{};
  int coreSyncSeq{-1};
  bool initialListDone{false};
};

// node listener 的 param 事件：enum_params 请求的每个结果都在这里返回。
// param == nullptr 是部分实现发的结束标记，收到即视为完成；其余实现
// 靠调用方静默期判定。
void nodeParam(void* data, int seq, std::uint32_t id, std::uint32_t index, std::uint32_t next,
               const struct spa_pod* param) {
  (void)index;
  (void)next;
  auto* state = static_cast<NodeEnumState*>(data);
  if (id != SPA_PARAM_EnumFormat || seq != state->seq) {
    return;
  }
  if (param == nullptr) {
    state->done = true;
    return;
  }

  state->lastParamAt = std::chrono::steady_clock::now();
  state->lastParamAt = std::chrono::steady_clock::now();
  const auto* formatProp = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_format);
  if (formatProp != nullptr) {
    collectFormatValues(&formatProp->value, state->formats);
  }
  const auto* rateProp = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate);
  if (rateProp != nullptr) {
    collectRateValues(&rateProp->value, state->rates);
  }
}

// node listener 事件表。
const struct pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = nullptr,
    .param = nodeParam,
};

// registry global 回调：过滤 Audio/Sink 节点，bind 后发起 EnumFormat 请求。
void registryGlobal(void* data,
                    std::uint32_t id,
                    std::uint32_t permissions,
                    const char* type,
                    std::uint32_t version,
                    const struct spa_dict* props) {
  (void)permissions;
  (void)version;
  auto* session = static_cast<EnumSession*>(data);
  if (type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
    return;
  }
  const char* mediaClass = props != nullptr ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : nullptr;
  if (mediaClass == nullptr || std::strcmp(mediaClass, "Audio/Sink") != 0) {
    return;
  }

  // deviceId 优先 node.name（如 alsa_output.pci-0000_04_00.6.HiFi__Speaker__sink），
  // 无则 device.name；deviceName 优先 node.description（如 "Ryzen HD Audio
  // Controller Speaker"），无则 node.name。
  const char* nodeName = props != nullptr ? spa_dict_lookup(props, PW_KEY_NODE_NAME) : nullptr;
  const char* deviceName = props != nullptr ? spa_dict_lookup(props, PW_KEY_DEVICE_NAME) : nullptr;
  const char* nodeDescription = props != nullptr ? spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION) : nullptr;

  auto state = std::make_unique<NodeEnumState>();
  state->deviceId = nodeName != nullptr ? nodeName : (deviceName != nullptr ? deviceName : "");
  state->deviceName = nodeDescription != nullptr ? nodeDescription : (nodeName != nullptr ? nodeName : "");
  if (state->deviceId.empty() && state->deviceName.empty()) {
    return;
  }

  auto* proxy = pw_registry_bind(session->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
  if (proxy == nullptr) {
    spdlog::debug("pipewire format enumerator: bind node {} failed", id);
    return;
  }
  state->node = static_cast<struct pw_node*>(proxy);
  if (pw_node_add_listener(state->node, &state->nodeListener, &kNodeEvents, state.get()) < 0) {
    pw_proxy_destroy(static_cast<struct pw_proxy*>(proxy));
    return;
  }

  // enum_params 返回服务器分配的 seq（int，>=0），param 事件回显该 seq；
  // 不能用自增序号匹配，否则事件被丢弃、节点永不完成。
  const int enumSeq = pw_node_enum_params(state->node, 0, SPA_PARAM_EnumFormat, 0, 0, nullptr);
  if (enumSeq < 0) {
    state->done = true;
  } else {
    state->seq = enumSeq;
  }
  state->lastParamAt = std::chrono::steady_clock::now();
  session->nodes.push_back(std::move(state));
}

// core done 事件：pw_core_sync 的应答，表示 registry 初始 global 列表
// 已全部送达，后续不再有新的初始节点。
void coreDone(void* data, std::uint32_t id, int seq) {
  auto* session = static_cast<EnumSession*>(data);
  if (id == PW_ID_CORE && seq == session->coreSyncSeq) {
    session->initialListDone = true;
  }
}

// core 事件表：字段按 struct pw_core_events 声明顺序全部列出
// （version, info, done, ping, error, remove_id, bound_id, add_mem,
// remove_mem, bound_props），避免部分初始化告警。
const struct pw_core_events kCoreEvents = {
    PW_VERSION_CORE_EVENTS,
    nullptr,
    coreDone,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

const struct pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registryGlobal,
    .global_remove = nullptr,
};

// 聚合节点结果：去重排序后组装 DeviceFormatCapabilities。
std::vector<DeviceFormatCapabilities> collectCapabilities(const EnumSession& session) {
  std::vector<DeviceFormatCapabilities> result;
  for (const auto& node : session.nodes) {
    if (node->formats.empty() && node->rates.empty()) {
      // 无任何 EnumFormat 结果（枚举失败或节点无格式能力）——跳过，
      // 该设备保留播放后端自身报告。
      continue;
    }
    DeviceFormatCapabilities caps{};
    caps.deviceId = node->deviceId;
    caps.deviceName = node->deviceName;

    std::sort(node->formats.begin(), node->formats.end());
    node->formats.erase(std::unique(node->formats.begin(), node->formats.end()), node->formats.end());
    caps.supportedSampleFormats = node->formats;

    std::sort(node->rates.begin(), node->rates.end());
    node->rates.erase(std::unique(node->rates.begin(), node->rates.end()), node->rates.end());
    caps.supportedSampleRates = node->rates;

    result.push_back(std::move(caps));
  }
  return result;
}

std::vector<DeviceFormatCapabilities> enumeratePipeWireDevices() {
  // pw_init 引用计数式初始化（返回 void），与末尾 pw_deinit 配对；
  // 初始化失败会体现在后续 pw_context_connect 失败上。
  pw_init(nullptr, nullptr);

  auto* loop = pw_thread_loop_new("seriona-format-enum", nullptr);
  if (loop == nullptr) {
    spdlog::warn("pipewire format enumerator: pw_thread_loop_new failed");
    pw_deinit();
    return {};
  }

  auto* context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
  if (context == nullptr) {
    spdlog::warn("pipewire format enumerator: pw_context_new failed");
    pw_thread_loop_destroy(loop);
    pw_deinit();
    return {};
  }

  auto* core = pw_context_connect(context, nullptr, 0);
  if (core == nullptr) {
    // 无 PipeWire 守护进程 / 无法连接：返回空列表，调用方保留设备自身报告。
    spdlog::warn("pipewire format enumerator: cannot connect to pipewire daemon");
    pw_context_destroy(context);
    pw_thread_loop_destroy(loop);
    pw_deinit();
    return {};
  }

  EnumSession session{};
  if (pw_thread_loop_start(loop) < 0) {
    spdlog::warn("pipewire format enumerator: pw_thread_loop_start failed");
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_thread_loop_destroy(loop);
    pw_deinit();
    return {};
  }

  pw_thread_loop_lock(loop);
  if (pw_core_add_listener(core, &session.coreListener, &kCoreEvents, &session) < 0) {
    spdlog::warn("pipewire format enumerator: pw_core_add_listener failed");
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_thread_loop_destroy(loop);
    pw_deinit();
    return {};
  }
  session.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (session.registry == nullptr) {
    spdlog::warn("pipewire format enumerator: pw_core_get_registry failed");
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_thread_loop_destroy(loop);
    pw_deinit();
    return {};
  }
  pw_registry_add_listener(session.registry, &session.registryListener, &kRegistryEvents, &session);
  // sync：registry 初始列表送达后 core done 事件到达，作为初始发现完成的信号。
  session.coreSyncSeq = pw_core_sync(core, PW_ID_CORE, 0);
  pw_thread_loop_unlock(loop);

  // 轮询等待：初始列表完成 && 全部已发现节点枚举结束（param == nullptr
  // 立即完成，或最后参数事件后静默 kEnumSilenceWindow），或 3 秒超时。
  // 每轮用 100ms 短超时（绝对时间），不依赖 pw_thread_loop_signal 语义，
  // 保证事件到达后能及时检查条件。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  bool finished = false;
  while (!finished) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      spdlog::warn("pipewire format enumerator: enumeration timed out after 3s ({} sink node(s))",
                   session.nodes.size());
      break;
    }

    pw_thread_loop_lock(loop);
    const bool allNodesSilent = std::all_of(session.nodes.begin(), session.nodes.end(),
                                            [&](const std::unique_ptr<NodeEnumState>& node) {
                                              return node->done || (now - node->lastParamAt) >= kEnumSilenceWindow;
                                            });
    finished = session.initialListDone && allNodesSilent;
    pw_thread_loop_unlock(loop);
    if (finished) {
      break;
    }

    // pw_thread_loop_get_time 返回 CLOCK_REALTIME 绝对时间（cond_timedwait
    // 用 REALTIME，不能用 steady_clock 计算 abstime，否则立即超时变成
    // busy loop 并与 loop 线程抢锁）。
    struct timespec abstime{};
    if (pw_thread_loop_get_time(loop, &abstime, 100'000'000) == 0) {
      pw_thread_loop_lock(loop);
      pw_thread_loop_timed_wait_full(loop, &abstime);
      pw_thread_loop_unlock(loop);
    }
  }

  auto result = collectCapabilities(session);
  if (!session.nodes.empty()) {
    spdlog::debug("pipewire format enumerator: {} sink node(s), {} with format capabilities",
                  session.nodes.size(), result.size());
  }

  // 清理：loop 停止后销毁 proxy、断开连接。node listener 随 proxy 销毁
  // 自动移除。
  pw_thread_loop_stop(loop);
  for (const auto& node : session.nodes) {
    if (node->node != nullptr) {
      pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(node->node));
    }
  }
  if (session.registry != nullptr) {
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(session.registry));
  }
  pw_core_disconnect(core);
  pw_context_destroy(context);
  pw_thread_loop_destroy(loop);
  pw_deinit();

  return result;
}

}

// PipeWire 枚举器实现（工厂声明的函数，定义在此）。
class PipeWireDeviceFormatEnumerator final : public DeviceFormatEnumerator {
public:
  [[nodiscard]] std::vector<DeviceFormatCapabilities> enumerate() override { return enumeratePipeWireDevices(); }
};

std::unique_ptr<DeviceFormatEnumerator> makePipeWireDeviceFormatEnumerator() {
  return std::make_unique<PipeWireDeviceFormatEnumerator>();
}

}
}

#endif  // defined(__linux__) && !defined(__APPLE__)
