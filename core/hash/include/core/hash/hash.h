#pragma once

// Hashing primitives for containers and content addressing of small keys.
//
// Every Hash<T> returns a well-mixed 64-bit value: containers take bits from either end
// (bucket index from the top, fingerprint from the bottom) and rely on that. Integers go
// through a full 64-bit finalizer rather than the identity mapping some standard libraries use.
//
// hash_bytes is a word-at-a-time multiplicative hash with a strong finalizer. It is
// deterministic across platforms and adequate for hash tables; it is not a cryptographic hash
// and not the content-addressing hash (BLAKE3 or xxHash3 arrive with the derived-data cache).
// It is a candidate for replacement by a benchmarked fast hash once the tunables harness exists.

#include <core/base/types.h>

#include <bit>
#include <concepts>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace engine {

inline constexpr u64 k_hash_seed = 0x9E3779B97F4A7C15ull;

// MurmurHash3 64-bit finalizer: a bijection with full avalanche.
constexpr u64 mix64(u64 x) noexcept {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

constexpr u64 hash_combine(u64 seed, u64 value) noexcept {
  return mix64(seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2)));
}

namespace hash_detail {

inline constexpr u64 k_word_multiplier = 0x9FB21C651E98DF25ull;

constexpr u64 mix_word(u64 h, u64 word) noexcept {
  h ^= word;
  h *= k_word_multiplier;
  h ^= h >> 32;
  return h;
}

inline u64 load_partial(const unsigned char* p, usize len) noexcept {
  u64 word = 0;
  std::memcpy(&word, p, len);  // little-endian on every supported platform
  return word;
}

}  // namespace hash_detail

inline u64 hash_bytes(const void* data, usize len, u64 seed = k_hash_seed) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  u64 h = seed ^ mix64(len);
  while (len >= 8) {
    u64 word;
    std::memcpy(&word, p, 8);
    h = hash_detail::mix_word(h, word);
    p += 8;
    len -= 8;
  }
  if (len > 0) {
    // Fold the tail length into the word so "ab" and "ab\0" differ.
    h = hash_detail::mix_word(h, hash_detail::load_partial(p, len) ^ (static_cast<u64>(len) << 56));
  }
  return mix64(h);
}

// Primary template is intentionally undefined: a missing specialization is a compile error
// with a clear name rather than a silent fallback to a weak hash.
template <class T>
struct Hash;

template <class T>
  requires std::integral<T>
struct Hash<T> {
  u64 operator()(T value) const noexcept { return mix64(static_cast<u64>(value)); }
};

template <class T>
  requires std::is_enum_v<T>
struct Hash<T> {
  u64 operator()(T value) const noexcept {
    return mix64(static_cast<u64>(static_cast<std::underlying_type_t<T>>(value)));
  }
};

template <class T>
struct Hash<T*> {
  u64 operator()(T* p) const noexcept {
    return mix64(static_cast<u64>(reinterpret_cast<usize>(p)));
  }
};

template <>
struct Hash<float> {
  u64 operator()(float value) const noexcept {
    if (value == 0.0f) value = 0.0f;  // -0.0 and +0.0 compare equal and must hash equal
    return mix64(std::bit_cast<u32>(value));
  }
};

template <>
struct Hash<double> {
  u64 operator()(double value) const noexcept {
    if (value == 0.0) value = 0.0;
    return mix64(std::bit_cast<u64>(value));
  }
};

// One hasher for every string-like key so that HashMap<std::string, V> accepts
// std::string_view and const char* lookups without constructing a std::string.
struct StringHash {
  using is_transparent = void;
  u64 operator()(std::string_view s) const noexcept { return hash_bytes(s.data(), s.size()); }
};

template <>
struct Hash<std::string> : StringHash {};
template <>
struct Hash<std::string_view> : StringHash {};
template <>
struct Hash<const char*> : StringHash {};
template <>
struct Hash<char*> : StringHash {};

template <class A, class B>
struct Hash<std::pair<A, B>> {
  u64 operator()(const std::pair<A, B>& p) const noexcept {
    return hash_combine(Hash<A>{}(p.first), Hash<B>{}(p.second));
  }
};

}  // namespace engine
