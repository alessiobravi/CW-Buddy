#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace cwassistant::test {

inline std::uint32_t rotateRight(const std::uint32_t value,
                                 const unsigned shift) noexcept {
  return (value >> shift) | (value << (32U - shift));
}

// Incremental SHA-256. Incremental rather than whole-file because a recording
// is not always one file: a SigMF capture is a `.sigmf-data` payload plus the
// `.sigmf-meta` sidecar that declares its sample rate and centre frequency,
// and a digest of the payload alone would let the sidecar -- which decides
// what the decoder window means, and therefore the score -- be edited without
// the guard noticing.
class Sha256 {
 public:
  void update(const std::uint8_t* data, std::size_t size) noexcept {
    total_bytes_ += size;
    for (std::size_t index = 0; index < size; ++index) {
      buffer_[buffered_++] = data[index];
      if (buffered_ == buffer_.size()) {
        transform();
        buffered_ = 0;
      }
    }
  }

  [[nodiscard]] std::string finish() noexcept {
    const std::uint64_t bits = total_bytes_ * 8ULL;
    constexpr std::uint8_t kTerminator = 0x80U;
    constexpr std::uint8_t kZero = 0x00U;
    update(&kTerminator, 1U);
    while (buffered_ != 56U) update(&kZero, 1U);
    std::array<std::uint8_t, 8> length{};
    for (std::size_t index = 0; index < length.size(); ++index)
      length[7U - index] = static_cast<std::uint8_t>(bits >> (index * 8U));
    update(length.data(), length.size());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint32_t word : state_) output << std::setw(8) << word;
    return output.str();
  }

 private:
  void transform() noexcept {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      words[index] = (static_cast<std::uint32_t>(buffer_[index * 4U]) << 24U) |
          (static_cast<std::uint32_t>(buffer_[index * 4U + 1U]) << 16U) |
          (static_cast<std::uint32_t>(buffer_[index * 4U + 2U]) << 8U) |
          static_cast<std::uint32_t>(buffer_[index * 4U + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const std::uint32_t s0 = rotateRight(words[index - 15U], 7U) ^
          rotateRight(words[index - 15U], 18U) ^
          (words[index - 15U] >> 3U);
      const std::uint32_t s1 = rotateRight(words[index - 2U], 17U) ^
          rotateRight(words[index - 2U], 19U) ^
          (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^
                                 rotateRight(e, 25U);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 = h + sum1 + choose + constants[index] +
                                       words[index];
      const std::uint32_t sum0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^
                                 rotateRight(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g; g = f; f = e; e = d + temporary1;
      d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
};

// One digest over several files, hashed end to end in the order given. Empty
// when any of them cannot be opened, so a missing file is never mistaken for a
// digest that simply does not match.
inline std::string filesSha256(
    const std::initializer_list<std::string> paths) {
  Sha256 digest;
  std::vector<char> chunk(64U * 1024U);
  for (const auto& path : paths) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    while (input.read(chunk.data(),
                      static_cast<std::streamsize>(chunk.size())) ||
           input.gcount() > 0) {
      digest.update(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                    static_cast<std::size_t>(input.gcount()));
      if (!input) break;
    }
  }
  return digest.finish();
}

inline std::string fileSha256(const std::string& path) {
  return filesSha256({path});
}

}  // namespace cwassistant::test
