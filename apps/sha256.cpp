#include "apps/sha256.h"

#include <array>
#include <cstring>

namespace daw {
namespace {

// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the
// first 64 primes. Written out rather than computed, because a generated table is one more thing
// that can be wrong in a way the output does not reveal.
constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr uint32_t rotr(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

void compress(std::array<uint32_t, 8>& state, const uint8_t block[64]) {
  uint32_t w[64];
  for (uint32_t i = 0; i < 16; ++i) {
    // BIG-ENDIAN, explicitly. Reading the block through a uint32_t* would take the host's byte
    // order and produce a digest that is correct only on one architecture.
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (uint32_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + S1 + ch + kRoundConstants[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;
    h = g; g = f; f = e;
    e = d + temp1;
    d = c; c = b; b = a;
    a = temp1 + temp2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

std::string sha256Hex(const void* data, size_t size) {
  // FIPS 180-4 section 5.3.3: the first 32 bits of the fractional parts of the square roots of the
  // first eight primes.
  std::array<uint32_t, 8> state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  const auto* bytes = static_cast<const uint8_t*>(data);

  size_t offset = 0;
  for (; offset + 64 <= size; offset += 64) {
    compress(state, bytes + offset);
  }

  // THE TAIL, THE 0x80 MARKER, AND THE LENGTH — and the two-block case is the one that gets
  // written wrong. When the remainder is 56..63 bytes the length does not fit after the marker, so
  // it takes a SECOND padded block. Getting that wrong produces correct digests for most inputs
  // and wrong ones for a narrow band of lengths, which is exactly the shape a round-trip test
  // cannot see.
  uint8_t tail[128] = {};
  const size_t remainder = size - offset;
  std::memcpy(tail, bytes + offset, remainder);
  tail[remainder] = 0x80;
  const size_t tailBlocks = (remainder >= 56) ? 2 : 1;
  const uint64_t bitLength = static_cast<uint64_t>(size) * 8u;
  const size_t lengthAt = tailBlocks * 64 - 8;
  for (int i = 0; i < 8; ++i) {
    tail[lengthAt + i] = static_cast<uint8_t>((bitLength >> (56 - 8 * i)) & 0xFFu);
  }
  for (size_t i = 0; i < tailBlocks; ++i) {
    compress(state, tail + i * 64);
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint32_t word : state) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      const uint8_t byte = static_cast<uint8_t>((word >> shift) & 0xFFu);
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0Fu]);
    }
  }
  return out;
}

}  // namespace daw
