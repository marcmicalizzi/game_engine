# Schemas

Single source of truth for every serialized or transmitted type (ADR-0007): document objects, protocol messages, world events, components, provenance. `schemac` (tools/schemac) compiles each `.schema` file into a C++ header and source with reflection tables, a JSON Schema, and a Markdown page. Generated code is never hand-edited.

Build integration: `engine_schema_library(NAME <module> SCHEMAS <files>)` in a `CMakeLists.txt` runs schemac and declares a module. The engine's schemas live here and compile into the `schemas` module; a test schema lives next to the tests that use it.

## The language

Line-oriented, no semicolons. `//` comments are ignored; `///` comments immediately before a declaration, field, or enumerator become its documentation.

```
namespace engine.content          // required; becomes the C++ namespace engine::content
import "other.schema"             // types from other.schema may be referenced by simple name

/// Doc for the enum.
enum LicenseClass : u8 {          // underlying type: any integer primitive (default u32)
  Permissive = 0                  // values default to previous + 1
  Proprietary = 3
}

/// Doc for the struct.
struct AssetProvenance @version(1) @kind(record) {
  generator: string               // value-initialized when no default is given
  model_id: string?               // optional: std::optional<std::string>, JSON null when empty
  prompt_hash: u64 = 0            // literal default
  inputs: id128[]                 // dynamic array: engine::Vector<engine::Id128>
  position: f32[3]                // fixed array: std::array<f32, 3>
  by_name: map<string, i32>       // engine::FlatMap; string keys serialize as a JSON object
  license_class: LicenseClass = Unknown   // enum default by enumerator name
  added: string = "v2" @since(2)  // introduced in schema version 2
  scratch: u32 @transient         // never serialized
  old: i32 @deprecated            // still serialized; flagged in docs and JSON Schema
}
```

**Primitives**: `bool`, `u8 u16 u32 u64`, `i8 i16 i32 i64`, `f32 f64`, `string` (UTF-8, `std::string`), `bytes` (`Vector<u8>`, hex in JSON), `id128` (`engine::Id128`, 32 hex characters in JSON).

**Type modifiers** (postfix, composable): `T?` optional, `T[]` array, `T[N]` fixed array, `map<K, V>` where `K` is a string, integer, `id128`, or enum.

**Defaults**: integer, float, `true`/`false`, `"string"`, an enumerator name (enum fields), `[]` (arrays, maps), `null` (optionals). A field without a default is value-initialized.

**Attributes**: on structs `@version(n)` (default 1) and `@kind(tag)`; on fields `@since(n)`, `@transient`, `@deprecated`; `@doc("...")` anywhere as an alternative to `///`.

**Cross-file references**: `import "core.schema"` then use `Vec3`, or write the qualified name `engine.core.Vec3` (still requires the import). Imports resolve against this directory.

## What is generated

- `<schemas/<stem>.h>`: `enum class` and `struct` definitions with defaults, `k_schema_version`, defaulted `operator==`, and `engine::schema::type_of<T>()` specializations.
- `<stem>.cpp`: constant-initialized `TypeInfo`, `FieldInfo`, and `TypeRef` tables plus a `Registrar` that adds every type to `engine::schema::Registry::global()` at startup.
- `<stem>.schema.json`: JSON Schema draft 2020-12 with `$defs` per type; `required` lists non-optional, non-container fields without defaults.
- `<stem>.md`: one table per type.

## Runtime

`core/schema` reads and writes any schema type as JSON through its descriptors (`engine::schema::to_json`, `from_json`), reports errors with field paths, tolerates missing fields (defaults apply), rejects unknown fields unless asked to ignore them, and applies registered migrations when reading an object stored at an older `@version` (`from_json_versioned`).

## Evolution rules (docs/plan/03-data-model.md §3.8)

- Adding a field: bump `@version`, mark the field `@since(new)`, give it a default.
- Renaming or removing a field: bump `@version` and register a `Migration` for `from_version = old` that rewrites the JSON; keep the migration forever (or as long as the migration corpus needs it).
- Never reuse a field name for a different meaning; never change a field's type in place.
