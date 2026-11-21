// Walking on structures (docs/subsystems/scene_collision.md, "Structures"; roadmap S89; plan 14
// §14.4): a village's house is a placement whose mesh its tile hands as a recipe, so its floors,
// walls, stair and doorways reach the walker's collision as that mesh's proxy, with no code here
// that names a building. These cases hold what that gives a walker: in from the lane through the
// doorway, across the rooms, up the stair and to a window — and stopped where a wall is in the way.

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/dwellings/describe.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/structures/structures.h>
#include <domain/village/flight.h>
#include <domain/village/village.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <span>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }

world::RingParams ring() {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  return p;
}

sim::ObserverSet at(f64 x, f64 z) {
  sim::ObserverSet set;
  set.add(WorldPos{x, 0.0, z}, 1.0f);
  return set;
}

physics::WorldOptions physics_options() {
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  return o;
}

// A flat floor at y = 0: the scene has no ground provider, so the village's pads and its lane are
// at zero and this is them.
void add_floor(physics::World& physics) {
  physics::ShapeId shape;
  REQUIRE(physics.create_box(Vec3{400.0f, 0.5f, 400.0f}, shape) == physics::Status::Ok);
  physics::BodyDesc slab;
  slab.shape = shape;
  slab.transform.position = place(Vec3{0.0f, -0.5f, 0.0f});
  slab.motion = physics::MotionType::Static;
  slab.layer = physics::Layer::Static;
  physics::BodyId body;
  REQUIRE(physics.create_body(slab, body) == physics::Status::Ok);
}

// A streamed scene of one village entry, two houses along the left of a street, and the plan the
// generator makes of it (the same function on the same flat ground).
struct Village {
  renderer::SceneData scene;
  village::VillageLayer layer;
  village::VillagePlan plan;
};
void village_scene(Village& v) {
  renderer::StreamedPlacements entry;
  entry.generator = "village";
  const std::string params =
      "{\"name\":\"walk\",\"seed\":7,\"houses\":2,\"storeys\":2,\"lanes\":[{\"name\":\"street\","
      "\"waypoints\":[[8,0,40],[88,0,40]],\"rows\":\"Left\"}]}";
  REQUIRE(parse_json(params, entry.params).ok);
  entry.where = "test.json: placements 0";
  entry.tile_meshes = true;
  schema::ReadContext reader;
  REQUIRE(schema::from_json(v.layer, entry.params, reader));
  REQUIRE(reader.ok());
  v.scene.streamed.push_back(entry);
  v.scene.tile_meshes = true;
  v.scene.mesh_options.cache = false;
  v.scene.terrain_mesh = ~0u;
  v.scene.world.enabled = true;
  v.scene.dynamic = true;
  scene_gen::Context context;
  std::string error;
  REQUIRE_MESSAGE(village::plan_scene_village(v.layer, context, v.plan, error), error);
  REQUIRE_MESSAGE(v.plan.built == 2, "refused " << v.plan.refused << ", no fit " << v.plan.no_fit
                                                << ": " << v.plan.first_refusal);
}

// Steers a walker at each point in turn. Returns how many it reached; `stuck` is where it stood
// when it gave up on the next.
u32 walk_along(physics::CharacterBody& walker, const Vector<village::WalkPoint>& points,
               WorldPos& stuck, f32& highest) {
  const f64 speed = static_cast<f64>(walker.config().walk_speed);
  const f64 hz = static_cast<f64>(walker.config().step_hz);
  u32 reached = 1;  // it starts on the first
  for (u32 k = 1; k < points.size(); ++k) {
    const WorldPos target = points[k].feet;
    const WorldPos from = walker.feet();
    const f64 apart = std::sqrt((target.x - from.x) * (target.x - from.x) +
                                (target.z - from.z) * (target.z - from.z));
    // Three times the walk at its own speed, and three seconds for the steps of a stair.
    const u32 budget = static_cast<u32>((3.0 * apart / speed + 3.0) * hz);
    bool there = false;
    for (u32 tick = 0; tick < budget && !there; ++tick) {
      const WorldPos feet = walker.feet();
      const f64 dx = target.x - feet.x, dz = target.z - feet.z;
      const f64 d = std::sqrt(dx * dx + dz * dz);
      if (d < 0.12 && std::fabs(target.y - feet.y) < 0.45) {
        there = true;
        break;
      }
      physics::CharacterInput input;
      if (d > 1e-6) input.move = Vec3{static_cast<f32>(dx / d), 0.0f, static_cast<f32>(dz / d)};
      // Slower over the last stretch, so a step does not carry it past the point.
      input.speed_scale = d < 0.3 ? 0.4f : 1.0f;
      REQUIRE(walker.step(input) == physics::Status::Ok);
      highest = std::max(highest, static_cast<f32>(walker.feet().y));
    }
    if (!there) {
      stuck = walker.feet();
      return reached;
    }
    ++reached;
  }
  stuck = walker.feet();
  return reached;
}

}  // namespace

