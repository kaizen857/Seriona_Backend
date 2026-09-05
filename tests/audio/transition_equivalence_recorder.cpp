#include "transition_equivalence_capture.h"
#include "transition_equivalence_sha256.h"

#include "seriona/audio/audio_contracts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// 等价录制独立二进制（doctest-less，自带 main）：
//   --out <dir>             录制全部 (场景 × 格式) 缓冲 → <dir>/<key>.raw + manifest.sha
//   --compare <dirA> <dirB> 逐字节/逐样本比对两目录同名键，报告最大偏差与尾部差异
// 同一驱动源可在基线 worktree（f5b1f5c）与当前树两处构建运行（见 capture.h 约束）。
namespace {

using namespace seriona::audio;

std::vector<AudioSampleFormat> parseFormats(const std::string& list) {
  std::vector<AudioSampleFormat> formats;
  std::string token;
  for (const char ch : list) {
    if (ch == ',') {
      if (token == "Float32") {
        formats.push_back(AudioSampleFormat::Float32);
      } else if (token == "Int16") {
        formats.push_back(AudioSampleFormat::Int16);
      } else if (token == "Int24") {
        formats.push_back(AudioSampleFormat::Int24);
      } else if (token == "Int32") {
        formats.push_back(AudioSampleFormat::Int32);
      } else {
        throw std::runtime_error("unknown format token: " + token);
      }
      token.clear();
    } else {
      token.push_back(ch);
    }
  }
  if (!token.empty()) {
    if (token == "Float32") {
      formats.push_back(AudioSampleFormat::Float32);
    } else if (token == "Int16") {
      formats.push_back(AudioSampleFormat::Int16);
    } else if (token == "Int24") {
      formats.push_back(AudioSampleFormat::Int24);
    } else if (token == "Int32") {
      formats.push_back(AudioSampleFormat::Int32);
    } else {
      throw std::runtime_error("unknown format token: " + token);
    }
  }
  if (formats.empty()) {
    throw std::runtime_error("no formats requested");
  }
  return formats;
}

void writeAll(const std::filesystem::path& dir, const EquivCaptureMap& captures) {
  std::filesystem::create_directories(dir);
  std::ofstream manifest(dir / "manifest.sha", std::ios::trunc);
  if (!manifest.good()) {
    throw std::runtime_error("cannot open manifest for writing: " + (dir / "manifest.sha").string());
  }
  std::cout << "captured keys: " << captures.size() << "\n";
  for (const auto& [key, capture] : captures) {
    const auto path = dir / (key + ".raw");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
      throw std::runtime_error("cannot write capture: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(capture.bytes.data()),
                 static_cast<std::streamsize>(capture.bytes.size()));
    output.close();
    const auto hex = equivalence::sha256Hex(std::string_view{
        reinterpret_cast<const char*>(capture.bytes.data()), capture.bytes.size()});
    manifest << hex << " " << key << " " << capture.bytes.size() << " " << capture.renderedFrames << " "
             << capture.sampleRate << " " << static_cast<int>(capture.channelCount) << " "
             << seriona::audio::formatName(capture.format) << " "
             << (capture.underrunObserved ? "underrun" : "ok") << "\n";
    std::cout << key << ": " << capture.bytes.size() << " bytes, " << capture.renderedFrames << " frames, "
              << (capture.underrunObserved ? "UNDERRUN!" : "no-underrun") << " sha=" << hex << "\n";
  }
  manifest.close();
}

struct SampleReader {
  AudioSampleFormat format{AudioSampleFormat::Float32};
  const std::vector<std::uint8_t>* bytes{nullptr};

  std::size_t bytesPerSample() const {
    switch (format) {
      case AudioSampleFormat::Int16:
        return 2U;
      case AudioSampleFormat::Int24:
        return 3U;
      case AudioSampleFormat::Int32:
      case AudioSampleFormat::Float32:
        return 4U;
      default:
        return 0U;
    }
  }

