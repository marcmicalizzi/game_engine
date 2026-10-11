#pragma once

// SHA-1 (FIPS 180-4), for the one place a protocol names it: the WebSocket opening handshake,
// whose `Sec-WebSocket-Accept` is base64(SHA-1(key + a fixed GUID)) (RFC 6455 §4.2.2;
// foundation/net). **Not for security.** SHA-1 has had practical collisions since 2017; nothing
// here may use it to decide whether bytes can be trusted, and content addressing has its own hash.
// It lives beside the other hashes rather than inside the network module because it is a pure
// function of bytes, with standard test vectors (hash_tests.cpp), and a module that needed it for
// something else would otherwise grow a second copy.
//
// Header-only like the rest of this module: about sixty lines, called once per connection.

#include <core/base/types.h>

#include <array>
#include <cstring>
#include <span>
#include <string_view>

namespace engine {

using Sha1Digest = std::array<u8, 20>;

class Sha1 {
 public:
  Sha1() noexcept { reset(); }

  void reset() noexcept {
    state_[0] = 0x67452301u;
    state_[1] = 0xEFCDAB89u;
    state_[2] = 0x98BADCFEu;
    state_[3] = 0x10325476u;
    state_[4] = 0xC3D2E1F0u;
    length_ = 0;
    used_ = 0;
  }

  void update(std::span<const u8> bytes) noexcept {
    for (const u8 b : bytes) {
      block_[used_++] = b;
      if (used_ == 64) {
        compress();
        used_ = 0;
      }
    }
    length_ += static_cast<u64>(bytes.size());
  }
  void update(std::string_view text) noexcept {
    update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
  }

  // The digest; the object is reset afterwards, ready for another message.
  Sha1Digest finish() noexcept {
    const u64 bits = length_ * 8u;
    block_[used_++] = 0x80u;
    if (used_ > 56) {
      while (used_ < 64)
        block_[used_++] = 0;
      compress();
      used_ = 0;
    }
    while (used_ < 56)
      block_[used_++] = 0;
    for (u32 i = 0; i < 8; ++i)
      block_[56 + i] = static_cast<u8>(bits >> (56 - 8 * i));
    compress();
    Sha1Digest out{};
    for (u32 i = 0; i < 5; ++i) {
      out[i * 4 + 0] = static_cast<u8>(state_[i] >> 24);
      out[i * 4 + 1] = static_cast<u8>(state_[i] >> 16);
      out[i * 4 + 2] = static_cast<u8>(state_[i] >> 8);
      out[i * 4 + 3] = static_cast<u8>(state_[i]);
    }
    reset();
    return out;
  }

 private:
  static constexpr u32 rotl(u32 x, u32 n) noexcept { return (x << n) | (x >> (32u - n)); }

  void compress() noexcept {
    u32 w[80];
    for (u32 i = 0; i < 16; ++i) {
      w[i] = (static_cast<u32>(block_[i * 4]) << 24) | (static_cast<u32>(block_[i * 4 + 1]) << 16) |
             (static_cast<u32>(block_[i * 4 + 2]) << 8) | static_cast<u32>(block_[i * 4 + 3]);
    }
    for (u32 i = 16; i < 80; ++i)
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (u32 i = 0; i < 80; ++i) {
      u32 f = 0;
      u32 k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const u32 t = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  u32 state_[5];
  u8 block_[64];
  u64 length_;
  u32 used_;
};

inline Sha1Digest sha1(std::string_view text) noexcept {
  Sha1 h;
  h.update(text);
  return h.finish();
}

}  // namespace engine
