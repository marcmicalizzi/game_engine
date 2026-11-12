// The placement generator `bridges` and what a bridge writes to its neighbours
// (docs/subsystems/bridges.md, "In a scene", "The ground and the neighbours"): `open` reads the
// layer and plans every crossing once, a recipe a bridge; `tile` hands the bridges whose centre
// falls in the tile, each its own mesh (scene_gen.md, "A tile's meshes"); `expand` is all of them.
// The approaches are `Fill` ground edits and the plan a polygon footprint.

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <domain/bridges/bridges.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace engine::bridges {

void unit_direction(const Crossing& c, f64& ux, f64& uz) noexcept {
  const f64 dx = static_cast<f64>(c.dir_x), dz = static_cast<f64>(c.dir_z);
  const f64 len = std::sqrt(dx * dx + dz * dz);
  ux = len > 0 ? dx / len : 1.0;
  uz = len > 0 ? dz / len : 0.0;
}

void world_mm(const Crossing& c, i64 x, i64 z, i64& wx, i64& wz) noexcept {
  f64 ux = 0, uz = 0;
  unit_direction(c, ux, uz);
  // +x the path, +z its right hand with y up: (-uz, ux).
  const f64 fx = static_cast<f64>(x), fz = static_cast<f64>(z);
  wx = c.x_mm + scene_gen::nearest_mm((fx * ux - fz * uz) / 1000.0);
  wz = c.z_mm + scene_gen::nearest_mm((fx * uz + fz * ux) / 1000.0);
}

scene_gen::Placement place(const Crossing& c) noexcept {
  f64 ux = 0, uz = 0;
  unit_direction(c, ux, uz);
  // A turn about y by theta takes +x to (cos, 0, -sin): cos = ux, sin = -uz. The half angle's
  // cosine and sine from the whole's by two square roots (no C library sine).
  const f64 half_cos = std::sqrt(std::max(0.0, 0.5 * (1.0 + ux)));
  const f64 half_sin_abs = std::sqrt(std::max(0.0, 0.5 * (1.0 - ux)));
  const f64 half_sin = -uz < 0 ? -half_sin_abs : half_sin_abs;
  scene_gen::Placement p;
  p.position = WorldPos(static_cast<f64>(c.x_mm) / 1000.0, static_cast<f64>(c.bed_mm) / 1000.0,
                        static_cast<f64>(c.z_mm) / 1000.0);
  p.rotation = Quat(0.0f, static_cast<f32>(half_sin), 0.0f, static_cast<f32>(half_cos));
  return p;
}

scene_gen::TileCoord tile_of(const Crossing& c, i64 tile_mm) noexcept {
  const auto cell = [tile_mm](i64 v) {
    return static_cast<i32>(v >= 0 ? v / tile_mm : -((-v + tile_mm - 1) / tile_mm));
  };
  return {cell(c.x_mm), cell(c.z_mm)};
}

Approach approach_of(const Crossing& c, const BridgePlan& plan, u32 grade) noexcept {
  Approach a;
  if (plan.form == BridgeForm::none || plan.form == BridgeForm::stones) return a;
  const i64 deck = c.bed_mm + plan.deck;
  for (u32 end = 0; end < 2; ++end) {
    const i64 x_end = end == 0 ? plan.x_start : plan.x_end;
    const i64 out = end == 0 ? -1 : 1;
    const i64 ground = profile_at(c, x_end);
    a.step_mm = std::max<i32>(a.step_mm, static_cast<i32>(std::max<i64>(ground - deck, 0)));
    if (ground >= deck) continue;
    // Out along the path at the grade until the fill's top meets the ground, every 100 mm.
    i64 d = 0;
    const i64 limit = static_cast<i64>(c.profile_mm.size()) * c.profile_step_mm;
    while (d < limit) {
      d += 100;
      const i64 top = deck - (static_cast<i64>(grade) * d) / 1000;
      if (top <= profile_at(c, x_end + out * d)) break;
    }
    a.length_mm[end] = static_cast<i32>(d);
  }
  return a;
}

i32 approach_width_mm(const Crossing& c, const BridgePlan& plan) noexcept {
  return std::max<i32>(static_cast<i32>(c.tread_mm), plan.width) + k_approach_shoulders_mm;
}

JsonValue approach_edits_json(const Layer& layer, u32 grade) {
  JsonValue list = JsonValue::array();
  for (u32 k = 0; k < layer.crossings.size(); ++k) {
    const Crossing& c = layer.crossings[k];
    BridgePlan plan;
    std::string why;
    if (!plan_bridge(c, crossing_seed(layer.seed, c), plan, why)) continue;
    const Approach a = approach_of(c, plan, grade);
    const i64 deck = c.bed_mm + plan.deck;
    for (u32 end = 0; end < 2; ++end) {
      const i64 length = a.length_mm[end];
      if (length <= 0) continue;
      const i64 x_end = end == 0 ? plan.x_start : plan.x_end;
      const i64 out = end == 0 ? -1 : 1;
      JsonValue points = JsonValue::array(), widths = JsonValue::array();
      // The deck's end, then the line falling at the grade to where it meets the ground: two
      // points, since the fill is straight; its top at the deck's end is the deck.
      const i64 ends[2] = {0, length};
      for (const i64 d : ends) {
        i64 wx = 0, wz = 0;
        world_mm(c, x_end + out * d, 0, wx, wz);
        const i64 top = deck - (static_cast<i64>(grade) * d) / 1000;
        JsonValue at = JsonValue::array();
        at.push_back(JsonValue(static_cast<f64>(wx) / 1000.0));
        at.push_back(JsonValue(static_cast<f64>(top) / 1000.0));
        at.push_back(JsonValue(static_cast<f64>(wz) / 1000.0));
        points.push_back(std::move(at));
        widths.push_back(JsonValue(static_cast<f64>(approach_width_mm(c, plan)) / 1000.0));
      }
      JsonValue o = JsonValue::object();
      o["name"] = JsonValue(layer.name + " " + std::to_string(k) + (end == 0 ? " near" : " far") +
                            " approach");
      o["mode"] = JsonValue("Fill");
      o["points"] = std::move(points);
      o["width_m"] = std::move(widths);
      // A soft embankment's batter and a small rounding, as the routes' treads take
      // (routes.md, "The tread").
      o["fill_slope"] = JsonValue(0.5);
      o["bank_round"] = JsonValue(0.15);
      o["fill_m"] = JsonValue(1.0);
      list.push_back(std::move(o));
    }
  }
  return list;
}

scene_gen::Footprint bridge_footprint(const Crossing& c, const BridgePlan& plan,
                                      scene_gen::FootprintPoint (&points)[4]) noexcept {
  if (plan.members.empty()) return scene_gen::Footprint{};
  i32 x0 = plan.members[0].x0, x1 = plan.members[0].x1;
  i32 z0 = plan.members[0].z0, z1 = plan.members[0].z1;
  for (const Member& m : plan.members) {
    x0 = std::min(x0, m.x0);
    x1 = std::max(x1, m.x1);
    z0 = std::min(z0, m.z0);
    z1 = std::max(z1, m.z1);
  }
  const i32 xs[4] = {x0, x1, x1, x0}, zs[4] = {z0, z0, z1, z1};
  for (u32 i = 0; i < 4; ++i)
    world_mm(c, xs[i], zs[i], points[i].x_mm, points[i].z_mm);
  return scene_gen::Footprint::polygon({points, 4});
}

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_bridges, "bridges");

