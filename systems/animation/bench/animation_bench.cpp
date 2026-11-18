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
#include <systems/animation/graph.h>

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
  // A second clip for the graph's blends (S35 stage 2): the same rig and keys, a turn about x, so a
  // blend of the two is a rotation neither clip has. The tick rows above play only "run".
  animation.name = "walk";
  for (assets::AnimationSampler& sampler : animation.samplers) {
    sampler.values[4] = 0.70710678f;
    sampler.values[6] = 0.0f;
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

// What measuring one clip's travel costs (`clip_displacement_bound`, A47): the number engine-view
// computes for an actor's tier row and a skinned instance's cull padding, the first time a clip is
// played (docs/subsystems/animation.md, "What a clip's first play costs"). The fixture is the size
// of a game animal as an exporter writes it: a chain of `joints` joints with a translation, a
// rotation and a scale track on every one, each **baked** at 30 keys a second over two seconds, the
// shape Blender's glTF exporter gives every channel by default (the forest's bird rigs are 37
// joints with seven such clips each). The spheres are a few centimetres round each joint, which
// moves nothing in the cost: the walk is over times and tracks, not over vertices.
namespace {

struct TravelFixture {
  Library library;
  JointBounds bounds;
};

TravelFixture& travel_fixture(u32 joints, u32 keys) {
  static std::vector<std::unique_ptr<TravelFixture>> cache;
  static std::vector<u64> ids;
  const u64 id = (u64{joints} << 32) | keys;
  for (usize i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) return *cache[i];
  }
  auto out = std::make_unique<TravelFixture>();
  assets::MeshData mesh;
  mesh.nodes.resize(joints);
  for (u32 j = 0; j < joints; ++j) {
    mesh.nodes[j].name = "joint" + std::to_string(j);
    mesh.nodes[j].parent = j == 0 ? -1 : static_cast<i32>(j - 1);
    mesh.nodes[j].local.position = Vec3{0.0f, j == 0 ? 0.0f : 0.1f, 0.0f};
  }
  assets::Skin skin;
  skin.name = "animal";
  for (u32 j = 0; j < joints; ++j)
    skin.joints.push_back(static_cast<i32>(j));
  mesh.skins.push_back(skin);
  assets::Animation animation;
  animation.name = "baked";
  const f32 rate = 30.0f;
  animation.duration = static_cast<f32>(keys - 1) / rate;
  for (u32 j = 0; j < joints; ++j) {
    for (u32 path = 0; path < 3; ++path) {
      assets::AnimationSampler sampler;
      sampler.interpolation = assets::k_interp_linear;
      sampler.components = static_cast<u8>(path == 1 ? 4 : 3);
      for (u32 k = 0; k < keys; ++k) {
        sampler.times.push_back(static_cast<f32>(k) / rate);
        // A swing of a few tenths of a radian about z, a small bob, and unit scale: any values
        // cost the same, so these only have to be a valid pose.
        const f32 angle = 0.3f * static_cast<f32>((k + j) % 17) / 17.0f;
        if (path == 0) {
          sampler.values.push_back(0.0f);
          sampler.values.push_back(j == 0 ? 0.0f : 0.1f + 0.002f * static_cast<f32>(k % 5));
          sampler.values.push_back(0.0f);
        } else if (path == 1) {
          sampler.values.push_back(0.0f);
          sampler.values.push_back(0.0f);
          sampler.values.push_back(angle * 0.5f);
          sampler.values.push_back(1.0f - angle * angle * 0.125f);
        } else {
          for (u32 c = 0; c < 3; ++c)
            sampler.values.push_back(1.0f);
        }
      }
      assets::AnimationChannel channel;
      channel.node = static_cast<i32>(j);
      channel.sampler = static_cast<u32>(animation.samplers.size());
      channel.path = path == 0   ? assets::k_path_translation
                     : path == 1 ? assets::k_path_rotation
                                 : assets::k_path_scale;
      animation.samplers.push_back(sampler);
      animation.channels.push_back(channel);
    }
  }
  mesh.animations.push_back(animation);
  std::string error;
  if (!out->library.add(mesh, "animal", nullptr, &error)) bench::keep(error);
  out->bounds.sphere.resize(joints);
  for (u32 j = 0; j < joints; ++j)
    out->bounds.sphere[j] = Vec4{0.0f, 0.1f * static_cast<f32>(j), 0.0f, 0.05f};
  cache.push_back(std::move(out));
  ids.push_back(id);
  return *cache.back();
}

void run_travel(bench::State& state, u32 joints, u32 keys) {
  TravelFixture& f = travel_fixture(joints, keys);
  const anim::Skeleton& skeleton = f.library.skeleton_data(0);
  const anim::Clip& clip = f.library.clip_data(0);
  while (state.keep_running())
    bench::keep(clip_displacement_bound(skeleton, clip, f.bounds));
  state.set_items(1);
}

}  // namespace

