#include "chtholly/Support/Digest.h"

#include "chtholly/Support/FileSystem.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace chtholly {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotateRight(std::uint32_t value,
                                    std::uint32_t amount) {
  return (value >> amount) | (value << (32u - amount));
}

class Sha256 {
public:
  void update(const unsigned char *data, std::size_t size) {
    total_bytes_ += size;
    while (size != 0) {
      const auto available = block_.size() - block_size_;
      const auto count = size < available ? size : available;
      for (std::size_t index = 0; index < count; ++index) {
        block_[block_size_ + index] = data[index];
      }
      block_size_ += count;
      data += count;
      size -= count;
      if (block_size_ == block_.size()) {
        transform(block_);
        block_size_ = 0;
      }
    }
  }

  std::string finish() {
    const auto bit_length = static_cast<std::uint64_t>(total_bytes_) * 8u;
    block_[block_size_++] = 0x80u;
    if (block_size_ > 56) {
      while (block_size_ < block_.size()) {
        block_[block_size_++] = 0;
      }
      transform(block_);
      block_size_ = 0;
    }
    while (block_size_ < 56) {
      block_[block_size_++] = 0;
    }
    for (std::size_t index = 0; index < 8; ++index) {
      block_[63 - index] =
          static_cast<unsigned char>(bit_length >> (index * 8));
    }
    transform(block_);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto value : state_) {
      out << std::setw(8) << value;
    }
    return out.str();
  }

private:
  void transform(const std::array<unsigned char, 64> &block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto offset = index * 4;
      words[index] = (static_cast<std::uint32_t>(block[offset]) << 24u) |
                     (static_cast<std::uint32_t>(block[offset + 1]) << 16u) |
                     (static_cast<std::uint32_t>(block[offset + 2]) << 8u) |
                     static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = rotateRight(words[index - 15], 7) ^
                      rotateRight(words[index - 15], 18) ^
                      (words[index - 15] >> 3u);
      const auto s1 = rotateRight(words[index - 2], 17) ^
                      rotateRight(words[index - 2], 19) ^
                      (words[index - 2] >> 10u);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                        rotateRight(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto temp1 =
          h + sum1 + choice + kRoundConstants[index] + words[index];
      const auto sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                        rotateRight(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = sum0 + majority;
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

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::array<unsigned char, 64> block_{};
  std::size_t block_size_ = 0;
  std::size_t total_bytes_ = 0;
};

} // namespace

std::string sha256Hex(std::string_view text) {
  Sha256 digest;
  digest.update(reinterpret_cast<const unsigned char *>(text.data()),
                text.size());
  return digest.finish();
}

std::optional<std::string> sha256File(const std::string &path) {
  std::ifstream input(pathForFileSystem(path), std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  Sha256 digest;
  std::array<char, 8192> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      digest.update(reinterpret_cast<const unsigned char *>(buffer.data()),
                    static_cast<std::size_t>(count));
    }
  }
  if (!input.eof()) {
    return std::nullopt;
  }
  return digest.finish();
}

} // namespace chtholly