TEST_CASE("scene_collision: a walker enters a house by its door, crosses it and climbs its stair") {
  Village v;
  village_scene(v);
  Vector<village::WalkPoint> points;
  WorldPos look;
  std::string error;
  REQUIRE_MESSAGE(village::walk_points(v.layer, v.plan, 0, points, look, error), error);
  REQUIRE(points.size() >= 7);
  const village::Plot* plot = nullptr;
  for (const village::Plot& p : v.plan.plots)
    if (p.state == village::PlotState::built && p.house == 0) plot = &p;
  REQUIRE(plot != nullptr);
  const village::House& house = v.plan.houses[0];
  // **The plot's own setback**: a grown village sets each house back by its own draw from the
  // entry's `setback_m` (village.md, "Plots"), so the house's front is where the plan put its door,
  // behind the plot's front along `inward` (the door is on the front face).
  const f64 setback = (static_cast<f64>(plot->door_x_mm - plot->front_x_mm) * plot->inward_x +
                       static_cast<f64>(plot->door_z_mm - plot->front_z_mm) * plot->inward_z) /
                      1000.0;
  REQUIRE(setback > 0.0);

  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  add_floor(physics);
  {
    SceneCollision collision;
    REQUIRE_MESSAGE(collision.create(physics, v.scene, nullptr, Config{}, &error), error);
    CHECK(collision.streamed());
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(static_cast<f64>(plot->house_x_mm) / 1000.0,
                    static_cast<f64>(plot->house_z_mm) / 1000.0),
                 0, true);
    const Stats& s = collision.stats();
    CHECK(s.placements == 2);  // both houses stand within the ring
    CHECK(s.tile_proxies >= 1);
    MESSAGE("two houses: " << s.tile_proxies << " proxies of " << s.proxy_triangles
                           << " triangles, " << s.proxy_bytes << " bytes; compounds "
                           << s.compound_bytes << " bytes");

    physics::CharacterConfig c;
    c.step_hz = 120;
    physics::CharacterBody walker;
    const WorldPos start{points[0].feet.x, points[0].feet.y + 0.05, points[0].feet.z};
    REQUIRE(walker.create(physics, c, start) == physics::Status::Ok);
    WorldPos stuck;
    f32 highest = 0.0f;
    const u32 reached = walk_along(walker, points, stuck, highest);
    INFO("stopped at (" << stuck.x << ", " << stuck.y << ", " << stuck.z << ") on the way to '"
                        << std::string(points[std::min(reached, points.size() - 1)].what)
                        << "' at (" << points[std::min(reached, points.size() - 1)].feet.x << ", "
                        << points[std::min(reached, points.size() - 1)].feet.y << ", "
                        << points[std::min(reached, points.size() - 1)].feet.z << ")");
    CHECK(reached == points.size());
    // At the window: upstairs (a storey over the pad), standing, inside the footprint's box.
    const WorldPos end = walker.feet();
    const f64 pad = static_cast<f64>(plot->pad_mm) / 1000.0;
    CHECK(std::fabs(end.y - points[points.size() - 1].feet.y) < 0.1);
    CHECK(house.route.size() >= 2);
    CHECK(end.y > pad + 2.0);  // the entry asks two storeys: the window is an upper one
    CHECK(walker.state().ground == physics::Ground::OnGround);
    const f64 rx = end.x - static_cast<f64>(plot->front_x_mm) / 1000.0;
    const f64 rz = end.z - static_cast<f64>(plot->front_z_mm) / 1000.0;
    const f64 behind_front = rx * plot->inward_x + rz * plot->inward_z;
    CHECK(behind_front > setback);
    MESSAGE("walked " << points.size() << " points to the window at (" << end.x << ", " << end.y
                      << ", " << end.z << "), " << end.y - pad << " m over the pad");

    // **And a wall is a wall.** A second walker starts on the pad a step from the house's front,
    // beside the doorway and not in it, and walks at the house for five seconds: it stops at the
    // wall's face, a capsule's radius short of it.
    const f64 door_u = (static_cast<f64>(plot->door_x_mm - plot->front_x_mm) * plot->along_x +
                        static_cast<f64>(plot->door_z_mm - plot->front_z_mm) * plot->along_z) /
                       1000.0;
    // Toward the middle of the front, so the walker faces wall and not the house's corner.
    const f64 aside =
        (door_u > 0.0 ? -1.0 : 1.0) * (static_cast<f64>(house.door_width_mm) / 2000.0 + 0.9);
    const auto front_point = [&](f64 u, f64 back) {
      return WorldPos{
          static_cast<f64>(plot->front_x_mm) / 1000.0 + plot->along_x * u + plot->inward_x * back,
          pad + 0.05,
          static_cast<f64>(plot->front_z_mm) / 1000.0 + plot->along_z * u + plot->inward_z * back};
    };
    physics::CharacterBody blocked;
    REQUIRE(blocked.create(physics, c, front_point(door_u + aside, setback - 1.2)) ==
            physics::Status::Ok);
    physics::CharacterInput in;
    in.move = Vec3{static_cast<f32>(plot->inward_x), 0.0f, static_cast<f32>(plot->inward_z)};
    for (u32 k = 0; k < 5 * 120; ++k)
      REQUIRE(blocked.step(in) == physics::Status::Ok);
    const WorldPos held = blocked.feet();
    const f64 held_back = (held.x - static_cast<f64>(plot->front_x_mm) / 1000.0) * plot->inward_x +
                          (held.z - static_cast<f64>(plot->front_z_mm) / 1000.0) * plot->inward_z;
    MESSAGE("walked at the wall beside the door: stopped " << setback - held_back
                                                           << " m short of its face");
    CHECK(held_back < setback - 0.25);  // five seconds would have carried it 7.5 m, through it
    CHECK(held_back > setback - 0.45);  // and it did reach the wall
  }
  CHECK(physics.built_shape_count() == 0);
}