  double readAt(std::size_t sampleIndex) const {
    const std::size_t offset = sampleIndex * bytesPerSample();
    switch (format) {
      case AudioSampleFormat::Int16: {
        std::int16_t value = 0;
        std::memcpy(&value, bytes->data() + offset, sizeof(value));
        return static_cast<double>(value);
      }
      case AudioSampleFormat::Int24: {
        std::uint32_t raw = static_cast<std::uint32_t>(bytes->data()[offset]) |
                            (static_cast<std::uint32_t>(bytes->data()[offset + 1U]) << 8U) |
                            (static_cast<std::uint32_t>(bytes->data()[offset + 2U]) << 16U);
        if ((raw & 0x800000U) != 0U) {
          raw |= 0xFF000000U;
        }
        return static_cast<double>(static_cast<std::int32_t>(raw));
      }
      case AudioSampleFormat::Int32: {
        std::int32_t value = 0;
        std::memcpy(&value, bytes->data() + offset, sizeof(value));
        return static_cast<double>(value);
      }
      case AudioSampleFormat::Float32: {
        float value = 0.0F;
        std::memcpy(&value, bytes->data() + offset, sizeof(value));
        return static_cast<double>(value);
      }
      default:
        return 0.0;
    }
  }
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.good()) {
    throw std::runtime_error("cannot open capture file: " + path.string());
  }
  input.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(input.tellg());
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(size);
  if (size > 0U && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
    throw std::runtime_error("cannot read capture file: " + path.string());
  }
  return bytes;
}

// 逐样本比对结果：maxDeviation = 公共前缀内最大样本偏差（浮点域按数值差；全等 = 0），
// differingSamples = 公共前缀内差异样本数。判定以 differingSamples 为准——maxDeviation
// 只作证据展示：NaN 输入参与算术比较会坍缩（std::max(0.0, NaN) 得 0.0 或 NaN），
// 故 NaN 样本显式计数为差异但不上卷 maxDeviation。
struct CompareResult {
  double maxDeviation{0.0};
  std::size_t differingSamples{0U};
};

CompareResult compareCaptures(const std::string& key,
                              const std::vector<std::uint8_t>& a,
                              const std::vector<std::uint8_t>& b,
                              AudioSampleFormat format) {
  const SampleReader readerA{format, &a};
  const SampleReader readerB{format, &b};
  const std::size_t sampleBytes = readerA.bytesPerSample();
  if (sampleBytes == 0U) {
    throw std::runtime_error("compare: unknown format for key " + key);
  }
  const std::size_t prefixBytes = std::min(a.size(), b.size());
  const std::size_t prefixSamples = prefixBytes / sampleBytes;
  CompareResult result;
  for (std::size_t sample = 0; sample < prefixSamples; ++sample) {
    const double va = readerA.readAt(sample);
    const double vb = readerB.readAt(sample);
    if (std::isnan(va) || std::isnan(vb)) {
      // NaN 与任何值（含自身）都不相等：显式计为差异样本，避免 max 坍缩吞掉差异。
      ++result.differingSamples;
      continue;
    }
    const double deviation = std::abs(va - vb);
    if (deviation != 0.0) {
      ++result.differingSamples;
      result.maxDeviation = std::max(result.maxDeviation, deviation);
    }
  }
  std::cout << key << ": A=" << a.size() << "B bytes, B=" << b.size() << "B bytes, prefix=" << prefixBytes
            << "B, differing samples=" << result.differingSamples << "/" << prefixSamples
            << ", max deviation=" << result.maxDeviation << "\n";
  if (a.size() != b.size()) {
    std::cout << "  !! LENGTH MISMATCH: A tail " << (a.size() > b.size() ? a.size() - b.size() : 0U)
              << " extra bytes, B tail " << (b.size() > a.size() ? b.size() - a.size() : 0U) << " extra bytes\n";
  }
  return result;
}