struct Built {
  scene_gen::PlacementMesh mesh;
  scene_gen::Placement placement;
  scene_gen::TileCoord tile;
  // The plan's footprint (`bridge_footprint`), its points held here, and the tiles its disc grown
  // by `k_shape_reach_mm` reaches (inclusive), which `tile` hands it to.
  scene_gen::FootprintPoint corners[4];
  scene_gen::TileCoord reach_lo, reach_hi;
  scene_gen::Footprint shape() const noexcept {
    return scene_gen::Footprint::polygon({corners, 4});
  }
  bool reaches(scene_gen::TileCoord t) const noexcept {
    return t.x >= reach_lo.x && t.x <= reach_hi.x && t.z >= reach_lo.z && t.z <= reach_hi.z;
  }
};

struct Entry {
  Layer layer;
  Vector<Built> bridges;  // the crossings that are built, in the layer's order
  i64 tile_mm = 32000;
  std::string where;
};

constexpr i64 floor_div(i64 a, i64 b) noexcept { return a / b - ((a % b != 0 && a < 0) ? 1 : 0); }

bool fail(std::string* error, const std::string& where, std::string_view why) {
  if (error != nullptr) *error = where + ": bridges: " + std::string(why);
  return false;
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto entry = std::make_unique<Entry>();
  entry->where = context.where.empty() ? std::string("bridges") : std::string(context.where);
  schema::ReadContext read;
  if (!schema::from_json(entry->layer, params, read) || !read.ok())
    return fail(error, entry->where,
                read.diagnostics.empty()
                    ? std::string("invalid engine.bridges.Layer")
                    : read.diagnostics[0].path + ": " + read.diagnostics[0].message);
  if (entry->layer.approach_grade_permille < 1 || entry->layer.approach_grade_permille > 1000)
    return fail(error, entry->where, "approach_grade_permille must be 1..1000");
  const i64 tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  entry->tile_mm = tile_mm > 0 ? tile_mm : 32000;
  // Where a recipe's file would be: the layer's directory, else the derived-data root's
  // `bridges/`, else `bridges` beside the scene. Nothing is written there: the recipe travels with
  // the tile (scene_gen.md, "A tile's meshes").
  std::string dir;
  if (!entry->layer.recipes.empty())
    dir = (std::filesystem::path(std::string(context.dir)) / entry->layer.recipes)
              .lexically_normal()
              .generic_string();
  else if (!context.ddc.empty())
    dir = io::join_path(context.ddc, "bridges");
  else
    dir = io::join_path(context.dir.empty() ? std::string_view(".") : context.dir, "bridges");
  u32 fords = 0;
  for (u32 k = 0; k < entry->layer.crossings.size(); ++k) {
    const Crossing& c = entry->layer.crossings[k];
    Bridge bridge;
    bridge.crossing = c;
    bridge.seed = crossing_seed(entry->layer.seed, c);
    bridge.stone_role = entry->layer.stone_role;
    bridge.wood_role = entry->layer.wood_role;
    bridge.log_role = entry->layer.log_role;
    BridgePlan plan;
    std::string why;
    if (!plan_bridge(c, bridge.seed, plan, why))
      return fail(error, entry->where, "crossings " + std::to_string(k) + ": " + why);
    if (plan.members.empty()) {
      ++fords;
      continue;
    }
    char name[48];
    std::snprintf(name, sizeof(name), "bridge-%016llx.bridge.json",
                  static_cast<unsigned long long>(source_hash(bridge)));
    Built b;
    const scene_gen::Footprint shape = bridge_footprint(c, plan, b.corners);
    // The reach is the footprint's disc about the crossing's centre (the mesh's origin), not
    // about the plan's middle: what a later entry reads as this placement's disc.
    f64 reach = 0.0;
    for (const scene_gen::FootprintPoint& q : b.corners) {
      const f64 dx = static_cast<f64>(q.x_mm - c.x_mm), dz = static_cast<f64>(q.z_mm - c.z_mm);
      reach = std::max(reach, std::sqrt(dx * dx + dz * dz) / 1000.0);
    }
    b.mesh = scene_gen::PlacementMesh(io::join_path(dir, name), std::string("bridges/") + name, 0,
                                      canonical_json(bridge), static_cast<f32>(reach));
    b.placement = place(c);
    b.placement.id = k + 1;
    b.tile = tile_of(c, entry->tile_mm);
    // The tiles the shape is handed to: every tile its disc, grown by the reach a later entry
    // keeps past a shape, touches (scene_gen.md, "The shapes a tile occupies").
    const i64 r = static_cast<i64>(shape.radius * 1000.0) + 1 + k_shape_reach_mm;
    b.reach_lo = {static_cast<i32>(floor_div(shape.x_mm - r, entry->tile_mm)),
                  static_cast<i32>(floor_div(shape.z_mm - r, entry->tile_mm))};
    b.reach_hi = {static_cast<i32>(floor_div(shape.x_mm + r, entry->tile_mm)),
                  static_cast<i32>(floor_div(shape.z_mm + r, entry->tile_mm))};
    entry->bridges.push_back(std::move(b));
  }
  ENGINE_LOG_INFO(log_bridges, "bridges planned", log::field("entry", entry->where),
                  log::field("crossings", static_cast<u64>(entry->layer.crossings.size())),
                  log::field("built", static_cast<u64>(entry->bridges.size())),
                  log::field("fords", static_cast<u64>(fords)));
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

// A tile is occupied when a bridge stands in it or a bridge's shape reaches it: both are what its
// hand-over carries.
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const Entry& e = *static_cast<const Entry*>(state);
  for (const Built& b : e.bridges)
    if (b.tile == tile || b.reaches(tile)) return true;
  return false;
}

