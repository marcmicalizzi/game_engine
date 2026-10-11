# hash (core)

**Purpose.** Hashing primitives for container keys and small-key identity: a 64-bit finalizer (`mix64`), `hash_combine`, `hash_bytes`, and `Hash<T>` specializations; and **SHA-1 and base64** (`sha1.h`, `base64.h`) for the one protocol that names them, the WebSocket opening handshake, and for engine-host's session token ([net](net.md)). Header-only.

**Owned data.** None.

**Invariants.**
- Every `Hash<T>` returns a well-mixed 64-bit value; containers take bucket bits from the top and fingerprint bits from the bottom and rely on both being good.
- `Hash<std::string>`, `Hash<std::string_view>`, and `Hash<const char*>` agree on equal text and are transparent, which is what makes heterogeneous lookup on string keys work.
- `Hash<float>` and `Hash<double>` hash `-0.0` and `+0.0` identically because they compare equal.
- There is no fallback for unspecialized types: using `Hash<T>` for a type without a specialization is a compile error, not a weak hash.

**Public API.** `core/hash/hash.h`: `k_hash_seed`, `mix64`, `hash_combine`, `hash_bytes(data, len, seed)`, `Hash<T>` for integers, enums, pointers, floats, string-like types, and pairs; `StringHash`. `core/hash/sha1.h`: `Sha1` (`update`, `finish`) and `sha1(text)`, a `Sha1Digest` of 20 bytes. `core/hash/base64.h`: `base64_encode(bytes, alphabet, pad)` with the standard or URL alphabet, `base64_decode` that takes either.

**SHA-1 is not for security.** It has had practical collisions since 2017. It is here because RFC 6455 computes `Sec-WebSocket-Accept` as base64(SHA-1(key + GUID)), and a pure function of bytes with standard test vectors belongs beside the other hashes rather than inside the network module, where a second user would otherwise grow a second copy. Nothing may use it to decide whether bytes can be trusted.

**Depends on.** `base`.

**Testing.** `tools/dev.ps1 test -Filter hash`. Covers determinism, length and content sensitivity across the word boundary, representation independence for strings, float zero handling, and top-bit distribution for sequential integers. `sha1_base64_tests.cpp` holds SHA-1 to the FIPS 180 and RFC 3174 vectors (the million a's fed in uneven pieces, and padding at 55, 56 and 64 bytes, checked against .NET's SHA1), base64 to RFC 4648 §10's vectors both ways and the URL alphabet, and the two together to RFC 6455 §1.3's worked handshake.

**Performance notes.** `hash_bytes` is a word-at-a-time multiplicative hash with the MurmurHash3 finalizer: adequate for hash tables, deterministic across platforms, not cryptographic, and not the content-addressing hash (BLAKE3 or xxHash3 arrive with the derived-data cache). It is a candidate for replacement by a benchmarked fast hash once the tunables harness exists.
