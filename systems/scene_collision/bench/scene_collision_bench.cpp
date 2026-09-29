// What the scene's collision costs a ring change (docs/subsystems/scene_collision.md, "What it
// costs"): the walker's ring moved a tile east each iteration — three tiles made (a 34 x 34
// heightfield and a compound of a hundred pieces each) and three let go — over a ground of plain
// arithmetic and a scene of boxes read whole. The dunes' own evaluation is the terrain
// capability's cost and is measured in engine-view's summary, where the real ground is.
#include <domain/geometry/cluster_lod.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <foundation/bench/bench.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <cmath>
#include <string>

using namespace engine;

namespace {

void still_destroy(void*) noexcept {}
f32 still_height(const void*, f32 x, f32 z) noexcept {
  return 1.0f + 0.05f * x + 0.02f * z + 0.0005f * x * z;
}
constexpr scene_gen::GroundOps k_ground{.destroy = &still_destroy, .height = &still_height};

// The same plane rising a centimetre a game second, which a time-lapse moves: evaluated in plain
// arithmetic, so what is measured is the collision's own work and not a generator's.
f32 rising_at(f32 x, f32 z, f64 t) noexcept {
  return still_height(nullptr, x, z) + 0.01f * static_cast<f32>(t);
}
f32 rising_height(const void*, f32 x, f32 z) noexcept { return rising_at(x, z, 0.0); }
bool rising_evaluate(const void*, f64 time, const scene_gen::Lattice& lattice, i32 i0, i32 j0,
                     u32 nx, u32 nz, u32, u32, std::span<f32> heights) noexcept {
  for (u32 j = 0; j < nz; ++j)
    for (u32 i = 0; i < nx; ++i)
      heights[j * nx + i] =
          rising_at(lattice.x(i0 + static_cast<i32>(i)), lattice.z(j0 + static_cast<i32>(j)), time);
  return true;
}
f64 rising_travel(const void*, f64 from, f64 to) noexcept { return 0.01 * (to - from); }
constexpr scene_gen::GroundOps k_rising{.destroy = &still_destroy,
                                        .height = &rising_height,
                                        .evaluate = &rising_evaluate,
                                        .travel_m = &rising_travel};

// A 1.2 x 0.6 x 0.6 m block (a ruin's fallen stone, roughly), a hundred to a 32 m tile, over a
// strip of 40 x 3 tiles.
void build(renderer::SceneData& scene) {
  Vector<Vec3> p;
  for (u32 k = 0; k < 8; ++k)
    p.push_back(Vec3{(k & 1) ? 0.6f : -0.6f, (k & 2) ? 0.6f : 0.0f, (k & 4) ? 0.3f : -0.3f});
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  Vector<u32> indices;
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      indices.push_back(v);
  }
  geometry::ClusterLodMesh lod;
  geometry::build_cluster_lod(std::span<const Vec3>(p.data(), p.size()),
                              std::span<const u32>(indices.data(), indices.size()),
                              geometry::ClusterLodOptions{}, lod);
  Vector<geometry::ClusterLodMesh> lods;
  lods.push_back(std::move(lod));
  geometry::merge_cluster_meshes(
      std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()), scene.lod, scene.parts);
  scene.mesh_fit.resize(1, Mat4::identity());
  scene.terrain_mesh = ~0u;
  u32 first = 0;
  for (i32 tz = -1; tz <= 1; ++tz) {
    for (i32 tx = -2; tx < 38; ++tx) {
      for (u32 k = 0; k < 100; ++k) {
        renderer::SceneInstance i;
        i.transform.position =
            Vec3{static_cast<f32>(tx) * 32.0f + 1.5f + static_cast<f32>(k % 10) * 3.0f,
                 still_height(nullptr, 0.0f, 0.0f),
                 static_cast<f32>(tz) * 32.0f + 1.5f + static_cast<f32>(k / 10) * 3.0f};
        gfx::InstanceDesc desc;
        renderer::make_instance(scene, i, first, desc);
        first += scene.parts[0].cluster_count;
        scene.instances.push_back(desc);
      }
    }
  }
  scene.pair_count = first;
}

}  // namespace

