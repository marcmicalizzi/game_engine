#include <domain/tissue/sha256.h>

#include <cstring>

namespace engine::tissue {

namespace {

constexpr u32 k_round[64] = {
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

constexpr u32 rotr(u32 x, u32 n) noexcept { return (x >> n) | (x << (32u - n)); }

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
      buffer_{} {}

void Sha256::block(const u8* data) noexcept {
  u32 w[64];
  for (u32 i = 0; i < 16; ++i)
    w[i] = (u32{data[4 * i]} << 24) | (u32{data[4 * i + 1]} << 16) | (u32{data[4 * i + 2]} << 8) |
           u32{data[4 * i + 3]};
  for (u32 i = 16; i < 64; ++i) {
    const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = state_[0];
  u32 b = state_[1];
  u32 c = state_[2];
  u32 d = state_[3];
  u32 e = state_[4];
  u32 f = state_[5];
  u32 g = state_[6];
  u32 h = state_[7];
  for (u32 i = 0; i < 64; ++i) {
    const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const u32 choose = (e & f) ^ (~e & g);
    const u32 t1 = h + s1 + choose + k_round[i] + w[i];
    const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const u32 majority = (a & b) ^ (a & c) ^ (b & c);
    const u32 t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
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

void Sha256::update(std::span<const u8> bytes) noexcept {
  const u8* data = bytes.data();
  usize size = bytes.size();
  length_ += size;
  if (buffered_ > 0) {
    const usize take = size < 64u - buffered_ ? size : 64u - buffered_;
    std::memcpy(buffer_ + buffered_, data, take);
    buffered_ += static_cast<u32>(take);
    data += take;
    size -= take;
    if (buffered_ < 64) return;
    block(buffer_);
    buffered_ = 0;
  }
  while (size >= 64) {
    block(data);
    data += 64;
    size -= 64;
  }
  if (size > 0) {
    std::memcpy(buffer_, data, size);
    buffered_ = static_cast<u32>(size);
  }
}

Sha256Digest Sha256::finish() noexcept {
  const u64 bits = length_ * 8u;
  const u8 one = 0x80;
  update(std::span<const u8>(&one, 1));
  const u8 zero = 0;
  while (buffered_ != 56)
    update(std::span<const u8>(&zero, 1));
  u8 tail[8];
  for (u32 i = 0; i < 8; ++i)
    tail[i] = static_cast<u8>(bits >> (56u - 8u * i));
  update(std::span<const u8>(tail, 8));
  Sha256Digest out{};
  for (u32 i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<u8>(state_[i] >> 24);
    out[4 * i + 1] = static_cast<u8>(state_[i] >> 16);
    out[4 * i + 2] = static_cast<u8>(state_[i] >> 8);
    out[4 * i + 3] = static_cast<u8>(state_[i]);
  }
  return out;
}

Sha256Digest sha256(std::span<const u8> bytes) noexcept {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

std::string to_hex(const Sha256Digest& digest) {
  static constexpr char k_digits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (u32 i = 0; i < 32; ++i) {
    out[2 * i] = k_digits[digest[i] >> 4];
    out[2 * i + 1] = k_digits[digest[i] & 0xfu];
  }
  return out;
}

std::string sha256_hex(std::span<const u8> bytes) { return to_hex(sha256(bytes)); }

}  // namespace engine::tissue
