#include <domain/ecs/tick_hash.h>

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace engine::ecs {

namespace {

using schema::Kind;

// One name per phase, so a schedule listing says which of the hash's systems is which.
constexpr const char* k_system_names[sim::k_phase_count] = {
    "ecs.tick_hash.input",   "ecs.tick_hash.events_in",    "ecs.tick_hash.lod",
    "ecs.tick_hash.systems", "ecs.tick_hash.physics",      "ecs.tick_hash.post_physics",
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
    case Kind::Enum:
      return type.type != nullptr ? hash_kind(type.type->enum_underlying, p, h) : h;
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

std::string entity_key(const Identity* identity, flecs::entity_t entity) {
  if (identity != nullptr && !identity->id.is_null()) {
    char hex[Id128::k_hex_length + 1];
    identity->id.to_hex(hex);
    return std::string(hex, Id128::k_hex_length);
  }
  return "e" + std::to_string(entity);
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
    for (u32 i = 0; i < tables_.size(); ++i) {
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
    table.name_hash = hash_bytes(type->info->qualified_name, std::strlen(type->info->qualified_name));
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
  for (const TickHashSource& source : sources_)
    names_.push_back(source.name);
  row_.resize(names_.size());

  log_->set_tables(std::span<const std::string>(names_.data(), names_.size()), config_.phases);
  if (config_.reserve_ticks > 0) log_->reserve(config_.reserve_ticks);

  // A request made before the set was fixed finds its table now, or is dropped.
  for (u32 i = 0; i < requests_.size();) {
    requests_[i].table = ~0u;
    for (u32 t = 0; t < tables_.size(); ++t) {
      if (names_[t] == requests_[i].name) requests_[i].table = t;
    }
    if (requests_[i].table == ~0u) {
      requests_.erase(requests_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

u64 TickHash::hash_table(const Table& table) {
  ecs_world_t* w = world_->world().c_ptr();
  u64 sum = 0;
  u64 count = 0;
  ecs_iter_t it = ecs_query_iter(w, table.query);
  while (ecs_query_next(&it)) {
    const auto* base = static_cast<const u8*>(ecs_field_w_size(&it, table.size, 0));
    const bool self = ecs_field_is_self(&it, 0);
    const Identity* ids = ecs_field_is_set(&it, 1)
                              ? static_cast<const Identity*>(ecs_field_w_size(&it, sizeof(Identity), 1))
                              : nullptr;
    const bool ids_self = ids != nullptr && ecs_field_is_self(&it, 1);
    for (i32 i = 0; i < it.count; ++i) {
      const Identity* identity = ids == nullptr ? nullptr : (ids_self ? ids + i : ids);
      const u64 seed = identity != nullptr && !identity->id.is_null()
                           ? word(word(k_hash_seed, identity->id.hi), identity->id.lo)
                           : word(k_hash_seed ^ 1u, it.entities[i]);
      const void* value = base + (self ? static_cast<usize>(i) * table.size : 0u);
      sum += mix64(hash_struct(*table.info, value, seed));
      ++count;
    }
  }
  stats_.entities += count;
  return hash_combine(hash_combine(sum, count), table.name_hash);
}

void TickHash::hash_into(sim::TickPhase phase, u64* out) {
  for (u32 t = 0; t < tables_.size(); ++t)
    out[t] = hash_table(tables_[t]);
  for (u32 s = 0; s < sources_.size(); ++s)
    out[tables_.size() + s] = sources_[s].hash(sources_[s].context, phase);
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
    const Identity* ids = ecs_field_is_set(&it, 1)
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
    self->capture(self->tables_[request.table], *request.out);
    ++self->stats_.captures;
    self->requests_.erase(self->requests_.begin() + static_cast<std::ptrdiff_t>(i));
  }
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