// By the clip's length: 37 joints, 111 tracks, one, two, four and eight seconds of keys at 30 a
// second (the smoke run takes the shortest).
ENGINE_BENCH_ARGS(animation_travel_keys, "animation.travel.keys", 31, 61, 121, 241) {
  const u32 keys = static_cast<u32>(state.arg());
  run_travel(state, 37, bench::smoke_mode() ? 31u : keys);
}

// The graph's control pass (S35 stage 1, graph.h) over `n` instances of the walker's graph: per
// instance, the inputs written (moving and fast flipping on a per-instance cadence, so a few
// percent of the instances change state each tick and the rest stay), the transitions evaluated,
// the clocks and the phase advanced. No pose: stage 1's pass decides states and hands the player
// its calls, so this is the whole of what a graph adds to a tick before the sampler runs.
ENGINE_BENCH_ARGS(animation_graph_control, "animation.graph.control", 1000, 10000, 100000) {
  const u32 n = bench::smoke_mode() ? 256u : static_cast<u32>(state.arg());
  CompiledGraph graph;
  std::string why;
  if (!compile_graph(locomotion_graph({"idle", "walk", "run", 0.2f}), nullptr, graph, why)) {
    bench::keep(why);
    return;
  }
  std::vector<AnimationGraphInput> inputs(n);
  std::vector<AnimationGraphState> states(n);
  std::vector<GraphStep> steps(n);
  for (u32 i = 0; i < n; ++i)
    init_graph_instance(graph, inputs[i], states[i]);
  u32 tick = 0;
  while (state.keep_running()) {
    for (u32 i = 0; i < n; ++i) {
      // Each instance flips `moving` every 37 + (i % 61) ticks and `fast` at half that rate.
      const u32 period = 37u + (i % 61u);
      const u32 at = tick + i;
      inputs[i].values[k_locomotion_moving] = ((at / period) & 1u) != 0 ? 1.0f : 0.0f;
      inputs[i].values[k_locomotion_fast] = ((at / (2u * period)) & 1u) != 0 ? 1.0f : 0.0f;
      inputs[i].values[k_locomotion_phase] = static_cast<f32>(at % 64u) * (1.0f / 64.0f);
    }
    bench::keep(graph_control(graph, inputs, states, 1.0f / 64.0f, steps));
    ++tick;
  }
  state.set_items(n);
}

