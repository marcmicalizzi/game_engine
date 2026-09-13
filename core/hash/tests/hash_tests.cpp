#include <core/hash/hash.h>

#include <doctest/doctest.h>

#include <bit>
#include <string>
#include <string_view>
#include <vector>

using namespace engine;

TEST_CASE("hash: mix64 is a bijection with no fixed point at zero") {
  static_assert(mix64(0) == 0);  // murmur3 finalizer maps 0 to 0; containers never rely on it
  CHECK(mix64(1) != mix64(2));
  CHECK(mix64(1) != 1);
  // Adjacent inputs differ in many bits.
  int total_bits = 0;
  for (u64 i = 1; i < 1000; ++i) total_bits += std::popcount(mix64(i) ^ mix64(i + 1));
  const double average = static_cast<double>(total_bits) / 999.0;
  CHECK(average > 24.0);
  CHECK(average < 40.0);
}

TEST_CASE("hash: hash_bytes is deterministic and sensitive to content and length") {
  const std::string a = "hello world";
  const std::string b = "hello worle";
  CHECK(hash_bytes(a.data(), a.size()) == hash_bytes(a.data(), a.size()));
  CHECK(hash_bytes(a.data(), a.size()) != hash_bytes(b.data(), b.size()));
  CHECK(hash_bytes("ab", 2) != hash_bytes("ab\0", 3));
  CHECK(hash_bytes("", 0) != hash_bytes("\0", 1));
  CHECK(hash_bytes(a.data(), a.size(), 1) != hash_bytes(a.data(), a.size(), 2));

  // Lengths across the 8-byte word boundary all hash and differ.
  std::vector<u64> seen;
  std::string s;
  for (int len = 0; len < 40; ++len) {
    seen.push_back(hash_bytes(s.data(), s.size()));
    s.push_back(static_cast<char>('a' + (len % 26)));
  }
  for (usize i = 0; i < seen.size(); ++i)
    for (usize j = i + 1; j < seen.size(); ++j) CHECK(seen[i] != seen[j]);
}

TEST_CASE("hash: string-like keys hash identically regardless of representation") {
  const std::string s = "asset/rock_03";
  const std::string_view sv = s;
  const char* cs = s.c_str();
  CHECK(Hash<std::string>{}(s) == Hash<std::string_view>{}(sv));
  CHECK(Hash<std::string>{}(s) == Hash<const char*>{}(cs));
  static_assert(requires { typename Hash<std::string>::is_transparent; });
}

TEST_CASE("hash: integers, enums, pointers, floats") {
  enum class Kind : u8 { A = 1, B = 2 };
  CHECK(Hash<int>{}(5) == mix64(5));
  CHECK(Hash<Kind>{}(Kind::A) != Hash<Kind>{}(Kind::B));
  int x = 0;
  int y = 0;
  CHECK(Hash<int*>{}(&x) != Hash<int*>{}(&y));
  CHECK(Hash<float>{}(-0.0f) == Hash<float>{}(0.0f));
  CHECK(Hash<double>{}(-0.0) == Hash<double>{}(0.0));
  CHECK(Hash<float>{}(1.0f) != Hash<float>{}(2.0f));
  CHECK(Hash<std::pair<int, int>>{}({1, 2}) != Hash<std::pair<int, int>>{}({2, 1}));
}

TEST_CASE("hash: sequential integer keys spread across buckets") {
  // Top bits are what HashMap uses for bucket selection; check they are not clustered.
  constexpr int k_buckets = 256;
  int counts[k_buckets] = {};
  constexpr int k_n = 65536;
  for (int i = 0; i < k_n; ++i) ++counts[Hash<int>{}(i) >> 56];
  const int expected = k_n / k_buckets;
  for (int c : counts) {
    CHECK(c > expected / 2);
    CHECK(c < expected * 2);
  }
}