namespace {

// **One house on a tile** for the stair case below: a placement generator whose tile (0, 0) hands
// the house in `g_stair_house` as its own mesh, its frame at (16, 0.03, 16) unturned (0.03 m: the
// village's floor over its pad).
scene_gen::PlacementMesh g_stair_house;
constexpr f64 k_house_x = 16.0, k_house_y = 0.03, k_house_z = 16.0;

void stair_house_place(const scene_gen::Context&, scene_gen::Placements& out) {
  out.meshes.push_back(g_stair_house);
  scene_gen::Placement p;
  p.mesh = static_cast<u32>(out.meshes.size() - 1);
  p.position = WorldPos{k_house_x, k_house_y, k_house_z};
  p.id = 1;
  out.instances.push_back(p);
  out.things += 1;
}
bool stair_house_open(const JsonValue&, const scene_gen::Context&, void** state, std::string*) {
  *state = nullptr;
  return true;
}
void stair_house_close(void*) noexcept {}
bool stair_house_expand(void*, const scene_gen::Context& context, scene_gen::Placements& out,
                        std::string*) {
  stair_house_place(context, out);
  return true;
}
bool stair_house_meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>&,
                        std::string*) {
  return true;  // nothing before a tile asks
}
bool stair_house_tile(void*, scene_gen::TileCoord t, const scene_gen::Context& context,
                      scene_gen::Placements& out, std::string*) {
  if (t.x == 0 && t.z == 0) stair_house_place(context, out);
  return true;
}
bool stair_house_occupies(const void*, scene_gen::TileCoord t) noexcept {
  return t.x == 0 && t.z == 0;
}
constexpr scene_gen::PlacementGeneratorDesc k_stair_house{.name = "test-stair-house",
                                                          .open = &stair_house_open,
                                                          .close = &stair_house_close,
                                                          .expand = &stair_house_expand,
                                                          .meshes = &stair_house_meshes,
                                                          .tile = &stair_house_tile,
                                                          .occupies = &stair_house_occupies,
                                                          .tile_meshes = true};

}  // namespace

// **Every stair the rural filler builds is one a walker climbs** (dwellings.md, "The stair"): for
// each plan type with an upper storey — the hall and parlour, the cross-passage with and without
// its inner room, the two-up two-down a narrow toft gets — and three seeds each (the plan's mirror,
// the house's dressing), the filler's own house stands alone on the floor and a walker set on its
// foot landing, a step before the first riser, walks straight up the flight to a step past its top,
// as the village's route does. This is the case that found the well's foot edge: the upper floor's
// slab ended over the first riser, so a walker on the second tread had its crown under the slab's
// edge behind it and could not lift for the third riser.
TEST_CASE(
    "scene_collision: a walker climbs the stair of every house type the rural filler builds") {
  (void)scene_gen::GeneratorRegistry::global().add(k_stair_house);
  struct Ask {
    const char* what;
    dwellings::PlanType type;
    u8 persons, means;
  };
  const Ask asks[] = {
      {"hall and parlour", dwellings::PlanType::HallParlour, 4, 2},
      {"cross-passage with an inner room", dwellings::PlanType::CrossPassage, 7, 2},
      {"cross-passage", dwellings::PlanType::CrossPassage, 8, 1},
      {"two-up two-down", dwellings::PlanType::TwoUpTwoDown, 4, 2},
  };
  const dwellings::RuralStyle style;
  u32 climbed = 0, flights = 0;
  for (const Ask& ask : asks) {
    dwellings::Household household;
    household.persons = ask.persons;
    household.means = ask.means;
    for (u64 seed = 1; seed <= 3; ++seed) {
      // The widest toft (from 20 m down, on the 10 cm module) that gives the household this type
      // on two storeys: the two-up two-down is what a narrow one demotes a hall and parlour to.
      dwellings::RuralPlot plot;
      plot.depth_cm = 1200;
      dwellings::RuralChoice choice;
      bool found = false;
      for (i32 frontage = 2000; frontage >= 400 && !found; frontage -= 10) {
        plot.frontage_cm = frontage;
        found = dwellings::choose_rural(plot, household, style, seed, choice, nullptr) &&
                choice.interior.type == ask.type && choice.interior.storeys == 2 &&
                (ask.type != dwellings::PlanType::CrossPassage ||
                 choice.interior.inner_room == (ask.means != 1));
      }
      INFO(std::string(ask.what) << ", seed " << seed << ", a toft " << plot.frontage_cm
                                 << " cm wide");
      REQUIRE(found);
      structure::Description d;
      std::string error;
      REQUIRE_MESSAGE(dwellings::fill_rural(plot, household, style, seed, "house", d, &error),
                      error);
      REQUIRE_MESSAGE(structures::validate(d, error), error);
      structures::Walk walk;
      REQUIRE_MESSAGE(structures::walk_records(d, walk, error), error);
      structure::Recipe recipe;
      recipe.description = d;
      recipe.scope = structure::MeshScope::whole;
      g_stair_house = scene_gen::PlacementMesh(
          "stairs/house-" + std::to_string(structures::source_hash(recipe)) + ".structure.json",
          ask.what, 0, structures::canonical_json(recipe), 15.0f);

      renderer::SceneData scene;
      renderer::StreamedPlacements entry;
      entry.generator = "test-stair-house";
      REQUIRE(parse_json("{}", entry.params).ok);
      entry.where = "test.json: placements 0";
      entry.tile_meshes = true;
      scene.streamed.push_back(entry);
      scene.tile_meshes = true;
      scene.mesh_options.cache = false;
      scene.terrain_mesh = ~0u;
      scene.world.enabled = true;
      scene.dynamic = true;

      physics::World physics;
      REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
      add_floor(physics);
      {
        SceneCollision collision;
        REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
        world::World world(ring());
        world.add_consumer(collision.consumer());
        world.update(at(k_house_x, k_house_z), 0, true);
        REQUIRE(collision.stats().placements == 1);
        u32 these = 0;
        for (const structures::WalkFlight& f : walk.flights) {
          if (f.stair >= d.stairs.size() || d.stairs[f.stair].storey != 0) continue;
          ++these;
          ++flights;
          const f64 fx = k_house_x + static_cast<f64>(f.foot.x);
          const f64 fz = k_house_z + static_cast<f64>(f.foot.z);
          const f64 dx = static_cast<f64>(f.dir.x), dz = static_cast<f64>(f.dir.y);
          // As the village's route: half a metre before the first riser, then up the flight's run
          // to half a metre past its last riser, on the floor it lands on.
          const f64 run = static_cast<f64>(f.risers - 1) * static_cast<f64>(f.going) + 0.5;
          Vector<village::WalkPoint> points;
          points.push_back(
              village::WalkPoint{WorldPos{fx - dx * 0.5, k_house_y, fz - dz * 0.5}, "foot", true});
          points.push_back(village::WalkPoint{
              WorldPos{fx + dx * run, k_house_y + static_cast<f64>(f.top), fz + dz * run}, "head",
              true});
          physics::CharacterConfig c;
          c.step_hz = 120;
          physics::CharacterBody walker;
          const WorldPos start{points[0].feet.x, points[0].feet.y + 0.05, points[0].feet.z};
          REQUIRE(walker.create(physics, c, start) == physics::Status::Ok);
          WorldPos stuck;
          f32 highest = 0.0f;
          const u32 reached = walk_along(walker, points, stuck, highest);
          const f64 up = walker.feet().y - k_house_y;
          INFO("stair " << f.stair << ": " << f.risers << " risers of " << f.rise << " m on "
                        << f.going << " m; stopped " << up << " m up at (" << stuck.x - k_house_x
                        << ", " << stuck.z - k_house_z << ") of the house's frame");
          CHECK(reached == 2);
          CHECK(std::fabs(up - static_cast<f64>(f.top)) < 0.1);
          if (reached == 2) ++climbed;
        }
        CHECK(these == 1);  // one stair, from the ground storey
      }
      CHECK(physics.built_shape_count() == 0);
    }
  }
  MESSAGE("climbed " << climbed << " of " << flights << " flights, four house types, three seeds");
  CHECK(climbed == flights);
}

