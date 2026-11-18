#pragma once

// SHA-256 (FIPS 180-4 §6.2), for content addressing: a document's blobs are named by the SHA-256 of
// their bytes (domain/doc/blob_store.h; docs/subsystems/doc.md, "Blobs"), so that the same bytes
// always get the same name and a file whose bytes do not hash to its name is known to be damaged.
// The 64-bit `hash_bytes` beside it is a table hash: two different heightfields could share a value,
// and a name shared by two contents would make a document read the wrong bytes without a word.
// SHA-256 is what git's newer object format and every content-addressed store of note uses, and has
// no known collision.
//
// Header-only like the rest of this module. Whole 64-byte blocks are compressed straight from the
// caller's bytes; only a partial block is copied, so hashing a 64 MiB grid costs the compression and
// not a byte-by-byte copy.

#include <core/base/types.h>

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace engine {

using Sha256Digest = std::array<u8, 32>;

class Sha256 {
 public:
  Sha256() noexcept { reset(); }

  void reset() noexcept {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    length_ = 0;
    used_ = 0;
  }

  void update(std::span<const u8> bytes) noexcept {
    const u8* p = bytes.data();
    usize n = bytes.size();
    length_ += static_cast<u64>(n);
    if (used_ > 0) {
      const usize take = n < 64 - used_ ? n : 64 - used_;
      std::memcpy(block_ + used_, p, take);
      used_ += static_cast<u32>(take);
      p += take;
      n -= take;
      if (used_ < 64) return;
      compress(block_);
      used_ = 0;
    }
    while (n >= 64) {
      compress(p);
      p += 64;
      n -= 64;
    }
    if (n > 0) {
      std::memcpy(block_, p, n);
      used_ = static_cast<u32>(n);
    }
  }
  void update(std::string_view text) noexcept {
    update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
  }

  // The digest; the object is reset afterwards, ready for another message.
  Sha256Digest finish() noexcept {
    const u64 bits = length_ * 8u;
    block_[used_++] = 0x80u;
    if (used_ > 56) {
      while (used_ < 64)
        block_[used_++] = 0;
      compress(block_);
      used_ = 0;
    }
    while (used_ < 56)
      block_[used_++] = 0;
    for (u32 i = 0; i < 8; ++i)
      block_[56 + i] = static_cast<u8>(bits >> (56 - 8 * i));
    compress(block_);
    Sha256Digest out{};
    for (u32 i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<u8>(state_[i] >> 24);
      out[i * 4 + 1] = static_cast<u8>(state_[i] >> 16);
      out[i * 4 + 2] = static_cast<u8>(state_[i] >> 8);
      out[i * 4 + 3] = static_cast<u8>(state_[i]);
    }
    reset();
    return out;
  }

 private:
  static constexpr u32 rotr(u32 x, u32 n) noexcept { return (x >> n) | (x << (32u - n)); }

  void compress(const u8* block) noexcept {
    static constexpr u32 k[64] = {
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
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) {
      w[i] = (static_cast<u32>(block[i * 4]) << 24) | (static_cast<u32>(block[i * 4 + 1]) << 16) |
             (static_cast<u32>(block[i * 4 + 2]) << 8) | static_cast<u32>(block[i * 4 + 3]);
    }
    for (u32 i = 16; i < 64; ++i) {
      const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    u32 e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (u32 i = 0; i < 64; ++i) {
      const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const u32 ch = (e & f) ^ (~e & g);
      const u32 t1 = h + s1 + ch + k[i] + w[i];
      const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const u32 maj = (a & b) ^ (a & c) ^ (b & c);
      const u32 t2 = s0 + maj;
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

  u32 state_[8];
  u8 block_[64];
  u64 length_;
  u32 used_;
};

inline Sha256Digest sha256(std::string_view bytes) noexcept {
  Sha256 h;
  h.update(bytes);
  return h.finish();
}

// The digest as 64 lowercase hexadecimal characters: the form a blob's name takes.
inline std::string sha256_hex(const Sha256Digest& digest) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const u8 b : digest) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}

}  // namespace engine
