#pragma once

// The typed API surface: a Luau definition file (`.d.luau`) for the engine API and for schema
// types, generated from the same descriptors the bindings read (docs/subsystems/scripting.md,
// "The typed API").
//
// Each struct becomes a `declare extern type` whose properties are all `read`, which is what the
// binding is: a property that exists reads with the IDL's type, a property that does not is a type
// error, and an assignment is a type error before it is a run-time one. Enums become unions of
// their enumerator names as string singletons, because that is what a script sees at run time.
// Loaded into Luau's analyser (Luau::Frontend::loadDefinitionFile) beside a script annotated with
// those types, it turns a misspelt property or a wrong enum name into a diagnostic with a line
// number instead of a run-time error in the middle of a quest.
//
// Stock `luau-analyze` cannot load a definition file (it has no such flag at 0.739), so the
// checker is Luau's analysis library driven directly; the scripting tests do exactly that.
//
// Mapping, one row per schema kind:
//   bool -> boolean; integers and floats -> number; string, bytes, id128 -> string;
//   vec2, vec3 -> vector (vec2 has z = 0); vec4, quat -> { read x, y, z, w: number };
//   json -> any; enum E -> E (a string-singleton union); struct S -> S (an extern type);
//   T? -> T?; T[] and T[N] -> { T } (a read-only table at run time); map<K, V> -> { [K]: V }.

#include <core/schema/type_info.h>

#include <span>
#include <string>

namespace engine::scripting {

// The engine API (`engine.log`, `engine.tunable`, `engine.tick`, `engine.fixed_step`,
// `engine.random`, `engine.object`) followed by every struct and enum reachable from `types`, in a
// deterministic order: enums by name, then structs with each one after the structs it contains.
// A type's simple name is its Luau name unless two emitted types share it, in which case both are
// spelled with their namespace (`engine_doc_Layer`).
std::string emit_type_definitions(std::span<const schema::TypeInfo* const> types);

// The engine API alone.
std::string emit_engine_api_definitions();

}  // namespace engine::scripting