// **A tile's hand-over** (scene_gen.md, "A tile's meshes", "The shapes a tile occupies"): the
// bridge whose centre is in the tile, as its own mesh, and the plan of every bridge whose shape
// reaches it, so the entries after this one keep their cover out from under the deck on a
// streamed tile as on the whole read.
bool tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string* error) {
  out.clear();
  const Entry& e = *static_cast<Entry*>(state);
  if (scene_gen::nearest_mm(static_cast<f64>(context.tile_size)) != e.tile_mm)
    return fail(error, e.where, "the tile size changed after open");
  for (const Built& b : e.bridges) {
    if (b.reaches(t)) out.shapes.add(b.shape());
    if (!(b.tile == t)) continue;
    scene_gen::Placement p = b.placement;
    p.mesh = out.meshes.size();  // no resident kit: a tile's meshes are all its own
    out.meshes.push_back(b.mesh);
    out.instances.push_back(p);
  }
  out.things = out.instances.size();
  return true;
}

bool expand(void* state, const scene_gen::Context&, scene_gen::Placements& out, std::string*) {
  out.clear();
  const Entry& e = *static_cast<Entry*>(state);
  for (const Built& b : e.bridges) {
    scene_gen::Placement p = b.placement;
    p.mesh = out.meshes.size();
    out.meshes.push_back(b.mesh);
    out.instances.push_back(p);
    out.shapes.add(b.shape());
  }
  out.things = out.instances.size();
  return true;
}

