#pragma once

// SHA-256 (FIPS 180-4) for the shader cache's keys (shader_cache.h). The tool links no engine
// library (a host tool, ADR-0034), so it carries its own; the cache's self-test checks it against
// the standard's test vectors before anything else (cmake/CheckShaderCache.cmake).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shader_meter {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t size);
  void update(std::string_view text) { update(text.data(), text.size()); }
  // The digest as 64 lowercase hex digits. The object is spent afterwards.
  std::string hex();

 private:
  void block(const std::uint8_t* p);
  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_;
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;
};

inline std::string sha256_hex(std::string_view text) {
  Sha256 h;
  h.update(text);
  return h.hex();
}

}  // namespace shader_meter
