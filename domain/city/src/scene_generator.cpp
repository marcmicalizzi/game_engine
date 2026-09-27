// Island City as a placement generator (scene_generator.h; docs/subsystems/scene_gen.md). The
// whole read is `engine-content city fragment`'s content — the same proxies, placed as the fragment
// places its instances — made in memory; a tile is the plan's per-tile query.
#include <core/jobs/job_system.h>
#include <core/schema/json_reflect.h>
#include <domain/city/city.h>
#include <domain/city/scene_generator.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <schemas/city.h>
#include <string>
#include <system_error>

namespace engine::city {

namespace {

// The entry, read once: the plan, where its proxies' meshes are, and the rectangle it covers.
struct CityState {
  CityEntry entry;
  std::string where;
  Plan plan;
  std::string mesh_dir;
  Vector<scene_gen::PlacementMesh> meshes;  // the twelve boxes, in `ProxyMesh` order
  Rect bounds;  // every lot, street and the coastline: a tile outside it holds nothing
};

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a city entry") : text;
}

void grow(Rect& r, i32 x, i32 z) noexcept {
  r.x0 = std::min(r.x0, x);
  r.z0 = std::min(r.z0, z);
  r.x1 = std::max(r.x1, x);
  r.z1 = std::max(r.z1, z);
}

// Everything the plan has anything on, closed: the coastline, every lot and every street node.
Rect plan_bounds(const Plan& plan) noexcept {
  Rect r{std::numeric_limits<i32>::max(), std::numeric_limits<i32>::max(),
         std::numeric_limits<i32>::min(), std::numeric_limits<i32>::min()};
  for (u32 i = 0; i + 1 < plan.coastline.size(); i += 2)
    grow(r, plan.coastline[i], plan.coastline[i + 1]);
  for (const Lot& lot : plan.lots) {
    grow(r, lot.rect.x0, lot.rect.z0);
    grow(r, lot.rect.x1, lot.rect.z1);
  }
  for (const Node& n : plan.nodes)
    grow(r, n.x, n.z);
  return r;
}

// The detail a streamed tile's ring draws: the city's LOD policy at the ring's inner radius, in
// tiles — rooms in the innermost ring.
Stage ring_stage(const scene_gen::Context& context) noexcept {
  if (context.world == nullptr || context.ring == 0 || context.world->rings.empty())
    return stage_for_distance(0.0f);
  const u32 inner = std::min<u32>(context.ring, context.world->rings.size()) - 1;
  return stage_for_distance(context.world->rings[inner].radius);
}

void place(const Vector<Proxy>& proxies, scene_gen::Placements& out) {
  out.instances.reserve(out.instances.size() + proxies.size());
  for (const Proxy& p : proxies) {
    scene_gen::Placement placement;
    placement.mesh = static_cast<u32>(p.mesh);
    placement.transform.position = proxy_translation(p);
    placement.transform.scale = proxy_scale(p);
    out.instances.push_back(placement);
  }
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  auto city = std::make_unique<CityState>();
  city->where = std::string(context.where);
  const std::string& where = city->where;
  schema::ReadContext ctx;
  if (!schema::from_json(city->entry, params, ctx) || !ctx.ok())
    return fail(where + ": " + schema_errors(ctx));
  const CityEntry& entry = city->entry;
  if (entry.plan.empty()) return fail(where + " names no plan");
  if (entry.tile.has_value() && entry.district.has_value())
    return fail(where + " names both a tile and a district; name one");
  const std::string dir(context.dir);
  const auto resolve = [&](const std::string& p) {
    return io::is_absolute_path(p) || dir.empty() ? p : io::join_path(dir, p);
  };
  std::string plan_path = resolve(entry.plan);
  std::error_code ec;
  if (std::filesystem::is_directory(std::filesystem::path(plan_path), ec))
    plan_path = io::join_path(plan_path, "plan.json");
  std::string why;
  if (!read_plan_file(plan_path, city->plan, why)) return fail(where + ": " + why);
  const Plan& plan = city->plan;
  // A streamed city is asked for one ring tile at a time, so its plan's tiles are the ring's.
  if (context.world != nullptr) {
    const i32 world_cm = to_cm(context.tile_size);
    if (plan.params.tile_cm != world_cm) {
      return fail(where + ": its plan's tile is " + std::to_string(plan.params.tile_cm / 100) +
                  " m and the world's " + std::to_string(context.tile_size) +
                  " m; a streamed city is expanded one ring tile at a time, so they must be one "
                  "grid");
    }
  }
  if (entry.district.has_value() && *entry.district >= plan.districts.size())
    return fail(where + ": the plan has no district " + std::to_string(*entry.district));
  // The twelve boxes, written beside the plan (or where the entry says) when one is missing: they
  // are a function of nothing, so a file already there is the right one.
  city->mesh_dir = entry.meshes.empty() ? io::join_path(io::parent_path(plan_path), "proxy")
                                        : resolve(entry.meshes);
  bool missing = false;
  for (u32 m = 0; m < k_proxy_meshes; ++m) {
    const std::string path = proxy_mesh_path(city->mesh_dir, static_cast<ProxyMesh>(m));
    missing = missing || !io::exists(path);
    scene_gen::PlacementMesh mesh;
    mesh.path = path;
    mesh.name = std::string("city/") + proxy_mesh_name(static_cast<ProxyMesh>(m));
    city->meshes.push_back(std::move(mesh));
  }
  if (missing && !write_proxy_meshes(city->mesh_dir, &why)) return fail(where + ": " + why);
  city->bounds = plan_bounds(plan);
  *state = city.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<CityState*>(state); }

// The fragment's content, in memory: a tile's proxies, a district's buildings and ground, or every
// lot's buildings and every street and park, as `engine-content city fragment` makes them.
bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  const CityState& city = *static_cast<const CityState*>(state);
  const Plan& plan = city.plan;
  const CityEntry& entry = city.entry;
  Vector<Proxy> proxies;
  std::string why;
  u32 buildings = 0;
  if (entry.tile.has_value()) {
    const TileCoord tile{(*entry.tile)[0], (*entry.tile)[1]};
    const Stage detail = entry.detail.value_or(Stage::Rooms);
    Vector<u32> lots;
    lots_in_tile(plan, tile, lots);
    for (const u32 l : lots)
      buildings += plan.lots[l].use != LotUse::Park ? 1u : 0u;
    if (!tile_proxies(plan, tile, detail, proxies, &why)) {
      if (error != nullptr) *error = city.where + ": " + why;
      return false;
    }
  } else {
    const bool district = entry.district.has_value();
    const Stage detail = entry.detail.value_or(district ? Stage::Floors : Stage::Massing);
    Vector<u32> lots;
    for (u32 l = 0; l < plan.lots.size(); ++l) {
      if (plan.lots[l].use == LotUse::Park) continue;
      if (district && plan.lots[l].district != *entry.district) continue;
      lots.push_back(l);
    }
    buildings = lots.size();
    // In runs of a few hundred, so a district of towers at rooms detail never holds every building
    // at once; the proxies are appended in lot order whatever the thread count.
    jobs::JobSystem* jobs = lots.size() > 32 ? context.jobs() : nullptr;
    Vector<Building> batch;
    for (u32 at = 0; at < lots.size(); at += 256) {
      const u32 n = std::min<u32>(256, lots.size() - at);
      if (!generate_buildings(plan, std::span<const u32>(lots.data() + at, n), detail, jobs, batch,
                              &why)) {
        if (error != nullptr) *error = city.where + ": " + why;
        return false;
      }
      for (const Building& b : batch)
        append_building_proxies(plan, b, detail, proxies);
    }
    if (district) {
      append_district_ground_proxies(plan, *entry.district, proxies);
    } else {
      // Every street's and park's pieces, wherever they are anchored.
      append_ground_proxies(
          plan, Rect{-2'000'000'000, -2'000'000'000, 2'000'000'000, 2'000'000'000}, proxies);
    }
  }
  for (const scene_gen::PlacementMesh& mesh : city.meshes)
    out.meshes.push_back(mesh);
  place(proxies, out);
  out.things += buildings;
  out.kind = 1;
  return true;
}

bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  const CityState& city = *static_cast<const CityState*>(state);
  for (const scene_gen::PlacementMesh& mesh : city.meshes)
    out.push_back(mesh);
  return true;
}

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const CityState& city = *static_cast<const CityState*>(state);
  const Rect r = tile_rect(city.plan, TileCoord{tile.x, tile.z});
  const Rect& b = city.bounds;
  return r.x0 <= b.x1 && r.x1 >= b.x0 && r.z0 <= b.z1 && r.z1 >= b.z0;
}

u8 representation(const void*, const scene_gen::Context& context) noexcept {
  return static_cast<u8>(ring_stage(context));
}

// The per-tile query: the plan's content restricted to the tile, at the ring's detail.
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string* error) {
  const CityState& city = *static_cast<const CityState*>(state);
  if (!occupies(state, at)) return true;
  const TileCoord t{at.x, at.z};
  Vector<Proxy> proxies;
  std::string why;
  if (!tile_proxies(city.plan, t, ring_stage(context), proxies, &why)) {
    if (error != nullptr) *error = "a tile's city was not made: " + why;
    return false;
  }
  if (proxies.empty()) return true;
  place(proxies, out);
  // The buildings whose lot the tile owns, each counted once however many tiles it touches.
  Vector<u32> lots;
  lots_in_tile(city.plan, t, lots);
  for (const u32 l : lots) {
    const Lot& lot = city.plan.lots[l];
    out.things += lot.use != LotUse::Park && owner_tile(city.plan, lot) == t ? 1u : 0u;
  }
  out.kind = 1;
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_city{
    .name = k_placement_generator,
    .open = &open,
    .close = &close,
    .expand = &expand,
    .meshes = &meshes,
    .tile = &tile,
    .occupies = &occupies,
    .representation = &representation,
};
const scene_gen::Registrar k_city_registrar{k_city};

}  // namespace

}  // namespace engine::city
