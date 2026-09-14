# json (core)

**Purpose.** The JSON document model, parser, and canonical writer that the authoring document (ADR-0002), the protocol, and all generated serialization use.

**Owned data.** `JsonValue` trees.

**Model.** `JsonValue` is 24 bytes: a kind byte and a 16-byte payload. Kinds: Null, Bool, Int (i64), Uint (u64), Float (f64), String, Array, Object. Strings are byte vectors exposed as `string_view`; arrays are `Vector<JsonValue>`; objects are `FlatMap<std::string, JsonValue>`, so members are always in sorted key order. Integers keep their exact 64-bit value with a signed/unsigned kind; anything with a fraction or exponent is a double. Numbers of different kinds compare equal by value.

**Canonical form** (`write_json`, pretty by default): sorted keys, two-space indentation, integers exact, floats in shortest round-trip form with a `.0` suffix when integral so the kind survives, `-0.0` preserved, minimal escaping (`" \ \n \r \t \b \f`, other control characters as `\u00XX`), UTF-8 passed through. Two equal documents produce byte-identical text on every platform, which is what makes diffs meaningful. NaN and infinity are written as `null` and reported as failure.

**Parser** (`parse_json`): strict JSON, no comments or trailing commas; duplicate keys keep the last value; `\u` escapes including surrogate pairs decode to UTF-8; integers beyond 64 bits become doubles; errors carry byte offset, line, and column. Nesting deeper than `max_depth` (default 256) is refused: the parser and destructor recurse once per level at roughly 1 KB of stack per level in debug builds, so a raised budget needs a matching thread stack.

**Invariants.**
- `parse_json(write_json(v)) == v` for every value without NaN/infinity.
- `write_json(parse_json(text))` is idempotent.
- On parse failure the output value is null.

**Public API.** `core/json/json_value.h` (`JsonValue`), `core/json/json.h` (`parse_json`, `write_json`, options and result types).

**Depends on.** `base`, `memory`, `containers`.

**Testing.** `tools/dev.ps1 test -Filter json`. Covers kinds and conversions, containers, valid and invalid documents with error positions, the depth budget, a pinned canonical rendering, float round trips including extremes, and UTF-8 pass-through.

**Performance notes.** This is the tooling and document path, not a frame-time path. simdjson remains the planned reader for bulk loads if profiling ever shows parsing on a critical path; the writer stays in-house because canonical output is the point.