// **What is drawn swings with what collides** (S91; scene_collision.md, "Doors"; structures.md, "A
// leaf is a mesh of its own"): every leaf of the example cot, meshed on its own (the `leaf` scope)
// and stood where its section 53 record makes its door (`declared_door`, the house turned and
// placed off the origin), lies inside the door's box at every angle of its swing: each vertex of
// the leaf mesh moved by `door_leaf_pose` is within the box's half sizes of its centre in the box's
// frame, the box's thickness short only by the proud board's 3 mm.
TEST_CASE("scene_collision: a building's leaf mesh stands in its door's box at every angle") {
  const structure::Description cot = structures::example_cot();
  Vector<geometry::ClusterFileDoor> records;
  structures::door_records(cot, records);
  REQUIRE(records.size() == 5);
  const f32 h = 0.38268343f, c = 0.92387953f;  // 45 degrees about +y, by halves
  const Quat turn{0.0f, h, 0.0f, c};
  const WorldPos origin = absolute(WorldPos::origin(), Vec3{40.0f, 2.0f, -12.0f});
  u32 checked = 0;
  for (u32 k = 0; k < records.size(); ++k) {
    structures::MeshPart parts[structures::k_surfaces];
    structures::Report report;
    std::string error;
    REQUIRE(structures::solve(cot, structure::MeshScope::leaf, parts, report, error, k));
    const structures::MeshPart& leaf = parts[static_cast<u32>(structure::Surface::timber)];
    REQUIRE(!leaf.positions.empty());
    DoorBody door = declared_door(records[k], origin, turn, 1.0f, k + 1);
    for (const i32 step : {0, 1, 2, 3, 4}) {
      door.angle_mdeg = door.open_mdeg / 4 * step;
      const DoorLeafPose pose = door_leaf_pose(door);
      const WorldPos centre =
          door.hinge +
          DVec3{rotate(pose.rotation, Vec3{door.half_extents.x, door.half_extents.y, door.face_z})};
      const Quat back = conjugate(pose.rotation);
      f32 worst = 0.0f;
      for (const Vec3& v : leaf.positions) {
        const WorldPos at = pose.origin + DVec3{rotate(pose.rotation, v)};
        const Vec3 local = rotate(back, Vec3{relative(at, centre)});
        worst = std::max(worst, std::fabs(local.x) - door.half_extents.x);
        worst = std::max(worst, std::fabs(local.y) - door.half_extents.y);
        worst = std::max(worst, std::fabs(local.z) - door.half_extents.z - 0.003f);
      }
      CHECK(worst < 1e-4f);
      ++checked;
    }
  }
  CHECK(checked == 25);
}

