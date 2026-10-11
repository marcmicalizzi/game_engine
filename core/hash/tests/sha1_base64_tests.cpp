// SHA-1 against FIPS 180's and RFC 3174's vectors, base64 against RFC 4648 §10's, and the two
// together against RFC 6455 §1.3's worked handshake — the one value foundation/net computes with
// them, so a mistake in either shows here before it shows as a browser refusing a connection.
#include <core/hash/base64.h>
#include <core/hash/sha1.h>

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace engine;

namespace {

std::string hex(const Sha1Digest& d) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (const u8 b : d) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}

}  // namespace

TEST_CASE("sha1: the standard vectors") {
  CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(hex(sha1("The quick brown fox jumps over the lazy dog")) ==
        "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST_CASE("sha1: a million a's, fed in uneven pieces, and padding across a block edge") {
  Sha1 h;
  const std::string chunk(997, 'a');
  usize fed = 0;
  while (fed + chunk.size() <= 1000000) {
    h.update(chunk);
    fed += chunk.size();
  }
  h.update(std::string(1000000 - fed, 'a'));
  CHECK(hex(h.finish()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  // 55, 56 and 64 bytes: the length lands in the first block, needs a second, or fills one.
  CHECK(hex(sha1(std::string(55, 'a'))) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
  CHECK(hex(sha1(std::string(56, 'a'))) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
  CHECK(hex(sha1(std::string(64, 'a'))) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

TEST_CASE("base64: RFC 4648's vectors, both ways") {
  const std::string_view plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
  const std::string_view coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  for (usize i = 0; i < 7; ++i) {
    CHECK(base64_encode(plain[i]) == coded[i]);
    std::string back;
    REQUIRE(base64_decode(coded[i], back));
    CHECK(back == plain[i]);
  }
}

TEST_CASE("base64: the URL alphabet without padding, and what decoding refuses") {
  const u8 bytes[] = {0xfb, 0xff, 0xbf, 0x01};
  CHECK(base64_encode(std::span<const u8>(bytes, 4)) == "+/+/AQ==");
  CHECK(base64_encode(std::span<const u8>(bytes, 4), Base64Alphabet::Url, false) == "-_-_AQ");
  std::string back;
  REQUIRE(base64_decode("-_-_AQ", back));
  CHECK(back == std::string(reinterpret_cast<const char*>(bytes), 4));
  CHECK_FALSE(base64_decode("Zm9v!", back));
  CHECK_FALSE(base64_decode("Z", back));
}

TEST_CASE("sha1 and base64: RFC 6455's opening handshake") {
  // §1.3: the client's key, the protocol's GUID appended, SHA-1, base64.
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  const Sha1Digest d = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  CHECK(base64_encode(std::span<const u8>(d.data(), d.size())) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
