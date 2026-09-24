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

**Primitives**: `bool`, `u8 u16 u32 u64`, `i8 i16 i32 i64`, `f32 f64`, `string` (UTF-8, `std::string`), `bytes` (`Vector<u8>`, hex in JSON), `id128` (`engine::Id128`, 32 hex characters in JSON), `vec2 vec3 vec4 quat` (`engine::Vec2` etc., JSON arrays of numbers), `json` (`engine::JsonValue`, any JSON value; for schema-free payloads such as document property bags).

**Type modifiers** (postfix, composable): `T?` optional, `T[]` array, `T[N]` fixed array, `map<K, V>` where `K` is a string, integer, `id128`, or enum.

**Defaults**: integer, float, `true`/`false`, `"string"`, an enumerator name (enum fields), `[]` (arrays, maps), `null` (optionals). A field without a default is value-initialized.

**Attributes**: on structs `@version(n)` (default 1), `@kind(tag)` and `@transient`; on fields `@since(n)`, `@transient`, `@deprecated`; `@doc("...")` anywhere as an alternative to `///`.

**`@transient` on a struct is not the same thing as `@transient` on a field.** A transient *field* is one column of a type that is otherwise persisted and transmitted. A transient *struct* is a whole type that exists only in the runtime world and is never written to the persistent store ([03 §3.4](../docs/plan/03-data-model.md#34-the-runtime-world)) — a per-tick cache, a perception result, anything the simulation can recompute. Only a component can be transient as a whole, because a component is the only thing the persistence layer writes whole, so `@transient` on a struct without `@kind(component)` is an error rather than a no-op. It reaches C++ as `TypeInfo::flags & schema::TypeFlag::transient`, which is how the store, the protocol and migrations read it without linking an ECS, and as the `ecs::Transient` tag on the flecs component (see below).

**Cross-file references**: `import "core.schema"` then use `Vec3`, or write the qualified name `engine.core.Vec3` (still requires the import). Imports resolve against this directory.

## Components

A struct carrying `@kind(component)` is a **world component** ([ADR-0028](../docs/adr/0028-ecs-and-persistent-store.md) seam 1). It is declared here once and nowhere else: the protocol, persistence, migrations and the systems that iterate it all see the same type, and a hand-registered ECS component is allowed only for module-private state that is never persisted and never visible to the protocol.

```
namespace engine.cloth

/// One panel's simulated state.
struct Panel @version(1) @kind(component) {
  rest_length: f32 = 0.1
  anchor: id128
}

/// The solver's working set for this tick. Recomputed every tick, so never saved.
struct PanelScratch @version(1) @kind(component) @transient {
  iterations: u32 = 0
}
```

`schemac` emits, in addition to the usual outputs, `<schemas/<stem>_ecs.h>` — a header whose `register_<stem>_components(flecs::world&)` registers every component the file declares with the entity store, under its schema-qualified name, with member reflection for the fields that map onto flecs' meta types and with `@transient` as a tag. It is a **header** because the generated schema module sits in the core layer and must not link flecs, while the registration must; a header is compiled only where it is included, and [ADR-0028](../docs/adr/0028-ecs-and-persistent-store.md) seam 5 already restricts who may include `<flecs.h>` to `domain/ecs`, `systems/`, `game/` and their tests and benches. It is written for every schema in every configuration and compiled in none where `ENGINE_WITH_ECS` is off, because nothing may include it there.

A capability's own components live with the capability, in `<layer>/<name>/schemas/<name>.schema`, compiled by `engine_schema_library(NAME <name>_schemas SCHEMAS schemas/<name>.schema CAPABILITY <name>)`, so adding them touches no shared file and they leave the build with the capability ([ADR-0027](../docs/adr/0027-additive-capabilities.md)). What the entity store does with the registration, and which schema kinds it can describe to flecs, is in [docs/subsystems/ecs.md](../docs/subsystems/ecs.md).

**Declared in one module, registered by another.** A capability split across layers declares its components with the ECS-free half and registers them from the half that may see flecs. `domain/audio/schemas/audio.schema` is the first: `AudioEmitter` and `AudioListener` are declared beside the mixer, and `systems/audio_system` calls the generated `register_audio_components()`, because the generated struct is plain C++ that any module may use and only the registration needs `<flecs.h>`. The same file's enumerations (`ChannelLayout`, `DistanceModel`, `Directivity`, `ChannelMapping`) are the mixer's C++ API types directly, so the IDL and the API cannot disagree about what a speaker layout or a distance model is ([docs/subsystems/audio.md](../docs/subsystems/audio.md)).

## What is generated

- `<schemas/<stem>.h>`: `enum class` and `struct` definitions with defaults, `k_schema_version`, defaulted `operator==`, and `engine::schema::type_of<T>()` specializations.
- `<schemas/<stem>_ecs.h>`: `register_<stem>_components(flecs::world&)`, the entity-store registration for this file's `@kind(component)` structs (see "Components" above). Always written; compiled only where `<flecs.h>` may be included.
- `<stem>.cpp`: constant-initialized `TypeInfo`, `FieldInfo`, and `TypeRef` tables plus a `Registrar` that adds every type to `engine::schema::Registry::global()` at startup.
- `<stem>.schema.json`: JSON Schema draft 2020-12 with `$defs` per type; `required` lists non-optional, non-container fields without defaults.
- `<stem>.md`: one table per type.

## Runtime

`core/schema` reads and writes any schema type as JSON through its descriptors (`engine::schema::to_json`, `from_json`), reports errors with field paths, tolerates missing fields (defaults apply), rejects unknown fields unless asked to ignore them, and applies registered migrations when reading an object stored at an older `@version` (`from_json_versioned`).

## Evolution rules (docs/plan/03-data-model.md §3.8)

- Adding a field: bump `@version`, mark the field `@since(new)`, give it a default.
- Renaming or removing a field: bump `@version` and register a `Migration` for `from_version = old` that rewrites the JSON; keep the migration forever (or as long as the migration corpus needs it).
- Never reuse a field name for a different meaning; never change a field's type in place.