int compareDirs(const std::filesystem::path& dirA, const std::filesystem::path& dirB) {
  // 逐键格式取自 dirA 的 manifest.sha（录制期固化：<sha> <key> <len> <frames> <rate> <ch> <fmt> <ok>）；
  // 不能按键名猜测（switch_direct 无格式后缀，Int16 数据被误读成 Float32 会产生 NaN 假差异）。
  std::map<std::string, AudioSampleFormat> formatByKey;
  const auto manifestPath = dirA / "manifest.sha";
  if (!std::filesystem::exists(manifestPath)) {
    // manifest 缺失 = 无法得知各键样本格式：显式失败，拒绝静默回退 Float32 误读。
    throw std::runtime_error("manifest.sha missing in " + dirA.string() +
                             " (cannot determine per-key sample format)");
  }
  {
    std::ifstream manifest(manifestPath);
    std::string sha;
    std::string key;
    std::string format;
    std::size_t length = 0;
    std::uint64_t frames = 0;
    std::uint32_t rate = 0;
    int channels = 0;
    std::string status;
    while (manifest >> sha >> key >> length >> frames >> rate >> channels >> format >> status) {
      if (format == "Float32") {
        formatByKey[key] = AudioSampleFormat::Float32;
      } else if (format == "Int16") {
        formatByKey[key] = AudioSampleFormat::Int16;
      } else if (format == "Int24") {
        formatByKey[key] = AudioSampleFormat::Int24;
      } else if (format == "Int32") {
        formatByKey[key] = AudioSampleFormat::Int32;
      } else {
        // 未知格式同样显式失败：读错样本宽度会产生假差异或漏报，不比猜测更安全。
        throw std::runtime_error("manifest.sha: unknown format '" + format + "' for key " + key);
      }
    }
  }
  int failures = 0;
  std::vector<std::string> keys;
  for (const auto& entry : std::filesystem::directory_iterator(dirA)) {
    const auto name = entry.path().filename().string();
    if (name.size() > 4U && name.substr(name.size() - 4U) == ".raw") {
      keys.push_back(name.substr(0U, name.size() - 4U));
    }
  }
  std::sort(keys.begin(), keys.end());
  for (const auto& key : keys) {
    const auto pathA = dirA / (key + ".raw");
    const auto pathB = dirB / (key + ".raw");
    if (!std::filesystem::exists(pathB)) {
      std::cout << key << ": MISSING in " << dirB.string() << "\n";
      ++failures;
      continue;
    }
    const auto bytesA = readFile(pathA);
    const auto bytesB = readFile(pathB);
    const auto formatIterator = formatByKey.find(key);
    if (formatIterator == formatByKey.end()) {
      // manifest 有键无格式项 = 清单与产物不一致：显式失败，拒绝 Float32 猜测。
      throw std::runtime_error("manifest.sha: no format entry for key " + key);
    }
    const CompareResult result = compareCaptures(key, bytesA, bytesB, formatIterator->second);
    // 判定以差异样本数为准（NaN 差异经 max 坍缩后 maxDeviation 可能仍为 0）。
    if (result.differingSamples != 0U || bytesA.size() != bytesB.size()) {
      ++failures;
    }
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string mode;
    std::filesystem::path dirA;
    std::filesystem::path dirB;
    std::string formats = "Float32,Int16,Int24,Int32";
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--out" && index + 1 < argc) {
        mode = "record";
        dirA = argv[++index];
      } else if (argument == "--compare" && index + 2 < argc) {
        mode = "compare";
        dirA = argv[++index];
        dirB = argv[++index];
      } else if (argument == "--formats" && index + 1 < argc) {
        formats = argv[++index];
      } else {
        throw std::runtime_error("usage: recorder --out <dir> [--formats F1,F2,...] | --compare <dirA> <dirB>");
      }
    }
    if (mode == "record") {
      EquivCaptureOptions options;
      options.formats = parseFormats(formats);
      options.includeDirectSwitch = true;
      const auto captures = seriona::audio::runEquivalenceCaptures(options);
      writeAll(dirA, captures);
      std::cout << "record OK -> " << dirA.string() << "\n";
      return 0;
    }
    if (mode == "compare") {
      const int failures = compareDirs(dirA, dirB);
      std::cout << (failures == 0 ? "COMPARE OK: all captures byte-identical" : "COMPARE FAILED") << "\n";
      return failures == 0 ? 0 : 1;
    }
    throw std::runtime_error("no mode given");
  } catch (const std::exception& error) {
    std::cerr << "recorder error: " << error.what() << "\n";
    return 2;
  }
}
