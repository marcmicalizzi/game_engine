#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/tick_hash.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace engine::ecs {

namespace {

using schema::Kind;

// One name per phase, so a schedule listing says which of the hash's systems is which.
constexpr const char* k_system_names[sim::k_phase_count] = {
    "ecs.tick_hash.input",      "ecs.tick_hash.events_in", "ecs.tick_hash.lod",
    "ecs.tick_hash.systems",    "ecs.tick_hash.physics",   "ecs.tick_hash.post_physics",
    "ecs.tick_hash.events_out", "ecs.tick_hash.persist"};

u64 word(u64 h, u64 value) noexcept { return hash_detail::mix_word(h, value); }

template <class T>
u64 scalar(const void* p) noexcept {
  T value;
  std::memcpy(&value, p, sizeof(T));
  if constexpr (sizeof(T) == 8) {
    u64 out;
    std::memcpy(&out, &value, 8);
    return out;
  } else {
    // Widened by value, so a u8 and an i8 of the same bits hash as their own numbers.
    return static_cast<u64>(value);
  }
}

u64 float_bits(const void* p) noexcept {
  u32 bits;
  std::memcpy(&bits, p, 4);
  return bits;
}

u64 hash_ref(const schema::TypeRef& type, const void* p, u64 h);

u64 hash_kind(Kind kind, const void* p, u64 h) {
  switch (kind) {
    case Kind::Bool: return word(h, *static_cast<const bool*>(p) ? 1u : 0u);
    case Kind::U8: return word(h, scalar<u8>(p));
    case Kind::U16: return word(h, scalar<u16>(p));
    case Kind::U32: return word(h, scalar<u32>(p));
    case Kind::U64: return word(h, scalar<u64>(p));
    case Kind::I8: return word(h, static_cast<u64>(static_cast<i64>(scalar<i8>(p))));
    case Kind::I16: return word(h, static_cast<u64>(static_cast<i64>(scalar<i16>(p))));
    case Kind::I32: return word(h, static_cast<u64>(static_cast<i64>(scalar<i32>(p))));
    case Kind::I64: return word(h, scalar<i64>(p));
    case Kind::F32: return word(h, float_bits(p));
    case Kind::F64: return word(h, scalar<u64>(p));
    default: return h;
  }
}

u64 hash_floats(const void* p, u32 count, u64 h) {
  for (u32 i = 0; i < count; ++i)
    h = word(h, float_bits(static_cast<const u8*>(p) + 4u * i));
  return h;
}

u64 hash_struct(const schema::TypeInfo& info, const void* p, u64 h) {
  for (const schema::FieldInfo& field : info.fields) {
    // A transient field is not state the capture can show (`to_json` leaves it out), so the hash
    // leaves it out too: a table that differs always has a field the diff can name.
    if ((field.flags & schema::FieldFlag::transient) != 0) continue;
    h = hash_ref(field.type, static_cast<const u8*>(p) + field.offset, h);
  }
  return h;
}

u64 hash_ref(const schema::TypeRef& type, const void* p, u64 h) {
  switch (type.kind) {
    case Kind::String: {
      const auto* s = static_cast<const std::string*>(p);
      return word(h, hash_bytes(s->data(), s->size()));
    }
    case Kind::Bytes: {
      const auto* b = static_cast<const Vector<u8>*>(p);
      return word(h, hash_bytes(b->data(), b->size()));
    }
    case Kind::Id128: {
      const auto* id = static_cast<const Id128*>(p);
      return word(word(h, id->hi), id->lo);
    }
    case Kind::Vec2: return hash_floats(p, 2, h);
    case Kind::Vec3: return hash_floats(p, 3, h);
    case Kind::Vec4:
    case Kind::Quat: return hash_floats(p, 4, h);
    case Kind::WorldPos:
    case Kind::DVec3:
      for (u32 i = 0; i < 3; ++i)
        h = word(h, scalar<u64>(static_cast<const u8*>(p) + 8u * i));
      return h;
    case Kind::Json: {
      // Canonical text: the one kind that allocates, and only for a component that holds one.
      const std::string text = write_json(*static_cast<const JsonValue*>(p));
      return word(h, hash_bytes(text.data(), text.size()));
    }
    case Kind::Enum: return type.type != nullptr ? hash_kind(type.type->enum_underlying, p, h) : h;
    case Kind::Struct: return type.type != nullptr ? hash_struct(*type.type, p, h) : h;
    case Kind::Optional: {
      if (type.optional_ops == nullptr || type.element == nullptr) return h;
      if (!type.optional_ops->has_value(p)) return word(h, 0);
      return hash_ref(*type.element, type.optional_ops->get(p), word(h, 1));
    }
    case Kind::Array:
    case Kind::FixedArray: {
      if (type.array_ops == nullptr || type.element == nullptr) return h;
      const usize n = type.array_ops->size(p);
      h = word(h, n);
      for (usize i = 0; i < n; ++i)
        h = hash_ref(*type.element, type.array_ops->at(p, i), h);
      return h;
    }
    case Kind::Map: {
      if (type.map_ops == nullptr || type.element == nullptr || type.key == nullptr) return h;
      const usize n = type.map_ops->size(p);
      h = word(h, n);
      for (usize i = 0; i < n; ++i) {
        h = hash_ref(*type.key, type.map_ops->key_at(p, i), h);
        h = hash_ref(*type.element, type.map_ops->value_at(p, i), h);
      }
      return h;
    }
    default: return hash_kind(type.kind, p, h);
  }
}

// ---- the flat program (ecs.md, "The tick hash's fast path") -------------------------------------
//
// A type whose every hashed field is a fixed group of scalars at a fixed offset is hashed by a
// program computed once: runs of (offset, count, load), each load turning one scalar into the very
// word `hash_kind` and `hash_ref` feed `mix_word` for it, in the same order, so the hash is the
// reflected one bit for bit with no walk of the type, no switch a field and no recursion.

using Op = detail::TickHashOp;

enum Load : u8 {
  k_load_u8,    // U8, widened by value
  k_load_u16,   // U16
  k_load_u32,   // U32, F32's bits
  k_load_u64,   // U64, I64, F64's bits, a world position's and an id's words
  k_load_i8,    // I8, sign-extended
  k_load_i16,   // I16
  k_load_i32,   // I32
  k_load_bool,  // Bool: 0 or 1
};

constexpr u32 k_load_width[] = {1, 2, 4, 8, 1, 2, 4, 1};

void push_op(Vector<Op>& ops, u32 offset, u8 load, u32 count) {
  const u32 width = k_load_width[load];
  for (u32 k = 0; k < count; ++k) {
    const u32 at = offset + k * width;
    // A run grows while the next scalar of the same load lies right after it.
    if (!ops.empty() && ops.back().load == load && ops.back().count < 0xFFFFu &&
        ops.back().offset + u32{ops.back().count} * width == at) {
      ++ops.back().count;
    } else {
      ops.push_back(Op{at, 1, load});
    }
  }
}

bool scalar_load(Kind kind, u8& load) {
  switch (kind) {
    case Kind::Bool: load = k_load_bool; return true;
    case Kind::U8: load = k_load_u8; return true;
    case Kind::U16: load = k_load_u16; return true;
    case Kind::U32:
    case Kind::F32: load = k_load_u32; return true;
    case Kind::U64:
    case Kind::I64:
    case Kind::F64: load = k_load_u64; return true;
    case Kind::I8: load = k_load_i8; return true;
    case Kind::I16: load = k_load_i16; return true;
    case Kind::I32: load = k_load_i32; return true;
    default: return false;
  }
}

bool flatten_struct(const schema::TypeInfo& info, u32 base, Vector<Op>& ops);

// What `hash_ref` does with a field of `type` at `offset`, as runs; false where it does anything a
// fixed run cannot say (a length, a pointer, a text).
bool flatten_ref(const schema::TypeRef& type, u32 offset, Vector<Op>& ops) {
  u8 load = 0;
  switch (type.kind) {
    case Kind::Vec2: push_op(ops, offset, k_load_u32, 2); return true;
    case Kind::Vec3: push_op(ops, offset, k_load_u32, 3); return true;
    case Kind::Vec4:
    case Kind::Quat: push_op(ops, offset, k_load_u32, 4); return true;
    case Kind::WorldPos:
    case Kind::DVec3: push_op(ops, offset, k_load_u64, 3); return true;
    case Kind::Id128:
      // `hash_ref` takes `hi` then `lo`, which is their order in the struct.
      static_assert(offsetof(Id128, hi) == 0 && offsetof(Id128, lo) == 8);
      push_op(ops, offset, k_load_u64, 2);
      return true;
    case Kind::Enum:
      if (type.type == nullptr) return true;  // `hash_ref` adds nothing for it either
      if (!scalar_load(type.type->enum_underlying, load)) return false;
      push_op(ops, offset, load, 1);
      return true;
    case Kind::Struct: return type.type == nullptr || flatten_struct(*type.type, offset, ops);
    default:
      if (!scalar_load(type.kind, load)) return false;
      push_op(ops, offset, load, 1);
      return true;
  }
}

bool flatten_struct(const schema::TypeInfo& info, u32 base, Vector<Op>& ops) {
  for (const schema::FieldInfo& field : info.fields) {
    if ((field.flags & schema::FieldFlag::transient) != 0) continue;  // as `hash_struct`
    if (!flatten_ref(field.type, base + field.offset, ops)) return false;
  }
  return true;
}

template <class T>
T load_as(const u8* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

// The flat program over one value: the words `hash_struct` would mix, in its order.
u64 hash_ops(const Op* op, const Op* end, const u8* p, u64 h) noexcept {
  for (; op != end; ++op) {
    const u8* q = p + op->offset;
    const u32 n = op->count;
    switch (op->load) {
      case k_load_u32:
        for (u32 k = 0; k < n; ++k)
          h = word(h, load_as<u32>(q + 4u * k));
        break;
      case k_load_u64:
        for (u32 k = 0; k < n; ++k)
          h = word(h, load_as<u64>(q + 8u * k));
        break;
      case k_load_u8:
        for (u32 k = 0; k < n; ++k)
          h = word(h, q[k]);
        break;
      case k_load_u16:
        for (u32 k = 0; k < n; ++k)
          h = word(h, load_as<u16>(q + 2u * k));
        break;
      case k_load_i8:
        for (u32 k = 0; k < n; ++k)
          h = word(h, static_cast<u64>(static_cast<i64>(load_as<i8>(q + k))));
        break;
      case k_load_i16:
        for (u32 k = 0; k < n; ++k)
          h = word(h, static_cast<u64>(static_cast<i64>(load_as<i16>(q + 2u * k))));
        break;
      case k_load_i32:
        for (u32 k = 0; k < n; ++k)
          h = word(h, static_cast<u64>(static_cast<i64>(load_as<i32>(q + 4u * k))));
        break;
      default:  // k_load_bool
        for (u32 k = 0; k < n; ++k)
          h = word(h, q[k] != 0 ? 1u : 0u);
        break;
    }
  }
  return h;
}

std::string entity_key(const Identity* identity, flecs::entity_t entity) {
  if (identity != nullptr && !identity->id.is_null()) {
    char hex[Id128::k_hex_length + 1];
    identity->id.to_hex(hex);
    return std::string(hex, Id128::k_hex_length);
  }
  return "e" + std::to_string(entity);
}

// ---- tags and relationship pairs (ecs.md, "What the structure column sees"; T196) -------------

constexpr u64 k_structure_tag = 0x5461670000000001ull;   // "Tag"
constexpr u64 k_structure_pair = 0x5061697200000002ull;  // "Pair"

// How an entity a tag or a pair names is named in the hash, as nothing that differs between two
// runs of one world: the component registry's qualified name, else the record's `Id128`, else
// flecs' symbol, else its name, else its flecs id — the last as an entity with no identity is
// seeded today. `text` (a capture's) says the same in words.
u64 stable_word(ecs_world_t* w, const ComponentRegistry& registry, flecs::entity_t identity,
                flecs::entity_t e, std::string* text) {
  if (e != 0 && ecs_is_alive(w, e)) {
    if (const ComponentType* type = registry.find(e); type != nullptr && type->info != nullptr) {
      const char* name = type->info->qualified_name;
      if (text != nullptr) *text = name;
      return word(1, hash_bytes(name, std::strlen(name)));
    }
    const auto* id = static_cast<const Identity*>(ecs_get_id(w, e, identity));
    if (id != nullptr && !id->id.is_null()) {
      if (text != nullptr) *text = entity_key(id, e);
      return word(word(2, id->id.hi), id->id.lo);
    }
    for (const char* name : {ecs_get_symbol(w, e), ecs_get_name(w, e)}) {
      if (name == nullptr || *name == 0) continue;
      if (text != nullptr) *text = name;
      return word(3, hash_bytes(name, std::strlen(name)));
    }
  }
  if (text != nullptr) *text = entity_key(nullptr, e);
  return word(4, e);
}

// Whether `id` of an archetype's type is a tag or a pair the structure column counts: every pair
// but a name (`(Identifier, Name)`, flecs' own), and every id with no data but `Identity`'s seed —
// a data component is a table of its own, or unhashed by choice. A transient tag only when the
// config hashes transient components.
bool structural(ecs_world_t* w, const ComponentRegistry& registry, ecs_id_t id, bool transient) {
  if (ECS_IS_PAIR(id)) return ECS_PAIR_FIRST(id) != static_cast<u32>(ecs_id(EcsIdentifier));
  if ((id & ECS_ID_FLAGS_MASK) != 0 || ecs_get_typeid(w, id) != 0) return false;
  const ComponentType* type = registry.find(static_cast<flecs::entity_t>(id));
  return type == nullptr || !type->transient || transient;
}

// One structural id as a word, and in words for a capture.
u64 structure_word(ecs_world_t* w, const ComponentRegistry& registry, flecs::entity_t identity,
                   ecs_id_t id, std::string* text) {
  if (!ECS_IS_PAIR(id)) {
    return mix64(word(k_structure_tag, stable_word(w, registry, identity, id, text)));
  }
  std::string first;
  std::string second;
  const u64 rel =
      stable_word(w, registry, identity, ecs_pair_first(w, id), text != nullptr ? &first : nullptr);
  // A value pair's second element is a number, not an entity.
  u64 target = 0;
  if (ECS_IS_VALUE_PAIR(id)) {
    target = word(5, ECS_PAIR_SECOND(id));
    if (text != nullptr) second = std::to_string(ECS_PAIR_SECOND(id));
  } else {
    target = stable_word(w, registry, identity, ecs_pair_second(w, id),
                         text != nullptr ? &second : nullptr);
  }
  if (text != nullptr) *text = "(" + first + ", " + second + ")";
  return mix64(word(word(k_structure_pair, rel), target));
}

}  // namespace

TickHash::TickHash(SimWorld& world, sim::SimScheduler& scheduler, sim::HashLog& log,
                   const TickHashConfig& config)
    : world_(&world), scheduler_(&scheduler), log_(&log), config_(config) {
  for (u32 phase = 0; phase < sim::k_phase_count; ++phase) {
    if (((config_.phases >> phase) & 1u) == 0) continue;
    sim::SystemDesc desc;
    desc.name = k_system_names[phase];
    desc.phase = static_cast<sim::TickPhase>(phase);
    desc.tiers = 0xFFu;
    // What it computes is reported, never read back into the world (ADR-0010).
    desc.determinism = sim::Determinism::Derived;
    desc.context = this;
    desc.end_tick = &TickHash::end_phase;
    scheduler_->add_system(desc);
  }
}

TickHash::~TickHash() {
  for (Table& table : tables_) {
    if (table.query != nullptr) ecs_query_fini(table.query);
  }
  if (identity_query_ != nullptr) ecs_query_fini(identity_query_);
}

bool TickHash::add_source(const TickHashSource& source) {
  if (frozen_ || source.name == nullptr || source.hash == nullptr) return false;
  sources_.push_back(source);
  return true;
}

bool TickHash::capture_at(u64 tick, sim::TickPhase phase, std::string_view table,
                          sim::Capture* out) {
  if (out == nullptr || ((config_.phases >> static_cast<u32>(phase)) & 1u) == 0) return false;
  Request request;
  request.tick = tick;
  request.phase = phase;
  request.name = std::string(table);
  request.out = out;
  if (frozen_) {
    request.table = ~0u;
    for (u32 i = 0; i <= tables_.size(); ++i) {  // the components, then the structure column
      if (names_[i] == table) request.table = i;
    }
    if (request.table == ~0u) return false;
  }
  requests_.push_back(std::move(request));
  return true;
}

void TickHash::freeze() {
  if (frozen_) return;
  frozen_ = true;
  flecs::world& w = world_->world();
  (void)identity_map(w);  // registers `Identity` with its name, if nothing has yet
  identity_ = w.component<Identity>().id();

  const ComponentRegistry& registry = components(w);
  components_seen_ = registry.size();
  Vector<const ComponentType*> chosen;
  for (const ComponentType& type : registry.all()) {
    if (type.size == 0) continue;  // a tag carries no state
    if (type.info == nullptr) {
      ++stats_.unhashed;
      continue;
    }
    if (type.transient && !config_.transient) continue;
    chosen.push_back(&type);
  }
  std::sort(chosen.begin(), chosen.end(), [](const ComponentType* a, const ComponentType* b) {
    return std::strcmp(a->info->qualified_name, b->info->qualified_name) < 0;
  });
  for (const ComponentType* type : chosen) {
    Table table;
    table.info = type->info;
    table.component = type->id;
    table.size = type->size;
    table.name_hash =
        hash_bytes(type->info->qualified_name, std::strlen(type->info->qualified_name));
    // Its flat program, where it has one (and the config does not ask for reflection alone).
    table.op_first = ops_.size();
    if (!config_.reflected && flatten_struct(*type->info, 0, ops_)) {
      table.fast = true;
      table.op_count = ops_.size() - table.op_first;
      ++stats_.fast_tables;
    } else {
      ops_.resize(table.op_first);
    }
    ecs_query_desc_t desc = {};
    desc.terms[0].id = type->id;
    desc.terms[0].inout = EcsIn;
    desc.terms[1].id = identity_;
    desc.terms[1].inout = EcsIn;
    desc.terms[1].oper = EcsOptional;
    desc.cache_kind = EcsQueryCacheAuto;
    table.query = ecs_query_init(w.c_ptr(), &desc);
    tables_.push_back(table);
    names_.push_back(type->info->qualified_name);
  }
  // The structure column: the tags and relationship pairs of every entity the component tables or
  // an `Identity` show (ecs.md, "What the structure column sees").
  names_.push_back(k_tick_hash_structure);
  structure_name_hash_ = hash_bytes(k_tick_hash_structure, std::strlen(k_tick_hash_structure));
  {
    ecs_query_desc_t desc = {};
    desc.terms[0].id = identity_;
    desc.terms[0].inout = EcsIn;
    desc.cache_kind = EcsQueryCacheAuto;
    identity_query_ = ecs_query_init(w.c_ptr(), &desc);
  }
  for (const TickHashSource& source : sources_)
    names_.push_back(source.name);
  row_.resize(names_.size());

  log_->set_tables(std::span<const std::string>(names_.data(), names_.size()), config_.phases);
  if (config_.reserve_ticks > 0) log_->reserve(config_.reserve_ticks);

  // A request made before the set was fixed finds its table now, or is dropped.
  for (u32 i = 0; i < requests_.size();) {
    requests_[i].table = ~0u;
    for (u32 t = 0; t <= tables_.size(); ++t) {
      if (names_[t] == requests_[i].name) requests_[i].table = t;
    }
    if (requests_[i].table == ~0u) {
      requests_.erase(requests_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

// One run of a table's entities: the sum of each entity's value hashed from its seed, through the
// table's flat program where it has one and through reflection where it does not.
u64 TickHash::hash_run(const Run& run) const noexcept {
  const Table& table = tables_[run.table];
  const auto* ids = static_cast<const Identity*>(run.ids);
  const usize stride = run.values_self ? table.size : 0u;
  const auto seed_of = [&](u32 i) {
    const Identity* identity = ids == nullptr ? nullptr : (run.ids_self ? ids + i : ids);
    return identity != nullptr && !identity->id.is_null()
               ? word(word(k_hash_seed, identity->id.hi), identity->id.lo)
               : word(k_hash_seed ^ 1u, run.entities[i]);
  };
  u64 sum = 0;
  if (table.fast) {
    const Op* first = ops_.data() + table.op_first;
    const Op* end = first + table.op_count;
    for (u32 i = 0; i < run.count; ++i)
      sum += mix64(hash_ops(first, end, run.values + i * stride, seed_of(i)));
  } else {
    for (u32 i = 0; i < run.count; ++i)
      sum += mix64(hash_struct(*table.info, run.values + i * stride, seed_of(i)));
  }
  return sum;
}

void TickHash::gather_runs(u32 grain) {
  ecs_world_t* w = world_->world().c_ptr();
  runs_.clear();
  for (u32 t = 0; t < tables_.size(); ++t) {
    const Table& table = tables_[t];
    ecs_iter_t it = ecs_query_iter(w, table.query);
    while (ecs_query_next(&it)) {
      Run run;
      run.table = t;
      run.values = static_cast<const u8*>(ecs_field_w_size(&it, table.size, 0));
      run.values_self = ecs_field_is_self(&it, 0);
      run.ids = ecs_field_is_set(&it, 1) ? ecs_field_w_size(&it, sizeof(Identity), 1) : nullptr;
      run.ids_self = run.ids != nullptr && ecs_field_is_self(&it, 1);
      // The world is not changed while a phase is hashed, so these pointers hold until it ends.
      for (u32 at = 0; at < static_cast<u32>(it.count); at += grain) {
        Run part = run;
        part.count = std::min<u32>(grain, static_cast<u32>(it.count) - at);
        part.values = run.values + (run.values_self ? usize{at} * table.size : 0u);
        part.ids = run.ids == nullptr || !run.ids_self ? run.ids
                                                       : static_cast<const Identity*>(run.ids) + at;
        part.entities = it.entities + at;
        runs_.push_back(part);
      }
    }
  }
}

u64 TickHash::hash_table(const Table& table) {
  // One table on the calling thread: its runs as they come, never gathered.
  ecs_world_t* w = world_->world().c_ptr();
  const u32 t = static_cast<u32>(&table - tables_.data());
  u64 sum = 0;
  u64 count = 0;
  ecs_iter_t it = ecs_query_iter(w, table.query);
  while (ecs_query_next(&it)) {
    Run run;
    run.table = t;
    run.count = static_cast<u32>(it.count);
    run.values = static_cast<const u8*>(ecs_field_w_size(&it, table.size, 0));
    run.values_self = ecs_field_is_self(&it, 0);
    run.ids = ecs_field_is_set(&it, 1) ? ecs_field_w_size(&it, sizeof(Identity), 1) : nullptr;
    run.ids_self = run.ids != nullptr && ecs_field_is_self(&it, 1);
    run.entities = it.entities;
    sum += hash_run(run);
    count += run.count;
  }
  stats_.entities += count;
  return hash_combine(hash_combine(sum, count), table.name_hash);
}

void TickHash::hash_into(sim::TickPhase phase, u64* out) {
  if (config_.jobs == nullptr) {
    for (u32 t = 0; t < tables_.size(); ++t)
      out[t] = hash_table(tables_[t]);
  } else {
    // **In parallel, to the same values** (ecs.md, "The tick hash's fast path"): the runs are
    // gathered here, on the one thread that may walk the world's queries, hashed a run a job, and
    // summed into their tables here again. A table's hash is a sum of its entities' words mod 2^64,
    // which no split and no order of addition changes, and each table's is written to its own
    // column of the row, so the order the jobs ran in is in no value; the runs are even summed in
    // the order they were gathered, which is the serial path's.
    gather_runs(config_.grain == 0 ? 1u : config_.grain);
    config_.jobs->parallel_for(jobs::Pool::Performance, runs_.size(), 1,
                               [this](u32 begin, u32 end) {
                                 for (u32 r = begin; r < end; ++r)
                                   runs_[r].sum = hash_run(runs_[r]);
                               });
    for (u32 t = 0; t < tables_.size(); ++t)
      out[t] = 0;
    Vector<u64>& counts = run_counts_;
    counts.assign(tables_.size(), 0);
    for (const Run& run : runs_) {
      out[run.table] += run.sum;
      counts[run.table] += run.count;
    }
    for (u32 t = 0; t < tables_.size(); ++t) {
      stats_.entities += counts[t];
      out[t] = hash_combine(hash_combine(out[t], counts[t]), tables_[t].name_hash);
    }
  }
  out[tables_.size()] = hash_structure();
  for (u32 s = 0; s < sources_.size(); ++s)
    out[tables_.size() + 1 + s] = sources_[s].hash(sources_[s].context, phase);
}

void TickHash::gather_structure_tables() {
  // Every archetype an entity the hash sees lives in, once: the tables the component queries and
  // the `Identity` query match, deduplicated by address (an address is in no value: the column is a
  // sum over entities).
  ecs_world_t* w = world_->world().c_ptr();
  structure_tables_.clear();
  const auto add = [&](ecs_query_t* query) {
    ecs_iter_t it = ecs_query_iter(w, query);
    while (ecs_query_next(&it)) {
      if (it.table != nullptr) structure_tables_.push_back(it.table);
    }
  };
  for (const Table& table : tables_)
    add(table.query);
  add(identity_query_);
  std::sort(structure_tables_.begin(), structure_tables_.end());
  structure_tables_.erase(std::unique(structure_tables_.begin(), structure_tables_.end()),
                          structure_tables_.end());
}

u64 TickHash::table_structure(ecs_table_t* table) const {
  ecs_world_t* w = world_->world().c_ptr();
  const ComponentRegistry& registry = components(world_->world());
  const ecs_type_t* type = ecs_table_get_type(table);
  u64 sum = 0;
  for (i32 i = 0; type != nullptr && i < type->count; ++i) {
    const ecs_id_t id = type->array[i];
    if (id == identity_ || !structural(w, registry, id, config_.transient)) continue;
    sum += structure_word(w, registry, identity_, id, nullptr);
  }
  return sum;
}

u64 TickHash::hash_structure() {
  // **Tags and relationship pairs** (roadmap T196): an archetype's tags and pairs are its type, so
  // each table's are hashed once, as a sum over its ids that no order of the type changes, and each
  // entity in it adds that sum mixed with its seed — the entity's `Id128`, or its flecs id when it
  // has none, as the component tables seed it. A table with neither adds nothing.
  gather_structure_tables();
  ecs_world_t* w = world_->world().c_ptr();
  u64 sum = 0;
  u64 count = 0;
  for (ecs_table_t* table : structure_tables_) {
    const u64 structure = table_structure(table);
    if (structure == 0) continue;
    const i32 rows = ecs_table_count(table);
    const flecs::entity_t* entities = ecs_table_entities(table);
    const auto* ids = static_cast<const Identity*>(ecs_table_get_id(w, table, identity_, 0));
    for (i32 r = 0; r < rows; ++r) {
      const Identity* identity = ids == nullptr ? nullptr : ids + r;
      const u64 seed = identity != nullptr && !identity->id.is_null()
                           ? word(word(k_hash_seed, identity->id.hi), identity->id.lo)
                           : word(k_hash_seed ^ 1u, entities[r]);
      sum += mix64(word(seed, structure));
    }
    count += static_cast<u64>(rows);
  }
  return hash_combine(hash_combine(sum, count), structure_name_hash_);
}

void TickHash::capture_structure(sim::Capture& out) {
  gather_structure_tables();
  ecs_world_t* w = world_->world().c_ptr();
  const ComponentRegistry& registry = components(world_->world());
  struct Entry {
    std::string id;
    JsonValue value;
  };
  Vector<Entry> entries;
  for (ecs_table_t* table : structure_tables_) {
    if (table_structure(table) == 0) continue;
    // The table's tags and pairs in words, sorted, which every entity of it shares.
    Vector<std::string> words;
    const ecs_type_t* type = ecs_table_get_type(table);
    for (i32 i = 0; i < type->count; ++i) {
      const ecs_id_t id = type->array[i];
      if (id == identity_ || !structural(w, registry, id, config_.transient)) continue;
      std::string text;
      (void)structure_word(w, registry, identity_, id, &text);
      words.push_back(std::move(text));
    }
    std::sort(words.begin(), words.end());
    JsonValue value = JsonValue::array();
    for (std::string& text : words)
      value.push_back(JsonValue(std::move(text)));
    const i32 rows = ecs_table_count(table);
    const flecs::entity_t* entities = ecs_table_entities(table);
    const auto* ids = static_cast<const Identity*>(ecs_table_get_id(w, table, identity_, 0));
    for (i32 r = 0; r < rows; ++r) {
      Entry entry;
      entry.id = entity_key(ids == nullptr ? nullptr : ids + r, entities[r]);
      entry.value = value;
      entries.push_back(std::move(entry));
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.id < b.id; });
  out.entities = JsonValue::array();
  for (Entry& entry : entries) {
    JsonValue item = JsonValue::object();
    item.set("id", entry.id);
    item.set("value", std::move(entry.value));
    out.entities.push_back(std::move(item));
  }
}

void TickHash::hash_now(sim::TickPhase phase, Vector<u64>& out) {
  freeze();
  out.resize(names_.size());
  hash_into(phase, out.data());
}

void TickHash::capture(const Table& table, sim::Capture& out) {
  ecs_world_t* w = world_->world().c_ptr();
  struct Entry {
    std::string id;
    JsonValue value;
  };
  Vector<Entry> entries;
  ecs_iter_t it = ecs_query_iter(w, table.query);
  while (ecs_query_next(&it)) {
    const auto* base = static_cast<const u8*>(ecs_field_w_size(&it, table.size, 0));
    const bool self = ecs_field_is_self(&it, 0);
    const Identity* ids =
        ecs_field_is_set(&it, 1)
            ? static_cast<const Identity*>(ecs_field_w_size(&it, sizeof(Identity), 1))
            : nullptr;
    const bool ids_self = ids != nullptr && ecs_field_is_self(&it, 1);
    for (i32 i = 0; i < it.count; ++i) {
      Entry entry;
      entry.id = entity_key(ids == nullptr ? nullptr : (ids_self ? ids + i : ids), it.entities[i]);
      (void)schema::to_json(*table.info, base + (self ? static_cast<usize>(i) * table.size : 0u),
                            entry.value);
      entries.push_back(std::move(entry));
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.id < b.id; });
  out.entities = JsonValue::array();
  for (Entry& entry : entries) {
    JsonValue item = JsonValue::object();
    item.set("id", entry.id);
    item.set("value", std::move(entry.value));
    out.entities.push_back(std::move(item));
  }
}

void TickHash::end_phase(sim::SystemContext& context) {
  auto* self = static_cast<TickHash*>(context.context);
  self->freeze();
  const u32 seen = components(self->world_->world()).size();
  if (seen > self->components_seen_) {
    self->stats_.late += seen - self->components_seen_;
    self->components_seen_ = seen;
  }
  self->hash_into(context.phase, self->row_.data());
  if (self->log_->append(context.tick.value, context.phase,
                         std::span<const u64>(self->row_.data(), self->row_.size()))) {
    ++self->stats_.rows;
  } else {
    ++self->stats_.refused;
  }
  for (u32 i = 0; i < self->requests_.size();) {
    Request& request = self->requests_[i];
    if (request.tick != context.tick.value || request.phase != context.phase) {
      ++i;
      continue;
    }
    request.out->tick = request.tick;
    request.out->phase = request.phase;
    request.out->table = self->names_[request.table];
    if (request.table == self->tables_.size()) {
      self->capture_structure(*request.out);
    } else {
      self->capture(self->tables_[request.table], *request.out);
    }
    ++self->stats_.captures;
    self->requests_.erase(self->requests_.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

bool tick_hash_both_ways(const schema::TypeInfo& info, const void* value, u64 seed, u64& reflected,
                         u64& fast) {
  reflected = hash_struct(info, value, seed);
  Vector<Op> ops;
  if (!flatten_struct(info, 0, ops)) return false;
  fast = hash_ops(ops.data(), ops.data() + ops.size(), static_cast<const u8*>(value), seed);
  return true;
}

TickHashMemory TickHash::memory() const noexcept {
  TickHashMemory m;
  m.tables = static_cast<u32>(names_.size());
  usize names = 0;
  for (const std::string& name : names_)
    names += name.capacity();
  m.per_table = (sizeof(Table) + sizeof(std::string) + sizeof(u64)) +
                (names_.empty() ? 0 : names / names_.size());
  m.per_entity = 0;
  u32 phases = 0;
  for (u32 p = 0; p < sim::k_phase_count; ++p)
    phases += (config_.phases >> p) & 1u;
  m.per_tick = phases * (sizeof(sim::HashLogRow) + sizeof(u64) * names_.size());
  m.log = log_->memory_bytes();
  return m;
}

}  // namespace engine::ecs
