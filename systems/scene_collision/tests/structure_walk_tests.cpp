// Walking on structures (docs/subsystems/scene_collision.md, "Structures"; roadmap S89; plan 14
// §14.4): a village's house is a placement whose mesh its tile hands as a recipe, so its floors,
// walls, stair and doorways reach the walker's collision as that mesh's proxy, with no code here
// that names a building. These cases hold what that gives a walker: in from the lane through the
// doorway, across the rooms, up the stair and to a window — and stopped where a wall is in the way.

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/dwellings/describe.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/structures/structures.h>
#include <domain/village/flight.h>
#include <domain/village/village.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
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
    CHECK(on_tile.mesh == 2 + hung[k].mesh);
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
