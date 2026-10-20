# ids (core)

**Purpose.** Stable 128-bit identity for authored objects (docs/plan/03-data-model.md §3.1, ADR-0002, ADR-0017): `Id128`, its hex encoding, a wall-clock generator, and deterministic generators for procedural content and tests.

**Owned data.** A per-thread random generator for `Id128::generate()`, and one process-wide `TimeOrderedIds`: an atomic word holding the last (millisecond << 16 | sequence) it issued.

**Layout.** The high 48 bits of `hi` are a millisecond Unix timestamp, so ids sort by creation time. For `generate()` the low 16 bits of `hi` are a sequence within the millisecond and `lo` is 64 random bits; for `from_seed` all 80 bits are a hash of seed and counter, with a synthetic timestamp derived from the seed. Serialized as 32 lowercase hex characters. The all-zero id is null and is never generated.

**The clock rule.** The wall clock is not monotone: NTP slews it, a container's steps it back (gate b33 saw two consecutive ids at 1792526767780 then ...779 ms), and a timestamp read twice can go down. So `generate()` does not stamp the clock's reading; it asks `TimeOrderedIds::next(now_ms)`, which issues `max(now_ms << 16 | r, last + 1)` with `r` a random sequence start below `0x8000`, by compare-and-swap, as UUIDv7 generators do (RFC 9562 §6.2, "monotonic random" with a counter). A clock behind the last id borrows from the millisecond issued last; more ids in one millisecond than its sequence holds (at least 32768) borrow from the next; neither ever waits for the clock. The random start keeps two processes that generate in the same millisecond from sharing high words, and the 64 random bits of `lo` keep their ids apart where they do. The source is a class rather than a hidden static so a test can feed it a clock.

**Invariants.**
- `generate()` never returns null; every id it returns in a process is strictly greater than every id it returned before, across threads and whatever the wall clock did (`hi` alone is strictly increasing), and its timestamp is never behind the latest clock reading it was given; ids across threads and processes never collide (tested with 160k ids over 8 threads, and with a fed clock that steps back 5 ms, stays behind, and overflows a millisecond's sequence: 73,010 ids, all increasing and unique).
- `from_seed(s, c)` is a pure function of its arguments on every platform, so procedurally generated content keeps its ids across builds and machines. The algorithm is pinned by test; changing it invalidates saved content.
- `from_hex` accepts exactly 32 hex digits of either case and nothing else.

**Public API.** `core/ids/id128.h`: `Id128` (`is_null`, `timestamp_ms`, `to_hex`, `from_hex`, `generate`, `from_seed`, `from_parts`, comparisons), `TimeOrderedIds` (`next(now_ms)`, `last`: the clock rule), `IdGenerator` (seeded stream), `Hash<Id128>`.

**Depends on.** `base`, `hash`.

**Testing.** `tools/dev.ps1 test -Filter ids`.

**Performance notes.** 16 bytes, trivially copyable, ordered by `<=>`. Hashing combines both words through `mix64`.