// **A house's leaves are drawn where its doors stand** (S91; scene_collision.md, "What is drawn
// swings with what collides"): a scene placing the example cot turned and off the origin, and a
// tile handing it, hang each of its five leaves as an instance of the leaf's own mesh. Every vertex
// of each drawn leaf is where the door's box stands it (`door_leaf_pose` of the door the collision
// makes from the same record), and where the house's own mesh had it before the leaf left it: the
// old mesher's frame for the leaf at the description's angle, rebuilt from the record (its pivot,
// its run `cos a shut + sin a swing` and its thickness `sin a shut - cos a swing`), so an untouched
// house draws what it drew.
TEST_CASE(
    "scene_collision: a house's leaves are drawn where its doors stand, shut as it drew them") {
  const structure::Description cot = structures::example_cot();
  structure::Recipe recipe;
  recipe.description = cot;
  recipe.scope = structure::MeshScope::whole;
  const std::string text = structures::canonical_json(recipe);
  Vector<geometry::ClusterFileDoor> records;
  structures::door_records(cot, records);
  REQUIRE(records.size() == 5);
  const f32 h = 0.38268343f, c = 0.92387953f;  // 45 degrees about +y, by halves
  const Quat turn{0.0f, h, 0.0f, c};
  const WorldPos origin = absolute(WorldPos::origin(), Vec3{40.0f, 2.0f, -12.0f});

  // A scene file's instance of the recipe, and a tile's placement of it.
  renderer::SceneDesc desc;
  desc.meshes.push_back("village/cot.structure.json");
  desc.recipes.push_back(renderer::MeshRecipe{"village/cot.structure.json", text});
  renderer::SceneInstance house;
  house.mesh = 0;
  house.transform = Transform3{Vec3{}, turn, Vec3::one()};
  house.origin = origin;
  desc.instances.push_back(house);
  std::string error;
  REQUIRE_MESSAGE(renderer::hang_scene_leaves(desc, error), error);
  REQUIRE(desc.leaves.size() == 5);
  CHECK(desc.meshes.size() == 6);  // the house's and one a leaf
  CHECK(desc.instances.size() == 6);
  CHECK(desc.instances[0].mesh == 0);  // the house keeps its place

  scene_gen::Placements tile;
  tile.meshes.push_back(
      scene_gen::PlacementMesh("village/cot.structure.json", "cot", 0, text, 6.0f));
  scene_gen::Placement placed;
  placed.position = origin;
  placed.rotation = turn;
  placed.mesh = 2;  // two resident meshes before the tile's own
  tile.instances.push_back(placed);
  Vector<renderer::SceneLeaf> hung;
  REQUIRE_MESSAGE(renderer::hang_placement_leaves(tile, 2, &hung, error), error);
  REQUIRE(hung.size() == 5);
  CHECK(tile.meshes.size() == 6);
  CHECK(tile.instances.size() == 6);
  CHECK(tile.instances[0].mesh == 2);

  u32 checked = 0;
  for (u32 k = 0; k < records.size(); ++k) {
    const geometry::ClusterFileDoor& r = records[k];
    structures::MeshPart parts[structures::k_surfaces];
    structures::Report report;
    REQUIRE(structures::solve(cot, structure::MeshScope::leaf, parts, report, error, k));
    const structures::MeshPart& leaf = parts[static_cast<u32>(structure::Surface::timber)];
    REQUIRE(!leaf.positions.empty());
    const renderer::SceneLeaf& drawn = desc.leaves[k];
    CHECK(drawn.house == 0);
    CHECK(drawn.leaf == k);
    const renderer::SceneInstance& at = desc.instances[drawn.instance];
    CHECK(desc.meshes[at.mesh] == structures::leaf_path("village/cot.structure.json", k));
    CHECK(hung[k].leaf == k);
    const scene_gen::Placement& on_tile = tile.instances[hung[k].instance];
    CHECK(on_tile.mesh == hung[k].mesh);  // the placement's own index, resident meshes counted
    // The door the collision makes from the same record, at the angle it is declared at.
    const DoorBody door = declared_door(r, origin, turn, 1.0f, k + 1);
    CHECK(door.angle_mdeg == drawn.angle_mdeg);
    const DoorLeafPose pose = door_leaf_pose(door);
    // The old mesher's frame for this leaf in the house's frame: the leaf frame's x, y and z, its
    // run, up and its thickness (the leaf's own z is that side's, `face_z`'s sign).
    const f64 a = static_cast<f64>(r.open_deg) * (3.14159265358979323846 / 180.0);
    const f64 ca = std::cos(a), sa = std::sin(a);
    const Vec3 shut{r.shut_x, 0.0f, r.shut_z}, swing{r.swing_x, 0.0f, r.swing_z};
    const Vec3 run = shut * static_cast<f32>(ca) + swing * static_cast<f32>(sa);
    const Vec3 thick = shut * static_cast<f32>(sa) - swing * static_cast<f32>(ca);
    const f32 side = door.face_z < 0.0f ? -1.0f : 1.0f;
    f32 worst_pose = 0.0f, worst_old = 0.0f, worst_tile = 0.0f;
    for (const Vec3& v : leaf.positions) {
      const WorldPos drawn_at = at.origin + DVec3{rotate(at.transform.rotation, v)};
      const WorldPos tile_at = on_tile.position + DVec3{rotate(on_tile.rotation, v)};
      const WorldPos collides = pose.origin + DVec3{rotate(pose.rotation, v)};
      const Vec3 old_local = r.pivot + run * v.x + Vec3{0.0f, v.y, 0.0f} + thick * (side * v.z);
      const WorldPos old_at = origin + DVec3{rotate(turn, old_local)};
      worst_pose = std::max(worst_pose, length(Vec3{relative(drawn_at, collides)}));
      worst_old = std::max(worst_old, length(Vec3{relative(drawn_at, old_at)}));
      worst_tile = std::max(worst_tile, length(Vec3{relative(tile_at, drawn_at)}));
    }
    INFO("leaf " << k << ": " << worst_pose << " m from its door's pose, " << worst_old
                 << " m from the house mesh's leaf, " << worst_tile << " m tile to scene");
    CHECK(worst_pose < 1e-4f);
    CHECK(worst_old < 1e-4f);
    CHECK(worst_tile < 1e-5f);
    ++checked;
  }
  CHECK(checked == 5);
}

