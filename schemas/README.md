# Schemas

Single source of truth for every serialized or transmitted type (ADR-0007): document objects, protocol messages, world events, components, provenance. `schemac` (tools/schemac) compiles each `.schema` file into a C++ header and source with reflection tables, a JSON Schema, and a Markdown page. Generated code is never hand-edited.

Build integration: `engine_schema_library(NAME <module> SCHEMAS <files>)` in a `CMakeLists.txt` runs schemac and declares a module. The engine's schemas live here and compile into the `schemas` module; a test schema lives next to the tests that use it.

## The language

Line-oriented, no semicolons. `//` comments are ignored; `///` comments immediately before a declaration, field, or enumerator become its documentation. A file with CRLF line ends compiles to the same bytes as the LF file on every platform: schemac reads its input as bytes and ends a line, a `///` comment and a string literal at either (roadmap T184; a CR kept in a doc comment reached a generated string literal, which MSVC accepts and Clang and GCC refuse).

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

**Primitives**: `bool`, `u8 u16 u32 u64`, `i8 i16 i32 i64`, `f32 f64`, `string` (UTF-8, `std::string`), `bytes` (`Vector<u8>`, hex in JSON), `id128` (`engine::Id128`, 32 hex characters in JSON), `vec2 vec3 vec4 quat` (`engine::Vec2` etc., float32, JSON arrays of numbers), `worldpos dvec3` (`engine::WorldPos`, `engine::DVec3`: three f64, below), `json` (`engine::JsonValue`, any JSON value; for schema-free payloads such as document property bags).

**Type modifiers** (postfix, composable): `T?` optional, `T[]` array, `T[N]` fixed array, `map<K, V>` where `K` is a string, integer, `id128`, or enum.

