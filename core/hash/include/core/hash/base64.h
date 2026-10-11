#pragma once

// Base64 (RFC 4648 §4) and its URL-safe alphabet (§5): the WebSocket handshake's key and accept
// values are standard base64 with padding, and engine-host's session token is base64url without
// padding, so that it can stand in a URL's query unescaped (docs/subsystems/net.md). Header-only,
// beside SHA-1 for the same reason: a pure function of bytes with the RFC's test vectors.

#include <core/base/types.h>

#include <span>
#include <string>
#include <string_view>

namespace engine {

enum class Base64Alphabet : u8 { Standard, Url };

inline std::string base64_encode(std::span<const u8> bytes,
                                 Base64Alphabet alphabet = Base64Alphabet::Standard,
                                 bool pad = true) {
  const char* table = alphabet == Base64Alphabet::Standard
                          ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
                          : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  usize i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const u32 v = (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8) |
                  static_cast<u32>(bytes[i + 2]);
    out.push_back(table[(v >> 18) & 63u]);
    out.push_back(table[(v >> 12) & 63u]);
    out.push_back(table[(v >> 6) & 63u]);
    out.push_back(table[v & 63u]);
  }
  const usize rest = bytes.size() - i;
  if (rest == 1) {
    const u32 v = static_cast<u32>(bytes[i]) << 16;
    out.push_back(table[(v >> 18) & 63u]);
    out.push_back(table[(v >> 12) & 63u]);
    if (pad) out += "==";
  } else if (rest == 2) {
    const u32 v = (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8);
    out.push_back(table[(v >> 18) & 63u]);
    out.push_back(table[(v >> 12) & 63u]);
    out.push_back(table[(v >> 6) & 63u]);
    if (pad) out.push_back('=');
  }
  return out;
}

inline std::string base64_encode(std::string_view text,
                                 Base64Alphabet alphabet = Base64Alphabet::Standard,
                                 bool pad = true) {
  return base64_encode(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()),
                       alphabet, pad);
}

// Either alphabet, padding optional; false for any other character, a length no encoding has, or
// padding anywhere but the end. `out` is replaced.
inline bool base64_decode(std::string_view text, std::string& out) {
  out.clear();
  while (!text.empty() && text.back() == '=')
    text.remove_suffix(1);
  if (text.size() % 4 == 1) return false;
  u32 acc = 0;
  u32 bits = 0;
  for (const char c : text) {
    u32 v = 0;
    if (c >= 'A' && c <= 'Z') {
      v = static_cast<u32>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      v = static_cast<u32>(c - 'a') + 26u;
    } else if (c >= '0' && c <= '9') {
      v = static_cast<u32>(c - '0') + 52u;
    } else if (c == '+' || c == '-') {
      v = 62;
    } else if (c == '/' || c == '_') {
      v = 63;
    } else {
      return false;
    }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xffu));
    }
  }
  return true;
}

}  // namespace engine
