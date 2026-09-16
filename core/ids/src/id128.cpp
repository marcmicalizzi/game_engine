#include <core/ids/id128.h>

#include <atomic>
#include <chrono>
#include <random>

namespace engine {

namespace {

constexpr char k_hex_digits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void write_hex64(u64 v, char* out) noexcept {
  for (int i = 15; i >= 0; --i) {
    out[i] = k_hex_digits[v & 0xF];
    v >>= 4;
  }
}

// Per-thread xoshiro256** seeded from std::random_device, the clock, and the thread's
// address so threads never share a sequence.
struct Rng {
  u64 s[4];

  Rng() noexcept {
    std::random_device rd;
    u64 seed = (static_cast<u64>(rd()) << 32) ^ rd();
    seed ^= static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= mix64(reinterpret_cast<usize>(this));
    for (u64& word : s) {
      seed = mix64(seed + 0x9E3779B97F4A7C15ull);
      word = seed;
    }
  }

  static u64 rotl(u64 x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

  u64 next() noexcept {
    const u64 result = rotl(s[1] * 5, 7) * 9;
    const u64 t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
  }
};

thread_local Rng t_rng;

}  // namespace

void Id128::to_hex(char* out) const noexcept {
  write_hex64(hi, out);
  write_hex64(lo, out + 16);
  out[32] = '\0';
}

bool Id128::from_hex(std::string_view text, Id128& out) noexcept {
  if (text.size() != k_hex_length) return false;
  u64 words[2] = {0, 0};
  for (usize i = 0; i < k_hex_length; ++i) {
    const int v = hex_value(text[i]);
    if (v < 0) return false;
    words[i / 16] = (words[i / 16] << 4) | static_cast<u64>(v);
  }
  out.hi = words[0];
  out.lo = words[1];
  return true;
}

Id128 Id128::generate() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const u64 ms =
      static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  Id128 id;
  do {
    const u64 r0 = t_rng.next();
    const u64 r1 = t_rng.next();
    id.hi = (ms << 16) | (r0 & 0xFFFFull);
    id.lo = r1;
  } while (id.is_null());
  return id;
}

Id128 Id128::from_seed(u64 seed, u64 counter) noexcept {
  Id128 id;
  do {
    const u64 a = mix64(seed ^ 0xA5A5A5A5A5A5A5A5ull);
    const u64 b = hash_combine(a, counter);
    const u64 c = hash_combine(b, 0x1234567887654321ull);
    // Synthetic timestamp: derived from the seed so a stream sorts together, never from time.
    id.hi = ((a >> 16) << 16) | (b & 0xFFFFull);
    id.lo = c;
    ++counter;
  } while (id.is_null());
  return id;
}

}  // namespace engine
