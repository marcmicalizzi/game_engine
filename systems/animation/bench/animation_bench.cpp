// What a crowd of animated characters costs a tick, by tier mix
// (docs/plan/11-performance-principles.md §11.8).
//
// The population is 10^4 instances of a 23-joint skeleton — the standard skeleton's humanoid core,
// which is the size this engine's generated characters will actually be — with a one-second clip
// of a rotation track per joint. The measured thing is the **tick**: the real `SimWorld`, the real
// three systems, the real pose pool, single-threaded, because that is the number
// [ADR-0028](../../docs/adr/0028-ecs-and-persistent-store.md) says to plan against until the
// worker hosting is fixed.
//
// The rows are tier *mixes* rather than a single number, because the whole claim of the LOD policy
// is that the mix is what decides the cost: `animation.tick.lod0` is every instance at LOD0, and
// `animation.tick.mixed` is the distribution a real scene has — a handful near, most far. The
// difference between them is what the policy buys, and a run that does not show it is a policy
// that is not working.
//
// Numbers and the machine's state they were taken on: docs/subsystems/animation.md.
#include <core/base/types.h>
#include <domain/assets/gltf.h>
#include <foundation/bench/bench.h>
#include <systems/animation/animation.h>

#include <memory>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

constexpr u32 k_joints = 23;  // anim::k_standard_joint_count, the humanoid core
constexpr u32 k_full_instances = 10000;
constexpr u32 k_smoke_instances = 256;
constexpr u32 k_hz = 64;

// The population. CTest runs every bench once as a smoke test, in a **debug** build, and standing
// up five 10^4-entity worlds there costs minutes for a run whose only job is to prove the bench
// still compiles and does not crash. The measured configuration is the full one; the smoke run is
// the same code over a crowd small enough to build in a moment (`foundation/store`'s bench sizes
// itself the same way).
u32 instances() noexcept { return bench::smoke_mode() ? k_smoke_instances : k_full_instances; }

// A 23-joint chain with a rotation track on every joint: a clip that touches every joint is the
// worst case for the sampler and the honest one for a character.
void build_library(Library& library) {
  assets::MeshData mesh;
  mesh.nodes.resize(k_joints);
  for (u32 j = 0; j < k_joints; ++j) {
    mesh.nodes[j].name = "joint" + std::to_string(j);
    mesh.nodes[j].parent = j == 0 ? -1 : static_cast<i32>(j - 1);
    mesh.nodes[j].local.position = Vec3{0.0f, j == 0 ? 0.0f : 0.25f, 0.0f};
  }
  assets::Skin skin;
  skin.name = "crowd";
  for (u32 j = 0; j < k_joints; ++j)
    skin.joints.push_back(static_cast<i32>(j));
  mesh.skins.push_back(skin);

  assets::Animation animation;
  animation.name = "run";
  animation.duration = 1.0f;
  for (u32 j = 0; j < k_joints; ++j) {
    assets::AnimationSampler sampler;
    sampler.interpolation = assets::k_interp_linear;
    sampler.components = 4;
    sampler.times.push_back(0.0f);
    sampler.times.push_back(1.0f);
    const f32 quarter = 0.70710678f;
    const f32 keys[8] = {0, 0, 0, 1, 0, 0, quarter, quarter};
    for (const f32 v : keys)
      sampler.values.push_back(v);
    assets::AnimationChannel channel;
    channel.node = static_cast<i32>(j);
    channel.sampler = j;
    channel.path = assets::k_path_rotation;
    animation.samplers.push_back(sampler);
    animation.channels.push_back(channel);
  }
  mesh.animations.push_back(animation);

  std::string error;
  if (!library.add(mesh, "crowd", nullptr, &error)) bench::keep(error);
}

// One world, its capability and its population, kept for the life of the process: building 10^4
// entities costs far more than the tick being measured, and every repeat would otherwise pay it.
struct Scene {
  Library library;
  ecs::SimWorldConfig config;
  ecs::SimWorld sim;
  AnimationSystem animation;

