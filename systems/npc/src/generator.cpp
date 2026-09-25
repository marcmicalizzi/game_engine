#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>
#include <systems/npc/generator.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

#include <schemas/npc.h>
#include <string>

namespace engine::npc {

namespace {

// Keys that name what a draw is for, so two draws of one record never share a stream.
constexpr u64 k_key_x = 1;
constexpr u64 k_key_z = 2;
constexpr u64 k_key_routine = 3;
constexpr u64 k_key_home = 4;
constexpr u64 k_key_job = 5;
constexpr u64 k_key_service = 6;
constexpr u64 k_key_leisure = 7;

// "npc-plac" and "npc-resi": the ids' high words, so a generated id is recognisable in a document.
constexpr u64 k_place_hi = 0x6E70632D706C6163ull;
constexpr u64 k_resident_hi = 0x6E70632D72657369ull;

u64 stream(u64 seed, u64 kind, u64 index, u64 key) noexcept {
  return mix64(
      hash_combine(hash_combine(hash_combine(hash_combine(k_hash_seed, seed), kind), index), key));
}

u32 pick(u64 h, u32 count) noexcept { return count == 0 ? 0u : static_cast<u32>(h % count); }

// A coordinate on the quarter-metre grid in [lo, hi).
f64 coordinate(u64 h, f64 lo, f64 hi) noexcept {
  const f64 span = hi > lo ? hi - lo : 0.0;
  const u64 steps = static_cast<u64>(span * 4.0);
  return steps == 0 ? lo : lo + static_cast<f64>(h % steps) * 0.25;
}

struct PlaceSet {
  u32 homes = 0;
  u32 workplaces = 0;
  u32 services = 0;
  u32 leisure = 0;
  u32 total() const noexcept { return homes + workplaces + services + leisure; }
  u32 first(PlaceRole role) const noexcept {
    switch (role) {
      case PlaceRole::Home: return 0;
      case PlaceRole::Work: return homes;
      case PlaceRole::Service: return homes + workplaces;
      case PlaceRole::Leisure: return homes + workplaces + services;
    }
    return 0;
  }
  PlaceRole role_of(u32 index) const noexcept {
    if (index < homes) return PlaceRole::Home;
    if (index < homes + workplaces) return PlaceRole::Work;
    if (index < homes + workplaces + services) return PlaceRole::Service;
    return PlaceRole::Leisure;
  }
};

Vec3 place_position(const GeneratorParams& p, u32 index) noexcept {
  const f64 x = coordinate(stream(p.seed, 'P', index, k_key_x), p.min_x, p.max_x);
  const f64 z = coordinate(stream(p.seed, 'P', index, k_key_z), p.min_z, p.max_z);
  return Vec3{static_cast<f32>(x), 0.0f, static_cast<f32>(z)};
}

doc::ObjectRecord place_record(const GeneratorParams& p, const PlaceSet& set, u32 index) {
  Place place;
  const PlaceRole role = set.role_of(index);
  static const char* const k_names[4] = {"home", "workplace", "service", "leisure"};
  place.name =
      std::string(k_names[static_cast<u32>(role)]) + " " + std::to_string(index - set.first(role));
  place.position = place_position(p, index);
  place.role = role;
  place.capacity = role == PlaceRole::Home ? 4u : 40u;
  doc::ObjectRecord record;
  record.id = place_id(p.seed, index);
  record.type = k_place_type;
  const JsonValue all = schema::to_json(place);
  for (const char* key : {"name", "position", "role", "capacity"}) {
    const JsonValue* v = all.find(key);
    if (v != nullptr) record.properties.insert_or_assign(key, *v);
  }
  return record;
}

Routine choose_routine(const GeneratorParams& p, u32 index) noexcept {
  u64 total = 0;
  for (u32 w : p.mix)
    total += w;
  if (total == 0) return Routine::Idle;
  u64 r = stream(p.seed, 'R', index, k_key_routine) % total;
  for (u32 i = 0; i < k_routine_count; ++i) {
    if (r < p.mix[i]) return static_cast<Routine>(i);
    r -= p.mix[i];
  }
  return Routine::Idle;
}

doc::ObjectRecord resident_record(const GeneratorParams& p, const PlaceSet& set,
                                  const Vector<Vec3>& positions, u32 index) {
  Resident r;
  const Id128 id = resident_id(p.seed, index);
  r.routine = choose_routine(p, index);
  const u32 home =
      set.first(PlaceRole::Home) + pick(stream(p.seed, 'R', index, k_key_home), set.homes);
  u32 job = k_no_place;
  if (r.routine == Routine::Shopkeeper) {
    job = set.first(PlaceRole::Service) + pick(stream(p.seed, 'R', index, k_key_job), set.services);
  } else if (r.routine == Routine::DayWorker || r.routine == Routine::NightWorker ||
             r.routine == Routine::Student) {
    job = set.first(PlaceRole::Work) + pick(stream(p.seed, 'R', index, k_key_job), set.workplaces);
  }
  const u32 service =
      set.first(PlaceRole::Service) + pick(stream(p.seed, 'R', index, k_key_service), set.services);
  const u32 leisure =
      set.first(PlaceRole::Leisure) + pick(stream(p.seed, 'R', index, k_key_leisure), set.leisure);
  r.home = place_id(p.seed, home);
  if (job != k_no_place) r.job = place_id(p.seed, job);
  r.service = place_id(p.seed, service);
  r.leisure = place_id(p.seed, leisure);

  // The state the world would compute for this resident at the layer's time: the same closed form,
  // the same variation, so materializing it there changes nothing.
  r.clock_offset = p.clock_offset_us;
  const RoutinePoint point =
      routine_at_offset(draw_variation(r.routine, p.seed, id), p.time_us, p.clock_offset_us);
  u32 at = home;
  switch (point.place) {
    case PlaceRole::Home: at = home; break;
    case PlaceRole::Work: at = job != k_no_place ? job : home; break;
    case PlaceRole::Service: at = service; break;
    case PlaceRole::Leisure: at = leisure; break;
  }
  r.position = positions[at];
  r.state = point.state;
  r.next_event = point.end_us;

  doc::ObjectRecord record;
  record.id = id;
  record.type = k_resident_type;
  const JsonValue all = schema::to_json(r);
  // What differs from the type's defaults and what the partition reads, and nothing else: 10^5
  // residents stay small on disk.
  for (const char* key : {"position", "home", "job", "service", "leisure", "routine", "state",
                          "next_event", "clock_offset"}) {
    if (job == k_no_place && std::string_view(key) == "job") continue;
    if (p.clock_offset_us == 0 && std::string_view(key) == "clock_offset") continue;
    const JsonValue* v = all.find(key);
    if (v != nullptr) record.properties.insert_or_assign(key, *v);
  }
  return record;
}

bool read_u32(const JsonValue& v, u32& out) {
  u64 n = 0;
  if (!v.get_u64(n) || n > 0xFFFF'FFFFull) return false;
  out = static_cast<u32>(n);
  return true;
}

}  // namespace

Id128 place_id(u64 seed, u32 index) noexcept {
  return Id128::from_parts(k_place_hi, (mix64(seed) & 0xFFFF'FFFF'0000'0000ull) | index);
}

Id128 resident_id(u64 seed, u32 index) noexcept {
  return Id128::from_parts(k_resident_hi, (mix64(seed) & 0xFFFF'FFFF'0000'0000ull) | index);
}

GeneratorParams resolved(const GeneratorParams& params) noexcept {
  GeneratorParams p = params;
  const auto at_least_one = [](u32 v) { return v > 0 ? v : 1u; };
  if (p.homes == 0) p.homes = at_least_one(p.residents / 4);
  if (p.workplaces == 0) p.workplaces = at_least_one(p.residents / 40);
  if (p.services == 0) p.services = at_least_one(p.residents / 50);
  if (p.leisure == 0) p.leisure = at_least_one(p.residents / 100);
  return p;
}

bool parse_params(const JsonValue& json, GeneratorParams& out, std::string& error) {
  if (!json.is_object()) {
    error = "the parameters are not a JSON object";
    return false;
  }
  GeneratorParams p;
  const JsonValue::Object& fields = json.as_object();
  for (usize i = 0; i < fields.size(); ++i) {
    const std::string_view key = fields.key_at(static_cast<u32>(i));
    const JsonValue& v = fields.value_at(static_cast<u32>(i));
    bool ok = true;
    if (key == "seed") {
      ok = v.get_u64(p.seed);
    } else if (key == "residents") {
      ok = read_u32(v, p.residents);
    } else if (key == "places") {
      ok = v.is_object();
      for (usize j = 0; ok && j < v.size(); ++j) {
        const std::string_view role = v.as_object().key_at(static_cast<u32>(j));
        u32* target = role == "homes"        ? &p.homes
                      : role == "workplaces" ? &p.workplaces
                      : role == "services"   ? &p.services
                      : role == "leisure"    ? &p.leisure
                                             : nullptr;
        ok = target != nullptr && read_u32(v.as_object().value_at(static_cast<u32>(j)), *target);
      }
    } else if (key == "extent") {
      f64 e[4] = {};
      ok = v.is_array() && v.size() == 4;
      for (usize j = 0; ok && j < 4; ++j)
        ok = v[j].get_f64(e[j]);
      ok = ok && e[2] > e[0] && e[3] > e[1];
      if (ok) {
        p.min_x = e[0];
        p.min_z = e[1];
        p.max_x = e[2];
        p.max_z = e[3];
      }
    } else if (key == "tile_size") {
      ok = v.get_f64(p.tile_size) && p.tile_size > 0.0;
    } else if (key == "layer") {
      std::string_view name;
      ok = v.get_string(name) && !name.empty();
      if (ok) p.layer = std::string(name);
    } else if (key == "time_us") {
      ok = v.get_i64(p.time_us);
    } else if (key == "clock_offset_us") {
      ok = v.get_i64(p.clock_offset_us);
    } else if (key == "mix") {
      ok = v.is_object();
      for (usize j = 0; ok && j < v.size(); ++j) {
        bool found = false;
        for (u32 r = 0; r < k_routine_count; ++r) {
          if (v.as_object().key_at(static_cast<u32>(j)) == routine_tables()[r].name) {
            found = read_u32(v.as_object().value_at(static_cast<u32>(j)), p.mix[r]);
          }
        }
        ok = found;
      }
    } else {
      error = "unknown parameter '" + std::string(key) + "'";
      return false;
    }
    if (!ok) {
      error = "parameter '" + std::string(key) + "' is not what it should be";
      return false;
    }
  }
  if (p.residents == 0) {
    error = "no residents to generate";
    return false;
  }
  out = p;
  return true;
}

doc::Layer generate_layer(const GeneratorParams& params, jobs::JobSystem* jobs,
                          GeneratorStats* stats) {
  const time::Stopwatch clock;
  const GeneratorParams p = resolved(params);
  PlaceSet set;
  set.homes = p.homes;
  set.workplaces = p.workplaces;
  set.services = p.services;
  set.leisure = p.leisure;

  Vector<Vec3> positions;
  positions.resize(set.total());
  for (u32 i = 0; i < set.total(); ++i)
    positions[i] = place_position(p, i);

  Vector<doc::ObjectRecord> records;
  records.resize(set.total() + p.residents);
  const u32 places = set.total();
  const auto build = [&](u32 begin, u32 end) {
    for (u32 i = begin; i < end; ++i) {
      records[i] =
          i < places ? place_record(p, set, i) : resident_record(p, set, positions, i - places);
    }
  };
  if (jobs != nullptr) {
    jobs->parallel_for(jobs::Pool::Performance, records.size(), 1024,
                       [&build](u32 begin, u32 end) { build(begin, end); });
  } else {
    build(0, records.size());
  }

  doc::Layer layer(p.layer, doc::LayerRole::Base);
  doc::LayerPartition partition;
  partition.property = "position";
  partition.tile_size = p.tile_size;
  layer.set_partition(partition);
  // The layer keeps its records by id, so the order they are set in is not the order they are held
  // or written in: the index order above is only this loop's.
  for (u32 i = 0; i < records.size(); ++i)
    layer.set(std::move(records[i]));
  if (stats != nullptr) {
    stats->places = places;
    stats->residents = p.residents;
    stats->tiles = doc::tile_count(layer);
    stats->ms = static_cast<f64>(clock.elapsed_ns()) / 1.0e6;
  }
  return layer;
}

bool generate_document(const io::Vfs& vfs, std::string_view dir, const GeneratorParams& params,
                       jobs::JobSystem* jobs, std::string& error, GeneratorStats* stats) {
  doc::Document document;
  doc::DocumentManifest manifest;
  if (doc::DocumentStore::exists(vfs, dir)) {
    if (!doc::DocumentStore::load(vfs, dir, document, manifest, &error)) return false;
  } else if (!doc::DocumentStore::create(vfs, dir, "residents", document, manifest, &error)) {
    return false;
  }
  if (document.find_layer(params.layer) >= 0) {
    error = "the document already has a layer '" + params.layer + "'";
    return false;
  }
  // The residents' layer is the edit layer, so the write-back's `SetProperty` lands in the layer
  // that defines the record and the partition files a resident that moved under its new tile.
  const u32 index = document.add_layer(generate_layer(params, jobs, stats));
  document.set_edit_layer(index);
  document.rebuild_index();
  document.mark_all_dirty();
  return doc::DocumentStore::save(vfs, dir, document, manifest, &error);
}

}  // namespace engine::npc