namespace {

// The example cot as a streamed tile's own mesh, handed by `test-stair-house` at (16, 0.03, 16).
void hand_cot() {
  structure::Recipe recipe;
  recipe.description = structures::example_cot();
  recipe.scope = structure::MeshScope::whole;
  g_stair_house = scene_gen::PlacementMesh("village/cot-doors.structure.json", "cot", 0,
                                           structures::canonical_json(recipe), 6.0f);
}

renderer::StreamedPlacements test_entry(const char* generator) {
  renderer::StreamedPlacements entry;
  entry.generator = generator;
  REQUIRE(parse_json("{}", entry.params).ok);
  entry.where = std::string("test.json: ") + generator;
  entry.tile_meshes = true;
  return entry;
}

// **What an entry after the houses is handed** (S91): a generator that places nothing and counts,
// over tile (0, 0) on a half-metre lattice, the points no footprint in `Context::earlier` covers —
// the question a forest, a sward or a scatter asks of a candidate — and how many footprints it was
// handed. Asked by the drawn tile, the query tile, the collision's tile and a whole read alike.
struct EarlierProbe {
  bool asked = false;
  u32 earlier = 0;
  u32 open = 0;
};
EarlierProbe g_probe;
void probe_count(const scene_gen::Context& context) {
  g_probe.asked = true;
  g_probe.earlier = static_cast<u32>(context.earlier.size());
  g_probe.open = 0;
  for (i64 z = 250; z < 32000; z += 500) {
    for (i64 x = 250; x < 32000; x += 500) {
      bool covered = false;
      for (const scene_gen::Footprint& f : context.earlier)
        covered = covered || f.covers_mm(x, z);
      g_probe.open += covered ? 0u : 1u;
    }
  }
}
bool probe_expand(void*, const scene_gen::Context& context, scene_gen::Placements&, std::string*) {
  probe_count(context);
  return true;
}
bool probe_tile(void*, scene_gen::TileCoord t, const scene_gen::Context& context,
                scene_gen::Placements&, std::string*) {
  if (t.x == 0 && t.z == 0) probe_count(context);
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_earlier_probe{.name = "test-earlier-probe",
                                                            .open = &stair_house_open,
                                                            .close = &stair_house_close,
                                                            .expand = &probe_expand,
                                                            .meshes = &stair_house_meshes,
                                                            .tile = &probe_tile,
                                                            .occupies = &stair_house_occupies};

}  // namespace

// **A streamed house's doors stand as drawn** (S91; scene_collision.md, "Doors"): a streamed tile's
// leaves are drawn at their declared angles in the tile's block, which nothing turns yet, so its
// doors collide where the leaves are drawn and an interact or a game's toggle turns none. Until
// 2026-11-20 a toggle swung the box and left the drawn leaf where it was: a gap the picture showed
// shut, or a box across a doorway it showed open. They hold no state of their own, so they go with
// their tile and come back as declared.
TEST_CASE("scene_collision: a streamed house's doors stand as drawn, and a toggle turns none") {
  (void)scene_gen::GeneratorRegistry::global().add(k_stair_house);
  hand_cot();
  renderer::SceneData scene;
  scene.streamed.push_back(test_entry("test-stair-house"));
  scene.tile_meshes = true;
  scene.mesh_options.cache = false;
  scene.terrain_mesh = ~0u;
  scene.world.enabled = true;
  scene.dynamic = true;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  add_floor(physics);
  {
    std::string error;
    SceneCollision collision;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(k_house_x, k_house_z), 0, true);
    (void)collision.step_doors(nullptr);
    REQUIRE(collision.door_stats().doors == 5);  // the cot's door and its four shutters
    CHECK(collision.door_stats().bodies == 5);
    Vector<i32> declared;
    for (const DoorBody& d : collision.doors()) {
      declared.push_back(d.angle_mdeg);
      CHECK_FALSE(collision.toggle_door(d.key));
    }
    for (u32 k = 0; k < 480; ++k)
      (void)collision.step_doors(nullptr);
    for (u32 i = 0; i < collision.doors().size(); ++i) {
      CAPTURE(i);
      CHECK(collision.doors()[i].angle_mdeg == declared[i]);
    }
    CHECK(collision.door_stats().toggles == 0);
    CHECK(collision.door_stats().bodies == 5);  // a leaf drawn shut still stops the walker
    // The tile goes: its doors with it, and back as declared.
    world.update(at(1000.0, 1000.0), 1, true);
    (void)collision.step_doors(nullptr);
    CHECK(collision.door_stats().doors == 0);
    world.update(at(k_house_x, k_house_z), 2, true);
    (void)collision.step_doors(nullptr);
    CHECK(collision.door_stats().doors == 5);
    CHECK(collision.door_stats().bodies == 5);
  }
  CHECK(physics.built_shape_count() == 0);
}

// **A scene's piece and a streamed piece are never one placement** (S91; scene_collision.md,
// "Doors"): scene instance n and the piece entry 0 names n are two names and two door keys. Until
// 2026-11-21 a streamed piece with an id was `entry << 32 | id`, which for entry 0 is n.
TEST_CASE("scene_collision: a scene's piece and a streamed piece are never one placement") {
  for (const u32 n : {1u, 2u, 7u, 0x7FFFFFFFu, 0xFFFFFFFEu}) {
    CAPTURE(n);
    const u64 scene = support_placement(n, k_no_index, 0, 0, 0, 0);
    CHECK(scene == n);  // a scene instance's name is its index, which the walker's doors read
    for (const u32 entry : {0u, 1u}) {
      CAPTURE(entry);
      const u64 streamed = support_placement(k_no_index, entry, n, 0, 0, 0);
      CHECK(streamed != scene);
      CHECK(streamed != support_placement(entry, k_no_index, 0, 0, 0, 0));
      CHECK(streamed != support_placement(k_no_index, entry, 0, 0, 0, n));  // the third form
      for (u32 k = 0; k < 4; ++k)
        CHECK(declared_door_key(streamed, k) != declared_door_key(scene, k));
    }
    CHECK(support_placement(k_no_index, 0, n, 0, 0, 0) !=
          support_placement(k_no_index, 1, n, 0, 0, 0));
  }
}