  explicit Scene(const u32 share[k_tier_count])
      : config(make_config()),
        sim(config),
        animation(library, AnimationConfig{instances() * k_joints}) {
    build_library(library);
    animation.install(sim);
    const Id128 skeleton = Library::skeleton_id("crowd/crowd");
    const Id128 clip = Library::clip_id("crowd/run");
    u32 total = 0;
    for (u32 t = 0; t < k_tier_count; ++t)
      total += share[t];
    const u32 count = instances();
    for (u32 i = 0; i < count; ++i) {
      const flecs::entity entity = sim.world().entity();
      animation.attach(entity, skeleton, clip);
      entity.try_get_mut<AnimationPlayer>()->time = static_cast<f32>(i % 97) / 97.0f;
      u32 pick = (i * total) / count;
      u8 tier = 0;
      for (u32 t = 0; t < k_tier_count; ++t) {
        if (pick < share[t]) {
          tier = static_cast<u8>(t);
          break;
        }
        pick -= share[t];
      }
      animation.set_tier(entity, tier);
    }
  }

  static ecs::SimWorldConfig make_config() {
    ecs::SimWorldConfig out;
    out.hz = k_hz;
    return out;
  }
};

// Scenes are expensive to build and are reused across repeats, so one is kept per tier mix for the
// life of the process.
Scene& scene_for(const u32 share[k_tier_count]) {
  static std::vector<std::unique_ptr<Scene>> cache;
  static std::vector<u64> keys;
  const u64 key = (u64{share[0]} << 48) | (u64{share[1]} << 32) | (u64{share[2]} << 16) | share[3];
  for (usize i = 0; i < keys.size(); ++i) {
    if (keys[i] == key) return *cache[i];
  }
  cache.push_back(std::make_unique<Scene>(share));
  keys.push_back(key);
  return *cache.back();
}

void run_mix(bench::State& state, const u32 share[k_tier_count]) {
  Scene& scene = scene_for(share);
  while (state.keep_running()) {
    scene.sim.step();
    bench::keep(scene.animation.stats().sampled);
  }
  state.set_items(instances());
}

}  // namespace

// Every instance at LOD0: one tick of a 10^4-strong crowd with nothing coarsened. The ceiling.
ENGINE_BENCH(animation_tick_lod0, "animation.tick.lod0") {
  const u32 share[k_tier_count] = {1, 0, 0, 0};
  run_mix(state, share);
}

// Every instance at LOD1: half the sampling rate, no cross-fade interpolation, still skinned.
ENGINE_BENCH(animation_tick_lod1, "animation.tick.lod1") {
  const u32 share[k_tier_count] = {0, 1, 0, 0};
  run_mix(state, share);
}

// Every instance at LOD2: a quarter of the sampling rate and no skinning matrices.
ENGINE_BENCH(animation_tick_lod2, "animation.tick.lod2") {
  const u32 share[k_tier_count] = {0, 0, 1, 0};
  run_mix(state, share);
}

// Every instance at LOD3: no slot, no pose, no matrices. What a capability costs when its
// instances exist and none of them is worth animating — the floor the LOD policy is bounded by,
// and the per-tick half of plan 11 §11.10's "absent capabilities are free".
ENGINE_BENCH(animation_tick_lod3, "animation.tick.lod3") {
  const u32 share[k_tier_count] = {0, 0, 0, 1};
  run_mix(state, share);
}

// The mix a scene actually has: a few characters near the camera, most of the crowd beyond it.
ENGINE_BENCH(animation_tick_mixed, "animation.tick.mixed") {
  const u32 share[k_tier_count] = {5, 15, 30, 50};
  run_mix(state, share);
}

// The pool on its own, with no world and no ECS: what acquiring and releasing a slot costs, which
// is what a camera cut across a tier boundary pays per instance.
ENGINE_BENCH_ARGS(animation_pool_churn, "animation.pool.churn", 256, 4096, 65536) {
  const u32 slot_count = static_cast<u32>(state.arg());
  PosePool pool;
  std::vector<u32> slots(slot_count, k_no_slot);
  for (u32 i = 0; i < slot_count; ++i)
    slots[i] = pool.acquire(0, k_joints);
  while (state.keep_running()) {
    for (u32 i = 0; i < slot_count; ++i)
      pool.release(slots[i]);
    for (u32 i = 0; i < slot_count; ++i)
      slots[i] = pool.acquire(0, k_joints);
    bench::keep(pool.live_count());
  }
  state.set_items(slot_count);
}
