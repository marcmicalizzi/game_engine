// SHA-256 against FIPS 180-4's examples and against Python's hashlib on the interchange's own
// spellings (docs/subsystems/tissue.md): the hash is a contract between two tools, so it is checked
// against the other tool's answers, computed once with `hashlib` and written down here.
#include <domain/tissue/sha256.h>
#include <domain/tissue/tissue_file.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <string_view>

using namespace engine;
using namespace engine::tissue;

namespace {

std::string hex_of(std::string_view text) {
  return sha256_hex(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

}  // namespace

TEST_CASE("sha256: the FIPS 180-4 examples") {
  CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  CHECK(hex_of(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: fed in pieces of any size, the same digest") {
  std::string text;
  for (u32 i = 0; i < 1000; ++i)
    text.push_back(static_cast<char>('a' + i % 26));
  const std::string whole = hex_of(text);
  for (const u32 piece : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 127u, 999u}) {
    Sha256 hasher;
    for (u32 at = 0; at < text.size(); at += piece) {
      const u32 n = std::min<u32>(piece, static_cast<u32>(text.size()) - at);
      hasher.update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()) + at, n));
    }
    CHECK(to_hex(hasher.finish()) == whole);
  }
}

TEST_CASE("sha256: the interchange's spellings match hashlib") {
  // hashlib.sha256(np.array([[0, 1, 2], [0, 2, 3]], dtype='<i8').tobytes()).hexdigest(): the
  // topology hash widens indices to little-endian u64, which is what NumPy's default integer is.
  const u32 triangles[] = {0, 1, 2, 0, 2, 3};
  CHECK(topology_sha256(triangles) ==
        "588155b6e6708a8cfa760da51896fc20cc2ef0f14ceb2b8e037dac125f2a3f32");
  const u32 quads[] = {0, 1, 2, 3, 4, 5, 6, 70000};
  CHECK(topology_sha256(quads) ==
        "001d1141ffcb54e5e7257fe19046d63ca248a4428c77821e40e443af3d896d84");
  // A block: hashlib.sha256(np.array([[1.5, -2.25, 0.1]], dtype='<f4').tobytes()).hexdigest().
  const f32 values[] = {1.5f, -2.25f, 0.1f};
  CHECK(sha256_hex(std::span<const u8>(reinterpret_cast<const u8*>(values), sizeof(values))) ==
        "0d5b9085b0d58831633e7d37428dfc4e3608a83816f081c0af399c498a701aec");
}