namespace {

// A box standing on y = 0 round (cx, cz), faces wound outwards, as a scene's own mesh.
void add_scene_box(Vector<Vec3>& positions, Vector<u32>& indices, Vec3 half, f32 cx, f32 cz) {
  const u32 base = positions.size();
  for (u32 k = 0; k < 8; ++k) {
    positions.push_back(Vec3{cx + ((k & 1) ? half.x : -half.x), (k & 2) ? 2.0f * half.y : 0.0f,
                             cz + ((k & 4) ? half.z : -half.z)});
  }
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      indices.push_back(base + v);
  }
}

// The scene's own two pieces on tile (0, 0) beside the streamed cot: instance 0 a post, instance 1
// a wall whose leaf the scene hung, hinged at (7.55, 0, 8), shut along +x, swinging toward +z.
void scene_house_at_instance_1(renderer::SceneData& scene) {
  Vector<geometry::ClusterLodMesh> lods;
  std::string error;
  for (u32 mesh = 0; mesh < 2; ++mesh) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    if (mesh == 0) {
      add_scene_box(positions, indices, Vec3{0.2f, 1.0f, 0.2f}, 4.0f, 4.0f);
    } else {
      add_scene_box(positions, indices, Vec3{1.5f, 1.0f, 0.05f}, 6.05f, 8.05f);
      add_scene_box(positions, indices, Vec3{1.5f, 1.0f, 0.05f}, 9.95f, 8.05f);
    }
    geometry::ClusterLodMesh lod;
    REQUIRE_MESSAGE(
        geometry::build_cluster_lod(std::span<const Vec3>(positions.data(), positions.size()),
                                    std::span<const u32>(indices.data(), indices.size()),
                                    geometry::ClusterLodOptions{}, lod, &error),
        error);
    lods.push_back(std::move(lod));
  }
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(
                      std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()),
                      scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  u32 first = 0;
  for (u32 mesh = 0; mesh < 2; ++mesh) {
    renderer::SceneInstance placed;  // every mesh once, at the origin
    placed.mesh = mesh;
    gfx::InstanceDesc desc;
    REQUIRE(renderer::make_instance(scene, placed, first, desc, &error));
    first += scene.parts[mesh].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;
  scene.sources.resize(scene.parts.size());
  geometry::ClusterFileDoor r;
  r.pivot = Vec3{7.55f, 0.0f, 8.0f};
  r.shut_x = 1.0f, r.shut_z = 0.0f;
  r.swing_x = 0.0f, r.swing_z = 1.0f;
  r.width_m = 0.9f;
  r.height_m = 2.0f;
  r.thickness_m = 0.05f;
  r.open_deg = 0;
  r.flags = geometry::k_door_flag_door;
  scene.sources[1].doors.push_back(r);
  scene.leaves.push_back(renderer::SceneLeaf{~0u, 1, 0, 0, 1.0f, 0});
}

}  // namespace

// **A streamed piece never takes a scene's door** (S91; scene_collision.md, "Doors"): the scene
// hangs the leaf of its instance 1, and the streamed cot on the same tile is the piece entry 0
// names
// 1. The scene's door, opened, is still open, still turning and one body after two ring changes
// that hold both. Until 2026-11-21 the two were one placement, so the cot's first leaf was the
// scene's door by key, and a ring change made it again as the cot declares it, shut and standing.
TEST_CASE("scene_collision: a streamed piece never takes the door of the scene's instance") {
  (void)scene_gen::GeneratorRegistry::global().add(k_stair_house);
  hand_cot();
  renderer::SceneData scene;
  scene_house_at_instance_1(scene);
  scene.streamed.push_back(test_entry("test-stair-house"));
  scene.tile_meshes = true;
  scene.mesh_options.cache = false;
  scene.terrain_mesh = ~0u;
  scene.world.enabled = true;
  scene.dynamic = true;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  add_floor(physics);
  {
    std::string error;
    SceneCollision collision;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(k_house_x, k_house_z), 0, true);
    (void)collision.step_doors(nullptr);
    const u64 key = declared_door_key(support_placement(1, k_no_index, 0, 0, 0, 0), 0);
    DoorBody now;
    REQUIRE(collision.door(key, now));
    CHECK_FALSE(now.fixed);
    CHECK(collision.door_stats().doors == 6);  // the scene's door, the cot's door and four shutters
    CHECK(collision.door_stats().bodies == 6);
    REQUIRE(collision.toggle_door(key));
    for (u32 k = 0; k < 480; ++k)
      (void)collision.step_doors(nullptr);
    REQUIRE(collision.door(key, now));
    const i32 opened = now.angle_mdeg;
    CHECK(opened == -90000);
    for (u64 change = 0; change < 2; ++change) {
      CAPTURE(change);
      world.update(at(1000.0, 1000.0), 1 + 2 * change, true);
      (void)collision.step_doors(nullptr);
      world.update(at(k_house_x, k_house_z), 2 + 2 * change, true);
      (void)collision.step_doors(nullptr);
      REQUIRE(collision.door(key, now));
      CHECK(now.angle_mdeg == opened);
      CHECK(now.open);
      CHECK_FALSE(now.fixed);
      CHECK(collision.door_stats().doors == 6);
      CHECK(collision.door_stats().bodies == 6);
      u32 at_hinge = 0;
      for (const DoorBody& d : collision.doors())
        at_hinge += d.key == key ? 1u : 0u;
      CHECK(at_hinge == 1);
    }
  }
  CHECK(physics.built_shape_count() == 0);
}

