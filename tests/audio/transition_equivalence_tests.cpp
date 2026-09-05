#include "transition_equivalence_capture.h"
#include "transition_equivalence_sha256.h"

#include <doctest.h>

#include <string>
#include <vector>

// 默认值等价总闸（任务 13）：9 项过渡默认（TransitionConfig 默认构造 = "旧行为等价"）
// 下重放改动前基线（f5b1f5c）参考缓冲 → 当前树输出逐样本相等（最大偏差 0）。
//
// 本用例现场重新生成当前树缓冲并对比「已提交的基线 SHA-256 表」——基线原始缓冲
// 不入库（.omo/evidence/baseline-*.raw），入库的固化物 = 本表（键 → 长度 + 摘要）。
// 表值由 transition_equivalence_recorder --out 在基线 worktree 上录制生成。
namespace {

struct BaselineEntry {
  const char* key;
  std::size_t bytes;
  const char* sha256;
};

// 生成源：tests/audio/transition_equivalence_recorder.cpp（基线 f5b1f5c worktree 录制，
// manifest.sha 逐行核对后固化于此；切换实现/驱动节奏/测试夹具即失效，须重新录制）。
constexpr BaselineEntry kBaselineHashes[] = {
    {"pause_resume_Float32", 38400, "d16b2366b69279c1dfe05ef7e48c00a3e9bbfa4f9196738bbdc3d55be9b20d05"},
    {"pause_resume_Int16", 19200, "39d521e6e707d523e5989cdf381de5125f2947da6d76848cd6d769fafbb53589"},
    {"pause_resume_Int24", 28800, "5f4b919a7b016edb3cbdfdef0dd1a7945f57295ad53e6badb6673b773a3d0901"},
    {"pause_resume_Int32", 38400, "dc17d2f1e79fe58aa95b190b60457965ce200b193594326656b94692c8d7c0cf"},
    {"playthrough_Float32", 234240, "584cf187b4cf10c67afff493b5cc1e7ba5f4b91791d0e8dda8db878f94448299"},
    {"playthrough_Int16", 117120, "de83bf15f72a387c212e3bdf1b343ffff4354764160c71a78ceb782b9b972860"},
    {"playthrough_Int24", 175680, "6dd1d73819dd0eb899abe29c210ead20df0cc8db3aba3b48c45d7f66bc990f89"},
    {"playthrough_Int32", 234240, "0e82efdc746a099dd42066a92e3988d34fcfe10bb4c69219bbb05250d1d5fdfb"},
    {"seek_Float32", 38400, "d16b2366b69279c1dfe05ef7e48c00a3e9bbfa4f9196738bbdc3d55be9b20d05"},
    {"seek_Int16", 19200, "39d521e6e707d523e5989cdf381de5125f2947da6d76848cd6d769fafbb53589"},
    {"seek_Int24", 28800, "5f4b919a7b016edb3cbdfdef0dd1a7945f57295ad53e6badb6673b773a3d0901"},
    {"seek_Int32", 38400, "dc17d2f1e79fe58aa95b190b60457965ce200b193594326656b94692c8d7c0cf"},
    {"switch_direct", 9600, "9bc1be7131c64c8a1bdc699a492cc4d06381f7021607f899a7d1e1ccbd690fc0"},
    {"switch_mixed_Float32", 38400, "d16b2366b69279c1dfe05ef7e48c00a3e9bbfa4f9196738bbdc3d55be9b20d05"},
    {"switch_mixed_Int16", 19200, "39d521e6e707d523e5989cdf381de5125f2947da6d76848cd6d769fafbb53589"},
    {"switch_mixed_Int24", 28800, "5f4b919a7b016edb3cbdfdef0dd1a7945f57295ad53e6badb6673b773a3d0901"},
    {"switch_mixed_Int32", 38400, "dc17d2f1e79fe58aa95b190b60457965ce200b193594326656b94692c8d7c0cf"}
};

}  // namespace

// SHA-256 已知答案测试（评审 N4）：录制缓冲全部为 64 的倍数，中块填充分支（len%64≠0）
// 无真实数据覆盖；此处用非 64 倍长输入补覆盖。向量经 sha256sum CLI 交叉验证后固化
// （echo -n "abc" | sha256sum 等；N 项为 'a'×N，"a"*55 与 64 块边界跨块路径同查）。
TEST_CASE("equivalence sha256 known-answer vectors (incl. non-64-multiple inputs)") {
  const auto check = [](const char* label, const std::string& bytes, const char* expectedHex) {
    CAPTURE(label);
    const auto hex = seriona::audio::equivalence::sha256Hex(bytes);
    CAPTURE(hex);
    CHECK_MESSAGE(hex == expectedHex, "sha256 digest mismatch for known-answer vector");
  };
  check("empty", "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  check("abc", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  check("a*1", "a", "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
  check("a*55", std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  check("a*56", std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  check("a*63", std::string(63, 'a'), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
  check("a*64", std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  check("a*65", std::string(65, 'a'), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
  check("a*100", std::string(100, 'a'), "2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e");
}

TEST_CASE("transition default-equivalence total gate matches pre-change baseline buffers") {
  seriona::audio::EquivCaptureOptions options;
  options.formats = {seriona::audio::AudioSampleFormat::Float32,
                     seriona::audio::AudioSampleFormat::Int16,
                     seriona::audio::AudioSampleFormat::Int24,
                     seriona::audio::AudioSampleFormat::Int32};
  options.includeDirectSwitch = true;

  const auto captures = seriona::audio::runEquivalenceCaptures(options);
  for (const auto& entry : kBaselineHashes) {
    CAPTURE(entry.key);
    const auto iterator = captures.find(entry.key);
    REQUIRE_MESSAGE(iterator != captures.end(), "current capture missing for committed baseline key");
    const auto& capture = iterator->second;
    CAPTURE(capture.bytes.size());
    CHECK_MESSAGE(capture.bytes.size() == entry.bytes, "buffer length differs from committed baseline");
    const auto hex = seriona::audio::equivalence::sha256Hex(std::string_view{
        reinterpret_cast<const char*>(capture.bytes.data()), capture.bytes.size()});
    CAPTURE(hex);
    CHECK_MESSAGE(hex == entry.sha256, "buffer hash differs from committed baseline hash (deviation > 0)");
    // 欠载若进入捕获窗口必以补零污染样本 → 先击穿上方长度/哈希断言；哈希全一致时
    // 该事件只可能落在收尾/停止期（macOS 慢 runner 假阳性来源），故降级为 advisory。
    if (capture.underrunObserved) {
      MESSAGE("underrun event observed outside the capture window (advisory; hash gate holds)");
    }
  }
  MESSAGE("equivalence gate: ", captures.size(), " captures regenerated and compared");
}
