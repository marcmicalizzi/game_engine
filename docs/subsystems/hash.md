# hash (core)

**Purpose.** Hashing primitives for container keys and small-key identity: a 64-bit finalizer (`mix64`), `hash_combine`, `hash_bytes`, and `Hash<T>` specializations. Header-only.

**Owned data.** None.

**Invariants.**
- Every `Hash<T>` returns a well-mixed 64-bit value; containers take bucket bits from the top and fingerprint bits from the bottom and rely on both being good.
- `Hash<std::string>`, `Hash<std::string_view>`, and `Hash<const char*>` agree on equal text and are transparent, which is what makes heterogeneous lookup on string keys work.
- `Hash<float>` and `Hash<double>` hash `-0.0` and `+0.0` identically because they compare equal.
- There is no fallback for unspecialized types: using `Hash<T>` for a type without a specialization is a compile error, not a weak hash.

**Public API.** `core/hash/hash.h`: `k_hash_seed`, `mix64`, `hash_combine`, `hash_bytes(data, len, seed)`, `Hash<T>` for integers, enums, pointers, floats, string-like types, and pairs; `StringHash`.

**Depends on.** `base`.

**Testing.** `tools/dev.ps1 test -Filter hash`. Covers determinism, length and content sensitivity across the word boundary, representation independence for strings, float zero handling, and top-bit distribution for sequential integers.

**Performance notes.** `hash_bytes` is a word-at-a-time multiplicative hash with the MurmurHash3 finalizer: adequate for hash tables, deterministic across platforms, not cryptographic, and not the content-addressing hash (BLAKE3 or xxHash3 arrive with the derived-data cache). It is a candidate for replacement by a benchmarked fast hash once the tunables harness exists.