// **Whether a declared door turns is the scene's, not a restore's** (S91; scene_collision.md,
// "Doors"): the streamed cot's doors handed back as turning and half open, as a save from before
// they stood would hand them. Until 2026-11-20 a restore was trusted for `fixed`, so a toggle swung
// each box and left its drawn leaf where it was; now the held tile's piece says they stand, and
// they stand at their declared angles.
TEST_CASE("scene_collision: a streamed door restored as turning stands again as drawn") {
  (void)scene_gen::GeneratorRegistry::global().add(k_stair_house);
  hand_cot();
  renderer::SceneData scene;
  scene.streamed.push_back(test_entry("test-stair-house"));
  scene.tile_meshes = true;
  scene.mesh_options.cache = false;
  scene.terrain_mesh = ~0u;
  scene.world.enabled = true;
  scene.dynamic = true;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  add_floor(physics);
  {
    std::string error;
    SceneCollision collision;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(k_house_x, k_house_z), 0, true);
    (void)collision.step_doors(nullptr);
    REQUIRE(collision.door_stats().doors == 5);
    Vector<DoorBody> saved;
    Vector<i32> declared;
    for (const DoorBody& d : collision.doors()) {
      declared.push_back(d.angle_mdeg);
      DoorBody turned = d;
      turned.fixed = false;
      turned.open = true;
      turned.angle_mdeg = d.open_mdeg / 2;
      saved.push_back(turned);
    }
    collision.set_doors({saved.data(), saved.size()});
    (void)collision.step_doors(nullptr);
    REQUIRE(collision.door_stats().doors == 5);
    CHECK(collision.door_stats().declared == 5);
    for (u32 i = 0; i < collision.doors().size(); ++i) {
      CAPTURE(i);
      const DoorBody& d = collision.doors()[i];
      CHECK(d.fixed);
      CHECK(d.angle_mdeg == declared[i]);
      CHECK_FALSE(collision.toggle_door(d.key));
    }
    CHECK(collision.door_stats().toggles == 0);
    CHECK(collision.door_stats().bodies == 5);
  }
  CHECK(physics.built_shape_count() == 0);
}

// **A leaf is no placement of its kind** (S91; scene_collision.md, "What is drawn swings with what
// collides"): the entry after the houses is handed the same footprints by the drawn tile, the query
// tile, the collision's tile and the whole read — the house's disc and none for its leaves. Until
// 2026-11-20 the drawn tile handed a disc round each of the cot's five hinges (the leaves it hung
// went through the entry's footprint loop), which no other reader of the tile had.
TEST_CASE("scene_collision: an entry after the houses is handed no footprint for their leaves") {
  (void)scene_gen::GeneratorRegistry::global().add(k_stair_house);
  (void)scene_gen::GeneratorRegistry::global().add(k_earlier_probe);
  hand_cot();
  renderer::SceneData scene;
  scene.streamed.push_back(test_entry("test-stair-house"));
  scene.streamed.push_back(test_entry("test-earlier-probe"));
  scene.tile_meshes = true;
  scene.mesh_options.cache = false;
  scene.terrain_mesh = ~0u;
  scene.world.enabled = true;
  scene.world.tile_size = 32.0f;
  scene.dynamic = true;
  std::string error;

  // The drawn tile (0, 0), as a host builds it, and the same tile built for a query.
  world::PlacementTiles drawn;
  world::PlacementTilesConfig drawn_config;
  drawn_config.tile_size = 32.0f;
  drawn_config.ring_count = 1;
  REQUIRE_MESSAGE(drawn.create(scene, drawn_config, &error), error);
  const world::TileConsumer consumer = drawn.consumer();
  world::TileEvent event;
  event.tile = {0, 0};
  event.to = 0;
  g_probe = EarlierProbe{};
  (void)consumer.activate(consumer.context, event);
  consumer.commit(consumer.context);
  REQUIRE(g_probe.asked);
  const EarlierProbe on_drawn = g_probe;
  g_probe = EarlierProbe{};
  (void)drawn.query_tile(world::TileCoord{0, 0}, ~0u);
  REQUIRE(g_probe.asked);
  const EarlierProbe on_query = g_probe;

  // The collision's tile.
  g_probe = EarlierProbe{};
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  {
    SceneCollision collision;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(k_house_x, k_house_z), 0, true);
  }
  REQUIRE(g_probe.asked);
  const EarlierProbe on_collision = g_probe;

  // The whole read of the same two entries.
  const test::TempDir tmp("scene_collision_leaf_footprints");
  const std::string path = tmp.file("whole.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"format":"engine.scene.v1","name":"leaf-footprints","placements":[)"
         R"({"generator":"test-stair-house","params":{}},)"
         R"({"generator":"test-earlier-probe","params":{}}]})";
    REQUIRE(f.good());
  }
  g_probe = EarlierProbe{};
  renderer::SceneDesc whole;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, whole, error), error);
  REQUIRE(g_probe.asked);
  const EarlierProbe on_whole = g_probe;
  CHECK(whole.leaves.size() == 5);  // the whole read hangs them, after the entries

  MESSAGE("footprints handed: drawn " << on_drawn.earlier << ", query " << on_query.earlier
                                      << ", collision " << on_collision.earlier << ", whole read "
                                      << on_whole.earlier << "; open points " << on_drawn.open
                                      << ", " << on_query.open << ", " << on_collision.open << ", "
                                      << on_whole.open);
  CHECK(on_drawn.earlier == 1);  // the house's disc
  CHECK(on_query.earlier == on_drawn.earlier);
  CHECK(on_collision.earlier == on_drawn.earlier);
  CHECK(on_whole.earlier == on_drawn.earlier);
  CHECK(on_query.open == on_drawn.open);
  CHECK(on_collision.open == on_drawn.open);
  CHECK(on_whole.open == on_drawn.open);
}