// The pose program (S35 stage 2, graph.h) over `n` graph-driven instances of the 23-joint rig: a
// graph of `stand` (one clip) and `move` (a Blend1D of walk and run by `speed`, led sync group),
// cross-fades of 0.25 s between them. The instances' states are made once by 40 control ticks with
// inputs spread per instance (most moving at a speed between the two clips, so most evaluate a
// two-clip blend; a few percent mid-fade, so they evaluate both states), then frozen: a row
// measures the program alone, `batch` instances at a time, into a pose arena of its own. Beside it,
// `animation.graph.control` above is the control pass over the same population.
namespace {

struct GraphFixture {
  Library library;
  CompiledGraph graph;
  std::vector<AnimationGraphInput> inputs;
  std::vector<AnimationGraphState> states;
  std::vector<Vec3> translation;
  std::vector<Quat> rotation;
  std::vector<Vec3> scale;
  std::vector<GraphInstance> instances;
};

GraphDesc crowd_graph() {
  GraphDesc d;
  u64 low = 1;
  const auto id = [&] { return Id128::from_parts(0xb3c4, low++); };
  d.id = id();
  d.graph.name = "crowd";
  for (const char* name : {"moving", "speed"}) {
    auto& p = d.params.emplace_back();
    p.id = id();
    p.parent = d.id;
    p.record.name = name;
  }
  d.params[0].record.kind = GraphParamKind::Bool;
  auto& m = d.machines.emplace_back();
  m.id = id();
  m.parent = d.id;
  m.record.name = "body";
  auto& stand = d.states.emplace_back();
  stand.id = id();
  stand.parent = m.id;
  stand.record.name = "stand";
  auto& sn = d.nodes.emplace_back();
  sn.id = id();
  sn.parent = stand.id;
  sn.record.clip = "crowd/walk";
  auto& move = d.states.emplace_back();
  move.id = id();
  move.parent = m.id;
  move.record.name = "move";
  auto& blend = d.nodes.emplace_back();
  blend.id = id();
  blend.parent = move.id;
  blend.record.kind = GraphNodeKind::Blend1D;
  blend.record.param = "speed";
  blend.record.sync_group = 1;
  const Id128 blend_id = blend.id;
  for (const char* clip : {"crowd/walk", "crowd/run"}) {
    auto& c = d.nodes.emplace_back();
    c.id = id();
    c.parent = blend_id;
    c.record.clip = clip;
    d.nodes[1].record.inputs.push_back(c.id);
  }
  d.nodes[1].record.thresholds = {1.0f, 3.0f};
  d.machines[0].record.entry = d.states[0].id;
  for (u32 k = 0; k < 2; ++k) {
    auto& t = d.transitions.emplace_back();
    t.id = id();
    t.parent = d.machines[0].id;
    t.record.from = d.states[k].id;
    t.record.to = d.states[1 - k].id;
    t.record.duration_s = 0.25f;
    t.record.conditions.push_back(
        GraphCondition{"moving", GraphCompare::Equal, k == 0 ? 1.0f : 0.0f});
  }
  return d;
}

GraphFixture& graph_fixture(u32 n) {
  static std::vector<std::unique_ptr<GraphFixture>> cache;
  static std::vector<u32> sizes;
  for (usize i = 0; i < sizes.size(); ++i) {
    if (sizes[i] == n) return *cache[i];
  }
  auto f = std::make_unique<GraphFixture>();
  build_library(f->library);
  std::string why;
  if (!compile_graph(crowd_graph(), &f->library, f->graph, why)) bench::keep(why);
  f->inputs.resize(n);
  f->states.resize(n);
  std::vector<GraphStep> steps(n);
  for (u32 i = 0; i < n; ++i)
    init_graph_instance(f->graph, f->inputs[i], f->states[i]);
  for (u32 tick = 0; tick < 40; ++tick) {
    for (u32 i = 0; i < n; ++i) {
      // Moving for all but one tick in 37 of a per-instance cycle, at a speed spread over [0, 4].
      const u32 period = 37u + (i % 61u);
      f->inputs[i].values[0] = ((tick + i) % period) != 0 ? 1.0f : 0.0f;
      f->inputs[i].values[1] = static_cast<f32>(i % 401u) * (4.0f / 400.0f);
    }
    (void)graph_control(f->graph, f->inputs, f->states, 1.0f / 64.0f, steps);
  }
  const u32 joints = f->library.skeleton_data(0).joint_count();
  f->translation.resize(usize{n} * joints);
  f->rotation.resize(usize{n} * joints);
  f->scale.resize(usize{n} * joints);
  f->instances.resize(n);
  for (u32 i = 0; i < n; ++i) {
    GraphInstance& gi = f->instances[i];
    gi.input = &f->inputs[i];
    gi.state = &f->states[i];
    gi.lead = 1.0f / 64.0f;
    gi.out = anim::PoseView{std::span<Vec3>(f->translation.data() + usize{i} * joints, joints),
                            std::span<Quat>(f->rotation.data() + usize{i} * joints, joints),
                            std::span<Vec3>(f->scale.data() + usize{i} * joints, joints)};
  }
  cache.push_back(std::move(f));
  sizes.push_back(n);
  return *cache.back();
}

void run_program(bench::State& state, u32 n, u32 batch) {
  GraphFixture& f = graph_fixture(n);
  PoseEvaluator evaluator;
  const anim::Skeleton& skeleton = f.library.skeleton_data(0);
  while (state.keep_running()) {
    evaluator.evaluate(f.graph, f.library, skeleton, f.instances, batch);
    bench::keep(f.rotation[0].w);
  }
  state.set_items(n);
}

}  // namespace

// By the population, at the default batch of 64 (`animation.graph.batch`).
ENGINE_BENCH_ARGS(animation_graph_sample, "animation.graph.sample", 1000, 10000) {
  run_program(state, bench::smoke_mode() ? 256u : static_cast<u32>(state.arg()), 64);
}

// By the batch, at 10^4 instances: what the default is chosen from (the design's L2 sizing).
ENGINE_BENCH_ARGS(animation_graph_batch, "animation.graph.batch", 16, 64, 256) {
  run_program(state, bench::smoke_mode() ? 256u : 10000u, static_cast<u32>(state.arg()));
}

// By the rig: two seconds at 30 keys a second, 23 joints (the humanoid core), 37 (a bird of the
// forest's roster) and 74 (a rig with fingers and a face).
ENGINE_BENCH_ARGS(animation_travel_joints, "animation.travel.joints", 23, 37, 74) {
  const u32 joints = static_cast<u32>(state.arg());
  run_travel(state, bench::smoke_mode() ? 23u : joints, 61);
}
