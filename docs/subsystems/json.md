# json (core)

**Purpose.** The JSON document model, parser, and canonical writer that the authoring document (ADR-0002), the protocol, and all generated serialization use.

**Owned data.** `JsonValue` trees.

**Model.** `JsonValue` is 24 bytes: a kind byte and a 16-byte payload. Kinds: Null, Bool, Int (i64), Uint (u64), Float (f64), String, Array, Object. Strings are byte vectors exposed as `string_view`; arrays are `Vector<JsonValue>`; objects are `FlatMap<std::string, JsonValue>`, so members are always in sorted key order. Integers keep their exact 64-bit value with a signed/unsigned kind; anything with a fraction or exponent is a double. Numbers of different kinds compare equal by value.

**Canonical form** (`write_json`, pretty by default): sorted keys, two-space indentation, integers exact, floats in shortest round-trip form with a `.0` suffix when integral so the kind survives, `-0.0` preserved, minimal escaping (`" \ \n \r \t \b \f`, other control characters as `\u00XX`), UTF-8 passed through. Two equal documents produce byte-identical text on every platform, which is what makes diffs meaningful. NaN and infinity are written as `null` and reported as failure.

**Parser** (`parse_json`): strict JSON, no comments or trailing commas; duplicate keys keep the last value, unless `JsonParseOptions::refuse_duplicate_keys` is set, the strict mode, which refuses the second at its place and names it in `JsonParseResult::duplicate_key`. The strict mode is for a file whose meaning a text merge or a tool's append could change without anyone seeing it: `foundation/content_class` reads every sidecar, categories file and policies file with it, because a class or a cap given twice would otherwise be decided by its order ([content_class](content_class.md)). Every other reader keeps the default. Operator configuration read the same way should move to it (the roles file, the credentials file, the roots file, the task ledger), which is a change of each reader's own and is left to them; `\u` escapes including surrogate pairs decode to UTF-8; integers beyond 64 bits become doubles; errors carry byte offset, line, and column. Nesting deeper than `max_depth` (default 256) is refused: the parser and destructor recurse once per level at roughly 1 KB of stack per level in debug builds, so a raised budget needs a matching thread stack.

**A reader with no tree** (`JsonReader`, `core/json/json_reader.h`; roadmap D40). `parse_json` is built on a pull reader, and the reader is public: `next_value` reads the start of a value — a scalar whole, an array or object opened — and `next_element`/`next_member` walk a container, each member's value read with `next_value`, built with `rest`/`read_value` or passed over with `skip`/`skip_value`, which check every byte as the parser does; `finish` refuses what follows the top-level value. Because the tree parser is that reader building a tree, the two refuse a malformed text with the same message at the same offset, line and column, and hold the same depth budget — and the same strict mode: `next_member` holds `refuse_duplicate_keys`, keeping each open object's keys only while it is set, so `parse_json`'s tree, a skipped value and a schema walk reading straight into its structs refuse a key given twice with the same message at the same offset (`json_text_tests.cpp`, "the strict mode refuses a key given twice on the walk as the parser does"). A string with no escape is a view of the text; one with escapes is decoded into a buffer the next string reuses. It is what lets a schema walk reflect a large file straight into its structs (`schema::read_json_text`, [schema](schema.md#json-text-with-no-tree)) instead of holding a tree of every value of the file while it is copied out — 1.93 bytes of engine heap per byte of a document's text, and the scene reader's peak halved without it ([doc](doc.md#reading-a-file-without-a-tree)).

**One spelling, any sink** (`core/json/json_format.h`). The canonical writer's spellings — strings, integers, floats, indentation, a whole `JsonValue` — are templates over a sink with `append` and `put`; `write_json` is them with a string as the sink, and so is a writer that walks a schema type's fields instead of a tree, or compares what it would write against bytes in hand, so every walk spells a value one way.

**Invariants.**
- `parse_json(write_json(v)) == v` for every value without NaN/infinity.
- `write_json(parse_json(text))` is idempotent.
- On parse failure the output value is null.

**Public API.** `core/json/json_value.h` (`JsonValue`), `core/json/json.h` (`parse_json`, `write_json`, options and result types), `core/json/json_reader.h` (`JsonReader`), `core/json/json_format.h` (the spellings over a sink).

**Depends on.** `base`, `memory`, `containers`.

**Testing.** `tools/dev.ps1 test -Filter json`. Covers kinds and conversions, containers, valid and invalid documents with error positions, the depth budget, a pinned canonical rendering, float round trips including extremes, and UTF-8 pass-through.

**Performance notes.** This is the tooling and document path, not a frame-time path. simdjson remains the planned reader for bulk loads if profiling ever shows parsing on a critical path; the writer stays in-house because canonical output is the point.
