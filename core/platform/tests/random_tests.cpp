// The system's secure random source answers, fills every byte it is asked for, and does not
// answer the same twice. Nothing statistical: a test of a CSPRNG's quality is the OS vendor's;
// this one catches a wrapper that returns early, fills a prefix, or hands back zeros.
#include <core/platform/random.h>

#include <doctest/doctest.h>

#include <array>
#include <cstring>

using namespace engine;

TEST_CASE("secure_random: fills the whole buffer, differently each time") {
  std::array<u8, 64> a{};
  std::array<u8, 64> b{};
  REQUIRE(platform::secure_random(a));
  REQUIRE(platform::secure_random(b));
  CHECK(std::memcmp(a.data(), b.data(), a.size()) != 0);
  // A 16-byte tail of zeros from a correct source has odds of 2^-128.
  bool tail_written = false;
  for (usize i = 48; i < 64; ++i)
    tail_written = tail_written || a[i] != 0;
  CHECK(tail_written);
  CHECK(platform::secure_random(std::span<u8>()));  // nothing asked, nothing to fail
}