**Defaults**: integer, float, `true`/`false`, `"string"`, an enumerator name (enum fields), `[]` (arrays, maps), `null` (optionals), and `[x, y, z]` on a `worldpos` or a `dvec3` (`home: worldpos = [419072, 0, -10000000.25]`; three finite numbers, a world position's inside the range below). A field without a default is value-initialized, which for the two vectors is the origin and zero.

### `worldpos`, `dvec3` and `vec3`: which one a position is

**A position in the world is a `worldpos`; a displacement that may be large is a `dvec3`; anything in a local frame is a `vec3`** ([ADR-0053](../docs/adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md), `core/math/world.h`). At 420 km a float32 steps by 3.1 cm and at 10,000 km by a metre, so a placed thing written as a `vec3` far out moves when the document is saved; three f64 step by 2 nm at 10,000 km.

- **`worldpos`** — a point, metres, y up: where an entity or a record *is* (`engine.world.Transform.position`, `Node.position`, a mover's, a resident's, a place's). Reaches C++ as `engine::WorldPos`, which does not add to another point and does not narrow to a float except through `relative(point, origin)`.
- **`dvec3`** — a displacement in f64: the difference of two world positions, an offset between far things, a route's step before it is known to be small. `engine::DVec3`.
- **`vec3`** — float32, for a frame of its own: a velocity, a direction, a mesh-, body- or bone-local offset (the tissue schema's), a position relative to an emitter or a listener. If the field's documentation would have to say "world position", it is not a `vec3`.

The language keeps the distinction the types keep: a `materialize` row joins a `worldpos` only to a `worldpos` and a `dvec3` only to a `dvec3`; joining either to a `vec3` is refused. All three take `@unit` (a length, or a velocity for a `dvec3` of one), on an optional too (`worldpos? @unit(m)`), and a row between two length units converts in f64.

**On the wire** each is `[x, y, z]`: JSON numbers, each the shortest decimal that reads back to the same double (`std::to_chars`, whose output the C++ standard fixes, so a saved document is the same bytes from MSVC, GCC and Clang, as the content build's containers are). A float32 `vec3` was always written as its exact widened double, so **a document written with a `vec3` position reads the same position as a `worldpos`** and a field can change from one to the other under a version bump without a migration of its JSON. **A `worldpos` read from outside is validated**: three finite numbers, each in [−137,438,953,408, 137,438,953,408) m (`world_cell_valid`: where a 64 m cell's i32 index ends); anything else is refused, naming the field. In memory and in the flat (binary) form each is 24 bytes; the JSON Schema says `"items": {"type": "number", "minimum": -137438953408, "exclusiveMaximum": 137438953408}` for a `worldpos`, and `schema.describe` names them `worldpos` and `dvec3`.

**A reader older than these types** must not meet them silently, so a type that changes a field from `vec3` to `worldpos` bumps its `@version` although its JSON is unchanged: an older build then refuses a save naming the type ("newer than this build's") rather than reading the positions as floats. A build older than the kinds themselves never sees one in a table, since generated tables are compiled with the build that reads them. `engine.world.Transform` and `Node`, `engine.kinematics.Mover` and `engine.npc.Place`, `Resident` and `NpcState` went to version 2 this way on 2026-10-05; the migration corpus's older saves load to the same hash and name the steps (`content/migration-corpus/README.md`).

**An optional array (`T[]?`) is how a field says "absent" apart from "empty"** — `std::optional<Vector<T>>`, `null` or missing in JSON for absent. Use it when the two mean different things: `engine.scene.Terrain.bands` is the first, where absent is the default band table and an empty table is refused as a mistake rather than read as "no dunes". A plain `T[]` cannot tell them apart, since `[]` is its default.

**A field whose type is chosen by a name is a `string` beside a `json`**, and the name's owner reads the `json` with a schema type of its own. `engine.scene.PlacementEntry` is the first: `{generator: string, params: json}`, where `generator` names a placement generator in the scene-generator registry and the generator reads `params` with `schema::from_json` into its own type (the ruins' `engine.scene.RuinScatter`, the city's `engine.city.CityEntry`), so an unknown field is still refused with its path — by the capability that knows the type, which the scene's schema cannot name without depending on every capability ([scene_gen](../docs/subsystems/scene_gen.md), [ADR-0046](../docs/adr/0046-scene-generators-register-themselves.md)). Use it only where the set of types is open by design; a closed set is an enum and a struct per case.

**Attributes**: on structs `@version(n)` (default 1), `@kind(tag)`, `@transient`, `@additive(n, ...)` and `@alias("Old", ...)`; on fields `@since(n)`, `@transient`, `@deprecated`, `@unit(symbol)` and `@alias("old", ...)`; `@doc("...")` anywhere as an alternative to `///`.

**`@unit(symbol)`** says what physical unit a number field is in: `@unit(m)`, `@unit(deg)`, `@unit("km/h")` (quoted when it has a `/`). The symbols are a small closed table in `tools/schemac/src/resolve.cpp` — length, angle, time, mass, velocity, angular velocity, temperature, ratio, and force (`N`, `kN`), energy (`J`, `kJ`) and power (`W`, `kW`), added for the muscle line tier's forces and work ledger, whose fields had named their units in name suffixes (`tendon_force_N`) until the table could say them — and an unknown one, or one on a field that holds no number, is an error. A number, a vector of them (`vec2` to `vec4`, `worldpos`, `dvec3`), a fixed array of them, or an optional of any of those can carry one. A unit changes no C++ type; it is read by `materialize` rows, which convert between two units of one dimension (below), and it appears in the generated Markdown.

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

## `materialize`: what a record becomes

A document record type (`@kind(record)`) becomes entities in the runtime world through a `materialize` declaration: which components it becomes, which record property fills which component field, and what its document parent becomes ([03 §3.4](../docs/plan/03-data-model.md#34-the-runtime-world)). It sits in any schema file that can see both sides — usually the one declaring the record type — and is data, not code: schemac compiles it into a `schema::MaterializeInfo` table the runtime reads, and nothing anywhere is written per type.

```
namespace engine.kinematics
import "world.schema"

struct Velocity @version(1) @kind(component) {
  linear: vec3 @unit("m/s")
  angular: vec3 @unit("rad/s")
}

struct Mover @version(2) @kind(record) {
  name: string
  position: worldpos @unit(m)
  orientation: quat
  velocity: vec3 @unit("m/s")
  spin: vec3 @unit("deg/s")
}

/// A mover is a transform and a velocity.
materialize Mover @tiers(0, 1, 2, 3) {
  Transform.position = position @writeback      // component field = record property
  Transform.orientation = orientation @writeback
  Velocity.linear = velocity                    // same unit: a direct copy
  Velocity.angular = spin                       // deg/s -> rad/s: converted
  parent = ChildOf                              // or `none`, the default
}
```

- **`Component.field = property`** fills one component field from one record property: a direct copy when the two declare the same unit (or none), a conversion when they declare different units of one dimension — `component = record × scale + offset`, computed by schemac from the unit table, applied in f64. The types must join: the same type, or two float widths. A unit on one side only is an error: it cannot say what the other side meant. A property the record does not set gives the component the record type's default.
- **`@writeback`** lets the runtime world write the field back to the record property when a system changes it (at `TickPhase::Persist`, as a document command attributed to `system`; [docs/subsystems/sim.md](../docs/subsystems/sim.md#write-back)). Only a field that is its own bytes can carry it — a number, an enum, an id, a vector, a quaternion, or a fixed array of them — on a component and a field that are not transient, and a property has at most one write-back row.
- **`Component`** alone on a line gives the entity that component with its schema defaults.
- **`parent = ChildOf`** makes the document's parent the entity's `ChildOf` relationship; `none` (the default) leaves the entity a root.
- **`@tiers(...)`** lists the LOD tiers the type materializes at (default all of 0–3); a record asked for at another tier is skipped with the reason.

Names resolve like any named type — this file's namespace, then imports, then a qualified name — and the left side splits at its last dot, so `engine.world.Transform.position` works too. Every row is checked: the record type must be `@kind(record)` and every component `@kind(component)` (a mapping to an undeclared component, or to a struct that is not a component, is refused), the fields and properties must exist, and a record type has one declaration. A record type with no declaration stays in the document and never becomes an entity, which is the right answer for canon, quests and provenance.

## What is generated

- `<schemas/<stem>.h>`: `enum class` and `struct` definitions with defaults, `k_schema_version`, defaulted `operator==`, and `engine::schema::type_of<T>()` specializations.
- `<schemas/<stem>_ecs.h>`: `register_<stem>_components(flecs::world&)`, the entity-store registration for this file's `@kind(component)` structs (see "Components" above). Always written; compiled only where `<flecs.h>` may be included.
- `<stem>.cpp`: constant-initialized `TypeInfo`, `FieldInfo`, and `TypeRef` tables plus a `Registrar` that adds every type to `engine::schema::Registry::global()` at startup; and, for a file with `materialize` declarations, one constant-initialized `MaterializeInfo` per declaration and a `MaterializeRegistrar` that adds them to `engine::schema::MaterializeRegistry::global()`.
- `<stem>.schema.json`: JSON Schema draft 2020-12 with `$defs` per type; `required` lists non-optional, non-container fields without defaults.
- `<stem>.md`: one table per type, with each field's unit, and one per `materialize` declaration.

The engine's own world vocabulary is `world.schema` here: `engine.world.Transform` (the component every placed thing has; its position a `worldpos` since version 2), `engine.world.Node` (the least a record can be and still be somewhere, with its mapping) and `engine.world.WriteBack` (the event a write-back leaves in the store's log).

## Runtime

`core/schema` reads and writes any schema type as JSON through its descriptors (`engine::schema::to_json`, `from_json`), reports errors with field paths, tolerates missing fields (defaults apply), rejects unknown fields unless asked to ignore them, and applies registered migrations when reading an object stored at an older `@version` (`from_json_versioned`).

## Evolution rules (docs/plan/03-data-model.md §3.8)

- Adding a field: bump `@version`, mark the field `@since(new)`, give it a default, and **declare the version additive on the struct, `@additive(new)`** (`@additive(3, 4)` for two). **The step is generated** (roadmap D27, 2026-11-17) for a declared version only (review B4, 2026-11-18): a version that adds a field may also have changed what an existing field means under its name, which the tables cannot see, so an undeclared one is still refused with `no migration registered` until a step is written; schemac refuses `@additive` on a version that is not one of the struct's steps or adds no field. A declared version is an additive one, and `MigrationRegistry::migrate` takes that step with nothing to do to the JSON (the object's missing field takes its default as it is read), so no hand-written no-op is registered for it (`schema::additive_step`, `core/schema/json_reflect.h`). It is derived from the descriptor tables schemac emits — every `FieldInfo` carries its `since_version` — rather than emitted as code beside them, so a type gets it with no generated function per step and a step registered by hand still wins: a version that adds a field and also removes one registers its step as below (a rename is an alias and needs none), and one that forgets to is refused by the reader as an unknown field rather than read wrongly. **A version that adds no field** (a field's type or meaning changed: `engine.world.Node` 2) is not additive and still needs a registered step to be read through `from_json_versioned`.
- Adding an enumerator: append it with the next value, never renumber or reuse one. An enumerator carries no `@since` (the parser reads only `@doc` on it), so its documentation says which version of the struct that reads it introduced it (`engine.tissue.CageKind.TetrahedralQuadratic`, read from `Region` version 2). A reader older than the enumerator refuses its name — `core/schema` reads an unknown enumerator as an error even where the reader asks it to ignore unknown fields — so where an older reader has to fall back rather than refuse, the new thing needs a field or a container section of its own beside the enumerator.
- **Renaming a field or a type: an alias, never delete-and-add** (plan 03 §3.8; roadmap D28, 2026-11-18). Keep the old name as `@alias("old")` on the field, or `@alias("Old")` on the struct (a simple name is in the file's namespace, a dotted one is taken as written; several are allowed, for a thing renamed twice). See "Renaming a field or a type" below.
- Removing a field, or a rename that also changes what the field means: bump `@version` and register a `Migration` for `from_version = old` that rewrites the JSON; keep the migration forever (or as long as the migration corpus needs it).
- Never reuse a field name for a different meaning; never change a field's type in place.
- **A field an older reader would skip and so misread needs a requirement, not only a version.** Where a reader tolerates unknown fields so that a newer file still loads (a container read with `ignore_unknown_fields`), a new field that *changes the meaning of one the reader knows* is read as if absent — an essential attachment read as the spring its version-1 field describes. A file format with such fields carries a list of what its meaning depends on, checked on the JSON before the file is read, and a reader that lacks any of it refuses the file as a capability failure rather than falling back: `engine.tissue.TissueDefinition.requirements` is the first ([tissue](../docs/subsystems/tissue.md#capabilities-and-the-requirements-rule)), naming records and their fields and enumerators by these files' spellings (`"Attachment.enforcement"`, `"AttachmentEnforcement.Essential"`).
- A field's name is its C++ member's, so it cannot be a C++ keyword: the tissue definition's `requires` list is spelled `requirements`.

## Renaming a field or a type

**A rename is an alias, never delete-and-add** ([plan 03 §3.8](../docs/plan/03-data-model.md#38-schema-evolution); roadmap D28, 2026-11-18). The new name is the declaration's, the old one stays beside it:

```
struct Marker @version(1) @kind(record) @alias("Beacon") {   // was engine.doc.test.Beacon
  label: string @alias("title")                               // was `title`
}
```

- **A reader accepts the old name and a writer writes the new.** `from_json` reads a member under a former name as the field, at every version, nested structs included; `to_json` writes the field's name. A delete-and-add would have read the old member as unknown (refused) or, where unknown fields are ignored, dropped it and given the new field its default, which is the silent loss this rule exists to prevent.
- **A record carrying both is refused by name.** An object holding a field under its current name and a former one, or under two former ones, is an error naming both ("carries both 'title' and 'label'"), never read as whichever came last: the two values may differ, and nothing says which the author meant.
- **What schemac checks.** A field's former name is an identifier, is not a name a field of the struct still has, and is claimed by one field only; a type's former name is qualified (a simple name in the file's namespace), is not a type declared now, and is claimed by one type of everything compiled together. The runtime registry refuses, across libraries and in either arrival order, a name that would mean two types: two types registering one former name, and a former name that is another type's current name ([schema](../docs/subsystems/schema.md), roadmap D44).
- **What reaches the runtime** (`core/schema/type_info.h`): `TypeInfo::aliases` (the type's former qualified names) and `TypeInfo::field_aliases` (`FieldAlias`: a former name and the field's index), with `find_field_alias`. They are on the type and not on `FieldInfo`, whose array a hot-reloaded game module hands the host: the type descriptor's size is checked at the module boundary and refused when it differs ([hotload](../docs/subsystems/hotload.md)), so a module built before these fields existed is refused rather than misread. `Registry::find` answers a current name only — what a writer names, and what a check that a name is registered asks; `Registry::find_former` a former one; `Registry::resolve` either, which is what a reader of a stored name calls (a document record's `type`, a layer file's type table).
- **A holder of raw JSON renames it in place**: `schema::rename_former_fields` (`json_reflect.h`) rewrites an object's members, and every struct value's inside it, to the current names. A document's records are such JSON, and a document is renamed when it is loaded — definitions as their files are read, overrides by the type the composed index gives them — so its next save writes the new names ([doc](../docs/subsystems/doc.md#a-renamed-type-or-field)).
- **A rename alone needs no version.** Every reader takes the former name at every version, so there is nothing for a step to do. What a version would add is the other direction, an older build meeting the new name: `from_json` refuses it as an unknown field, which is a refusal and not a misreading; a document keeps a record of a type it does not know as data, and validation reports a property it does not know. Where an older build must refuse a newer document outright — a record type it would materialize with the renamed field at its default — bump `@version` too, so the layer's type table refuses the file ([doc](../docs/subsystems/doc.md#a-layer-says-which-versions-it-holds)), and register that version's step, which has nothing to rewrite.
- **Every other reader of a stored name** (roadmap D41): a save's manifest resolves a former type name and an event's type id finds the renamed type (`find_by_stable_id`, `has_stable_id`), its payload handed on in the current names; a checkpoint's records are renamed as a load renames a document's; a layer partitioned by a renamed field follows the rename, or is refused by name when its types disagree ([doc](../docs/subsystems/doc.md#a-renamed-type-or-field)).
- **A client's command naming a former name is refused, naming the current one** ("'title' is a former name of engine.doc.test.Marker's 'label': write 'label'"); the engine's own replays of a journal written before the rename are renamed and applied ([doc](../docs/subsystems/doc.md#a-command-that-names-a-former-name)).
- **A hot-reloaded game module built before a rename** names a type or a field path by its former names and resolves it (`ModuleHost::resolve` answers through `Registry::resolve` and `find_field_alias`, roadmap D43): the old and the current spelling of one path share one host entry, kept under the current names, so a refusal message names what the schema says now ([hotload](../docs/subsystems/hotload.md)).
- **Not yet.** The protocol's methods that take a type name from a client (`schema.describe`, the role and placement checks) still look it up by the current name only.