ENGINE_BENCH(scene_collision_ring_change, "scene_collision.ring_change") {
  renderer::SceneData scene;
  build(scene);
  int unused = 0;
  scene_gen::GroundProvider ground(&k_ground, &unused);
  physics::WorldOptions options;
  options.max_bodies = 1024;
  physics::World physics;
  physics.init(options);
  scene_collision::SceneCollision collision;
  std::string error;
  collision.create(physics, scene, &ground, scene_collision::Config{}, &error);
  world::RingParams params = scene_collision::ring_params_from_tunables(32.0f);
  params.max_activations = 0;
  params.max_deactivations = 0;
  world::World ring(params);
  ring.add_consumer(collision.consumer());
  u64 tick = 0;
  sim::ObserverSet at;
  at.add(Vec3{16.0f, 0.0f, 16.0f}, 1.0f);
  ring.update(at, tick++, true);
  u32 step = 0;
  while (state.keep_running()) {
    // East a tile an update for 32 and west again: three tiles in, three out, each time.
    const u32 leg = step % 64;
    const f32 x = 16.0f + 32.0f * static_cast<f32>(leg < 32 ? leg : 63 - leg);
    sim::ObserverSet o;
    o.add(Vec3{x, 0.0f, 16.0f}, 1.0f);
    ring.update(o, tick++, true);
    ++step;
  }
  bench::keep(collision.stats().bodies);
  state.set_items(3);
}

// What a walking tick costs the collision when no tile changes: the ring's update from the
// walker's feet, 6 mm on at a time (a walk at 240 Hz), and the refresh a still ground returns from
// at once. The walker's host pays this every tick (walk.h).
ENGINE_BENCH(scene_collision_tick, "scene_collision.tick") {
  renderer::SceneData scene;
  build(scene);
  int unused = 0;
  scene_gen::GroundProvider ground(&k_ground, &unused);
  physics::World physics;
  physics.init(physics::WorldOptions{});
  scene_collision::SceneCollision collision;
  std::string error;
  collision.create(physics, scene, &ground, scene_collision::Config{}, &error);
  world::World ring(scene_collision::ring_params_from_tunables(32.0f));
  ring.add_consumer(collision.consumer());
  u64 tick = 0;
  sim::ObserverSet at;
  at.add(Vec3{16.0f, 0.0f, 16.0f}, 1.0f);
  ring.update(at, tick++, true);
  f32 x = 4.0f;
  while (state.keep_running()) {
    x = x < 28.0f ? x + 0.00625f : 4.0f;  // within the one tile, so nothing comes or goes
    sim::ObserverSet o;
    o.add(Vec3{x, 0.0f, 16.0f}, 1.0f);
    ring.update(o, tick++, false);
    bench::keep(collision.refresh(x, 16.0f));
  }
  bench::keep(collision.stats().bodies);
  state.set_items(1);
}

// What a frame of moving sand costs the walker's host (docs/subsystems/scene_collision.md, "The
// walker goes with the ground"): the drawn blend moved on by a frame's worth, the tile under a
// standing walker rebuilt at it (a 34 x 34 heightfield and its body, the broadphase after), and the
// walker carried with it — the frame's first tick. The pair is fixed, so no field is evaluated:
// that is the generator's cost, and comes a pair at a time.
ENGINE_BENCH(scene_collision_follow, "scene_collision.follow") {
  renderer::SceneData scene;
  build(scene);
  int unused = 0;
  scene_gen::GroundProvider ground(&k_rising, &unused);
  physics::WorldOptions options;
  options.max_bodies = 1024;
  physics::World physics;
  physics.init(options);
  scene_collision::SceneCollision collision;
  std::string error;
  collision.create(physics, scene, &ground, scene_collision::Config{}, &error);
  scene_collision::GroundTime time;
  time.moving = true;
  time.time_a = 0.0;
  time.time_b = 1000.0;  // ten metres of rise across the pair
  collision.set_ground_time(time);
  world::World ring(scene_collision::ring_params_from_tunables(32.0f));
  ring.add_consumer(collision.consumer());
  sim::ObserverSet at;
  at.add(Vec3{16.0f, 0.0f, 16.0f}, 1.0f);
  ring.update(at, 0, true);
  f32 y = 0.0f;
  collision.ground_height(16.0f, 16.0f, y);
  physics::CharacterConfig c;
  c.step_hz = 240;
  physics::CharacterBody body;
  body.create(physics, c, Vec3{16.0f, y + 0.02f, 16.0f});
  u32 frame = 0;
  while (state.keep_running()) {
    // Two millimetres a frame, the pair crossed in 5,000 frames and started again.
    time.blend = static_cast<f64>(frame % 5000 + 1) / 5000.0;
    collision.set_ground_time(time);
    bench::keep(collision.follow(body));
    ++frame;
  }
  bench::keep(collision.stats().refreshes);
  state.set_items(1);
}