// **The approaches, handed to the ground** (bridges.md, "The ground and the neighbours";
// scene_gen.md, "What an entry plans for the ground"): each `Fill` edit read back from the JSON
// `engine-content bridges` writes, so an approach planned at the read and the same approach
// pasted into `terrain.tiles.edits` are one edit, field for field, and build one tile set. A
// function of the layer alone: the crossings carry the ground they were sampled on.
bool plan_ground(void* state, const scene_gen::Context&, scene_gen::GroundPlan& out,
                 std::string* error) {
  const Entry& e = *static_cast<const Entry*>(state);
  const JsonValue edits = approach_edits_json(e.layer, e.layer.approach_grade_permille);
  for (const JsonValue& json : edits.as_array()) {
    scene::TerrainEdit edit;
    schema::ReadContext reader;
    if (!schema::from_json(edit, json, reader) || !reader.ok())
      return fail(error, e.where,
                  "an approach's edit did not read back as engine.scene.TerrainEdit");
    out.edits.push_back(std::move(edit));
  }
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_generator{.name = "bridges",
                                                        .open = open,
                                                        .close = close,
                                                        .expand = expand,
                                                        .meshes = nullptr,
                                                        .tile = tile,
                                                        .occupies = occupies,
                                                        .representation = nullptr,
                                                        .tile_meshes = true,
                                                        .tile_meshes_masked = false,
                                                        .plan_ground = plan_ground};
const scene_gen::Registrar k_registrar{k_generator};

}  // namespace
}  // namespace engine::bridges
