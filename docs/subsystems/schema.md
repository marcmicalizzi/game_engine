# schema (core) and schemac (tools)

**Purpose.** Every serialized or transmitted type is declared once in a `.schema` file (ADR-0007). `schemac` compiles it to C++ types with constant-initialized reflection tables, a JSON Schema, and Markdown; the `schema` runtime reads and writes any such type as JSON through those tables and applies version migrations. Agents never write serialization code. The IDL is documented in `schemas/README.md`.

**Owned data.** The global type `Registry` (qualified name → `TypeInfo`) and the global `MigrationRegistry`.

**Descriptors.** `TypeInfo` (struct or enum: names, version, tag, doc, fields or enumerators, type-erased `StructOps`); `FieldInfo` (name, `TypeRef`, offset, `@since`, flags, doc); `TypeRef` (a `Kind` plus, for containers and optionals, element/key refs and type-erased `OptionalOps`/`ArrayOps`/`MapOps`). Generated code references the op tables from `core/schema/ops.h` so the walker never instantiates templates.

**JSON conventions.** Structs are objects keyed by field name, transient fields omitted; enums are names (integers accepted on read, unknown enumerators written as integers); optionals are null when empty; `Id128` and `bytes` are hex strings; maps with string keys are objects, other maps are arrays of `[key, value]`; fixed arrays must match their declared length. Missing fields keep defaults; unknown fields are errors unless `ReadOptions::ignore_unknown_fields`. Diagnostics carry a path such as `inners[1].x`.

**Migrations.** A `Migration{type, from_version, apply}` rewrites the JSON of one version step. `from_json_versioned` applies every step from the stored version to the current one, then maps; a missing step or a stored version newer than the build is an error.

**Invariants.**
- Every generated `TypeInfo` is constant-initialized, so descriptors are valid at any point during static initialization; registration happens in a `Registrar` during dynamic initialization and the registry is safe to query from `main()` onward.
- `from_json(to_json(x)) == x` for every schema type, modulo transient fields.
- Two types never share a qualified name (verified at registration).

**Build integration.** `engine_schema_library(NAME m SCHEMAS a.schema b.schema)` runs schemac once over the set (imports also resolve against `schemas/`) and declares module `m` from the generated sources; include as `<schemas/<stem>.h>`. Outputs go to `build/<preset>/<dir>/generated/<m>/{include,src,json,docs}`. schemac writes files only when content changes, so unchanged schemas do not trigger rebuilds. schemac itself is a standalone tool (standard library only) under `tools/schemac`, built before any module.

**Public API.** `core/schema/type_info.h`, `core/schema/ops.h`, `core/schema/json_reflect.h`.

**Depends on.** `base`, `hash`, `memory`, `containers`, `ids`, `json`.

**Testing.** `tools/dev.ps1 test -Filter schema`. A feature-complete schema (`core/schema/tests/features.schema`) is generated into a test-only module; tests pin the generated defaults, descriptors, a canonical JSON rendering, error paths, enum handling, and migrations. The generator's own error paths (unknown types, bad defaults, missing imports) are exercised manually for now; a schemac negative-test harness is a follow-up.

**Performance notes.** Reflection-driven JSON is the tooling and document path. Hot-path binary serialization, when needed, will be generated directly from the same descriptors rather than walked at runtime.
