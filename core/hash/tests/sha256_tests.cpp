// SHA-256 against FIPS 180's vectors (the one-block "abc", the two-block 448-bit message, a million
// a's) and the empty message, fed whole and in uneven pieces, so the block-at-a-time path and the
// partial-block copy both reach every padding case: what a document's blob names rest on
// (domain/doc/blob_store.h).
#include <core/hash/sha256.h>

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace engine;

TEST_CASE("sha256: the standard vectors") {
  CHECK(sha256_hex(sha256("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex(sha256("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256_hex(sha256("The quick brown fox jumps over the lazy dog")) ==
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST_CASE("sha256: a million a's, whole and in uneven pieces") {
  const std::string million(1000000, 'a');
  const std::string expected = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
  CHECK(sha256_hex(sha256(million)) == expected);
  for (const usize piece : {usize{1}, usize{7}, usize{63}, usize{64}, usize{65}, usize{997}}) {
    Sha256 h;
    usize fed = 0;
    while (fed < million.size()) {
      const usize n = piece < million.size() - fed ? piece : million.size() - fed;
      h.update(std::string_view(million).substr(fed, n));
      fed += n;
    }
    CHECK(sha256_hex(h.finish()) == expected);
  }
}

TEST_CASE("sha256: every length across the padding's block edge agrees fed whole and byte by byte") {
  std::string message;
  for (u32 length = 0; length < 200; ++length) {
    Sha256 bytewise;
    for (const char c : message)
      bytewise.update(std::string_view(&c, 1));
    CHECK(bytewise.finish() == sha256(message));
    message.push_back(static_cast<char>('a' + length % 26));
  }
  // The object resets after finish(): the same message hashes the same twice.
  Sha256 h;
  h.update("abc");
  const Sha256Digest first = h.finish();
  h.update("abc");
  CHECK(h.finish() == first);
}
