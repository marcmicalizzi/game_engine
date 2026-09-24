#pragma once

// ScriptContext's private state, shared by the files of src/. The only place outside Luau's own
// sources that includes lua.h: nothing under include/ does, and nothing outside this module links
// Luau's include directories (foundation/scripting/CMakeLists.txt).
//
// Host functions a script can call (the engine API, the metamethods of object views, the
// interrupt, the allocator) run between a Luau raise and the protected call that catches it, and
// Luau is built with LUA_USE_LONGJMP=1 (cmake/EngineScripting.cmake). So every such function keeps
// to one rule: **no local with a destructor is alive at any point where it can raise** — luaL_error
// and every Luau call that allocates. std::string, Vector and JsonValue live in Impl, never on the
// stack of a function a script called, unless the function cannot raise while they exist.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/memory/memory.h>
#include <core/schema/type_info.h>
#include <foundation/scripting/script_context.h>

#include <lua.h>
#include <lualib.h>
#include <string>
#include <string_view>

namespace engine::scripting {

// The userdata tag of a schema object view. Tagged userdata carry their metatable in the VM's
// per-tag table (lua_setuserdatametatable), so creating a view is one allocation and no lookup.
inline constexpr int k_view_tag = 1;

// A read-only view of a schema struct, as the script holds it. It never owns what it shows and
// never points at it unchecked: a root view names its binding slot and the slot's serial, and
// reads whatever object the slot holds now; a nested view (`npc.stats`) points into the object
// but carries the slot's version, which rebind() and unbind() bump. A view whose stamp no longer
// matches reads as an error, never as memory.
struct View {
  const void* data;  // nullptr: the slot's current object (a root view)
  u32 slot;
  u32 stamp;       // the slot's serial (root view) or version (nested view) at creation
  u32 type_index;  // Impl::types, for a nested view; a root view reads the slot's
  u32 reserved;
};
static_assert(sizeof(View) == 24, "scripting: a view is 24 bytes of VM heap plus its header");

// Field lookup by string atom. Luau assigns every string an atom when it is created
// (lua_Callbacks::useratom); the context gives every field name of every registered schema struct
// one, so `npc.health` finds its field by a binary search over a few 4-byte entries instead of
// string compares.
struct FieldAtom {
  i16 atom;
  u16 field;
};

struct TypeBinding {
  const schema::TypeInfo* type = nullptr;
  Vector<FieldAtom> by_atom;    // sorted by atom
  bool atoms_complete = false;  // every field has an atom, so a miss by atom is a miss
};

struct Slot {
  const schema::TypeInfo* type = nullptr;
  const void* object = nullptr;
  u32 serial = 1;   // bumped by unbind: root views of the old binding go stale
  u32 version = 1;  // bumped by unbind and rebind: nested views go stale
  u32 type_index = 0;
  int view_ref = LUA_NOREF;  // the binding's root view, created once and reused every call
  bool live = false;
  bool temporary = false;  // bound for one call (Arg::object, engine.object)
};

struct Script {
  std::string name;  // the chunk name
  std::string path;  // empty unless loaded from a file
  i64 mtime_ms = 0;
  u64 file_size = 0;
  u64 content_hash = 0;
  int env_ref = LUA_NOREF;  // the script's frozen global table
  u32 generation = 0;
  bool reported_failure = false;  // hot reload logs a broken save once
};

struct FunctionSlot {
  u32 script = 0;
  int ref = LUA_NOREF;
  std::string name;
};

// Why the call in flight failed, recorded where it happened (the interrupt, the allocator) rather
// than read back from a message a script could have written itself.
enum class Cause : u8 { None, Budget, Time, Memory };

struct ScriptContext::Impl {
  lua_State* L = nullptr;
  ScriptLimits limits;
  mem::TagId tag;

  // The VM heap, as the allocator sees it.
  u64 heap = 0;
  u64 heap_peak = 0;
  u64 allocated = 0;

  // The call in flight.
  u32 depth = 0;
  u64 steps = 0;
  u64 step_limit = 0;
  i64 deadline_ns = 0;
  Cause cause = Cause::None;
  int budget_message_ref = LUA_NOREF;
  int time_message_ref = LUA_NOREF;

  // Where the error handler found the first Luau frame of a failing call.
  char error_chunk[256] = {};
  u32 error_line = 0;
  bool error_located = false;

  Vector<Script> scripts;
  Vector<FunctionSlot> functions;
  Vector<Slot> slots;
  Vector<u32> free_slots;
  Vector<u32> temporaries;
  Vector<TypeBinding> types;
  HashMap<const schema::TypeInfo*, u32> type_index;
  HashMap<std::string_view, i16> atoms;
  bool atoms_exhausted = false;

  ObjectResolver resolver;
  u64 tick = 0;
  f64 fixed_step = 0.0;

  ScriptError error;
  u64 calls = 0;
  u64 last_call_steps = 0;

  Vector<u64>* gc_pauses = nullptr;
  bool in_gc_step = false;
  i64 gc_step_start = 0;
};

using Impl = ScriptContext::Impl;

inline Impl& impl_of(lua_State* L) noexcept {
  return *static_cast<Impl*>(lua_callbacks(L)->userdata);
}

// ---- bindings.cpp -----------------------------------------------------------------------------

// Builds the field-name atoms from the schema registry. Before the VM exists: every string the VM
// creates afterwards is matched against them.
void build_atoms(Impl& impl);
i16 atom_of(Impl& impl, const char* s, usize len) noexcept;
// The view metatable, registered for k_view_tag.
void register_views(lua_State* L);
u32 type_binding(Impl& impl, const schema::TypeInfo& type);
// A slot for `object`; its root view is created (and, for a persistent binding, referenced) by
// the caller. Grows the slot table only when no slot is free.
u32 acquire_slot(Impl& impl, ObjectRef object, bool temporary);
void release_slot(Impl& impl, u32 slot) noexcept;
// Pushes a new root view of `slot`. May raise (it allocates).
void push_root_view(lua_State* L, Impl& impl, u32 slot);
// Releases every slot bound for the call that just ended.
void release_temporaries(Impl& impl) noexcept;
// Resolves a view to the object it shows, or returns false when it is stale.
bool resolve_view(const Impl& impl, const View& view, const void*& data,
                  const TypeBinding*& binding) noexcept;

// ---- values.cpp -------------------------------------------------------------------------------

// Pushes a schema value (a field, an array element) as the script sees it. May raise.
void push_value(lua_State* L, Impl& impl, const schema::TypeRef& type, const void* value, u32 slot,
                u32 version);
// Pushes a JSON value as a read-only table or scalar. May raise.
void push_json(lua_State* L, const JsonValue& value);
// Converts the value at `index` to JSON. Never raises: it touches only values that need no
// allocation to read, and reports what it cannot convert in `error`.
bool to_json(lua_State* L, Impl& impl, int index, JsonValue& out, std::string& error);

}  // namespace engine::scripting
