#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// SHA-256（FIPS 180-4）紧凑实现，仅供任务 13 等价录制/断言用例对捕获缓冲计算
// 摘要使用（不引入第三方依赖；正确性在证据阶段与 sha256sum 交叉验证）。
namespace seriona::audio::equivalence {

class Sha256 {
public:
  Sha256() { reset(); }

  void reset() {
    state_ = {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
              0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL};
    buffer_.fill(0);
    bufferLen_ = 0;
    totalBytes_ = 0;
  }

  void update(const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    totalBytes_ += length;
    while (length > 0) {
      const std::size_t space = 64U - bufferLen_;
      const std::size_t take = length < space ? length : space;
      for (std::size_t i = 0; i < take; ++i) {
        buffer_[bufferLen_ + i] = bytes[i];
      }
      bufferLen_ += take;
      bytes += take;
      length -= take;
      if (bufferLen_ == 64U) {
        processBlock(buffer_.data());
        bufferLen_ = 0;
      }
    }
  }

  [[nodiscard]] std::string hexDigest() {
    // FIPS 180-4 填充：0x80 → 补零至 ≡56 (mod 64) → 64 位大端比特长度。
    const std::uint64_t bitCount = totalBytes_ * 8U;
    const std::uint8_t pad = 0x80U;
    update(&pad, 1U);
    while (bufferLen_ != 56U) {
      const std::uint8_t zero = 0U;
      update(&zero, 1U);
    }
    std::array<std::uint8_t, 8> lengthBytes{};
    for (int i = 0; i < 8; ++i) {
      lengthBytes[static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>((bitCount >> (56U - 8U * static_cast<unsigned>(i))) & 0xFFU);
    }
    update(lengthBytes.data(), lengthBytes.size());
    // 填充后总长为 64 的倍数：最后一次 update 必已把长度块整体冲入 processBlock，
    // 缓冲应为空。长度校验在证据阶段与 sha256sum 交叉比对。
    std::string hex;
    hex.reserve(64U);
    static constexpr char kHex[] = "0123456789abcdef";
    for (const std::uint32_t word : state_) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        const std::uint8_t byte = static_cast<std::uint8_t>((word >> shift) & 0xFFU);
        hex.push_back(kHex[(byte >> 4U) & 0x0FU]);
        hex.push_back(kHex[byte & 0x0FU]);
      }
    }
    return hex;
  }

private:
  static std::uint32_t rotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
  }

  void processBlock(const std::uint8_t* block) {
    static constexpr std::array<std::uint32_t, 64> kConstants{
        0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL,
        0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL,
        0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL,
        0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
        0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
        0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
        0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL,
        0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
        0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL,
        0xc67178f2UL};

    std::array<std::uint32_t, 64> words{};
    for (int i = 0; i < 16; ++i) {
      words[static_cast<std::size_t>(i)] =
          (static_cast<std::uint32_t>(block[static_cast<std::size_t>(i) * 4U]) << 24U) |
          (static_cast<std::uint32_t>(block[static_cast<std::size_t>(i) * 4U + 1U]) << 16U) |
          (static_cast<std::uint32_t>(block[static_cast<std::size_t>(i) * 4U + 2U]) << 8U) |
          static_cast<std::uint32_t>(block[static_cast<std::size_t>(i) * 4U + 3U]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotateRight(words[i - 15U], 7U) ^ rotateRight(words[i - 15U], 18U) ^
                               (words[i - 15U] >> 3U);
      const std::uint32_t s1 = rotateRight(words[i - 2U], 17U) ^ rotateRight(words[i - 2U], 19U) ^
                               (words[i - 2U] >> 10U);
      words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + ch + kConstants[i] + words[i];
      const std::uint32_t s0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t bufferLen_{0};
  std::uint64_t totalBytes_{0};
};

inline std::string sha256Hex(std::string_view bytes) {
  Sha256 hasher;
  hasher.update(bytes.data(), bytes.size());
  return hasher.hexDigest();
}

}  // namespace seriona::audio::equivalence
