# ids (core)

**Purpose.** Stable 128-bit identity for authored objects (docs/plan/03-data-model.md §3.1, ADR-0002, ADR-0017): `Id128`, its hex encoding, a wall-clock generator, and deterministic generators for procedural content and tests.

**Owned data.** A per-thread random generator for `Id128::generate()`.

**Layout.** The high 48 bits of `hi` are a millisecond Unix timestamp, so ids sort roughly by creation time; the remaining 80 bits are random or, for `from_seed`, a hash of seed and counter with a synthetic timestamp derived from the seed. Serialized as 32 lowercase hex characters. The all-zero id is null and is never generated.

**Invariants.**
- `generate()` never returns null; ids from one thread are non-decreasing in timestamp; ids across threads never collide (tested with 160k ids over 8 threads).
- `from_seed(s, c)` is a pure function of its arguments on every platform, so procedurally generated content keeps its ids across builds and machines. The algorithm is pinned by test; changing it invalidates saved content.
- `from_hex` accepts exactly 32 hex digits of either case and nothing else.

**Public API.** `core/ids/id128.h`: `Id128` (`is_null`, `timestamp_ms`, `to_hex`, `from_hex`, `generate`, `from_seed`, `from_parts`, comparisons), `IdGenerator` (seeded stream), `Hash<Id128>`.

**Depends on.** `base`, `hash`.

**Testing.** `tools/dev.ps1 test -Filter ids`.

**Performance notes.** 16 bytes, trivially copyable, ordered by `<=>`. Hashing combines both words through `mix64`.
