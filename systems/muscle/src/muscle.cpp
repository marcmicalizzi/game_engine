#include <systems/muscle/muscle.h>

namespace engine::muscle {

bool parse_sha256(std::string_view hex, Sha256& out) {
  if (hex.size() != 64) return false;
  for (usize i = 0; i < 32; ++i) {
    const u8 hi = hex_nibble(hex[2 * i]);
    const u8 lo = hex_nibble(hex[2 * i + 1]);
    if (hi > 15 || lo > 15) return false;
    out[i] = static_cast<u8>((hi << 4) | lo);
  }
  return true;
}

std::string to_hex(const Sha256& value) {
  constexpr char k_digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const u8 b : value) {
    out.push_back(k_digits[b >> 4]);
    out.push_back(k_digits[b & 15]);
  }
  return out;
}

}  // namespace engine::muscle
