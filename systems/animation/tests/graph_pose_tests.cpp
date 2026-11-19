// Animation graphs as data, stage 2 (S35; docs/subsystems/animation.md, "Graphs: the pose
// program"): the Blend1D and its sync groups, the pose program by the normalized weighted sum, and
// the two scheduled systems that run it for graph-driven entities that carry no player.
//
// What stays byte-identical, and the case that holds each: a blend at a weight of exactly 0 or 1 is
// the one clip's pose; a stage-1 graph run through the pose program is the pose of the player its
// steps drive, at every tick with no cross-fade in flight (a cross-fade is the weighted sum now,
// not the player's slerp: the one place the two may differ); one worker and eight, and any batch
// size, give the same bytes.
#include "animation_glb.h"

#include <core/hash/hash.h>
#include <core/ids/id128.h>
#include <core/jobs/job_system.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/world_commands.h>
#include <foundation/tunables/tunables.h>
#include <systems/animation/animation.h>
#include <systems/animation/graph.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

constexpr f32 k_dt = 1.0f / 64.0f;

struct Rig {
  engine::test::TempDir tmp{"engine_animation_graph_pose"};
  Library library;
  Id128 skeleton = Library::skeleton_id("rig/bar_skin");
  u32 skeleton_index = 0;
  u32 walk = 0;
  u32 idle = 0;
  Rig() {
    const std::string path = tmp.file("rig.glb");
    REQUIRE(test_fixture::write_animated_glb(path));
    std::string why;
    REQUIRE_MESSAGE(library.load_gltf(path, "rig", nullptr, &why), why);
    skeleton_index = library.skeleton_index(skeleton);
    walk = library.clip_index(Library::clip_id("rig/walk"));
    idle = library.clip_index(Library::clip_id("rig/idle"));
    REQUIRE(walk != Library::k_not_found);
    REQUIRE(idle != Library::k_not_found);
  }
  const anim::Skeleton& skel() const { return library.skeleton_data(skeleton_index); }
};

// A graph of one machine: parameters `moving` (bool) and `speed` (float, [0, 1]); states `stand`
// (the idle clip on its own clock) and `move` (a Blend1D of `inputs` by `speed`); stand -> move
// while moving and move -> stand while not, each a 0.25 s cross-fade. `synced`: the Blend1D is
// phase-locked to sync group 1, led (the machine has no phase_param).
struct BlendInput {
  const char* clip;
  f32 at;
  f32 rate = 1.0f;
};
GraphDesc blend_graph(std::initializer_list<BlendInput> inputs, bool synced, bool looping,
                      bool with_stand = true) {
  GraphDesc d;
  u64 low = 1;
  const auto id = [&] { return Id128::from_parts(0x5b2, low++); };
  d.id = id();
  d.graph.name = "blend";
  for (const char* name : {"moving", "speed"}) {
    auto& p = d.params.emplace_back();
    p.id = id();
    p.parent = d.id;
    p.record.name = name;
  }
  d.params[0].record.kind = GraphParamKind::Bool;
  d.params[1].record.range_min = 0.0f;
  d.params[1].record.range_max = 1.0f;
  auto& m = d.machines.emplace_back();
  m.id = id();
  m.parent = d.id;
  m.record.name = "body";
  const Id128 machine = m.id;
  Id128 stand;
  if (with_stand) {
    auto& s = d.states.emplace_back();
    s.id = id();
    s.parent = machine;
    s.record.name = "stand";
    stand = s.id;
    auto& n = d.nodes.emplace_back();
    n.id = id();
    n.parent = s.id;
    n.record.clip = "rig/idle";
  }
  auto& mv = d.states.emplace_back();
  mv.id = id();
  mv.parent = machine;
  mv.record.name = "move";
  const Id128 move = mv.id;
  auto& b = d.nodes.emplace_back();
  b.id = id();
  b.parent = move;
  b.record.kind = GraphNodeKind::Blend1D;
  b.record.param = "speed";
  b.record.looping = looping;
  b.record.sync_group = synced ? u8{1} : u8{0};
  const Id128 blend = b.id;
  for (const BlendInput& in : inputs) {
    auto& c = d.nodes.emplace_back();
    c.id = id();
    c.parent = blend;
    c.record.clip = in.clip;
    c.record.rate = in.rate;
    c.record.looping = looping;
    for (auto& node : d.nodes) {
      if (node.id == blend) {
        node.record.inputs.push_back(c.id);
        node.record.thresholds.push_back(in.at);
      }
    }
  }
  d.machines[0].record.entry = with_stand ? stand : move;
  if (with_stand) {
    const auto into = [&](const Id128& from, const Id128& to, f32 moving) {
      auto& t = d.transitions.emplace_back();
      t.id = id();
      t.parent = machine;
      t.record.from = from;
      t.record.to = to;
      t.record.duration_s = 0.25f;
      t.record.conditions.push_back(GraphCondition{"moving", GraphCompare::Equal, moving});
    };
    into(stand, move, 1.0f);
    into(move, stand, 0.0f);
  }
  return d;
}

CompiledGraph compiled(const GraphDesc& d, const Library* library) {
  CompiledGraph g;
  std::string why;
  REQUIRE_MESSAGE(compile_graph(d, library, g, why), why);
  return g;
}

// One instance evaluated into `out` with machine 0 in state `state` (an index) at `time` seconds.
void evaluate_one(const Rig& rig, const CompiledGraph& g, f32 speed, u32 state, f32 time,
                  anim::Pose& out) {
  AnimationGraphInput in;
  AnimationGraphState st;
  init_graph_instance(g, in, st);
  in.values[1] = speed;
  st.state[0] = static_cast<u16>(state + 1);
  st.time[0] = time;
  out.resize(rig.skel().joint_count());
  GraphInstance gi;
  gi.input = &in;
  gi.state = &st;
  gi.out = anim::PoseView(out);
  PoseEvaluator evaluator;
  evaluator.evaluate(g, rig.library, rig.skel(), std::span<const GraphInstance>(&gi, 1), 64);
}

bool same_pose(const anim::Pose& a, const anim::Pose& b) {
  const usize n = a.joint_count();
  return n == b.joint_count() &&
         std::memcmp(a.translation.data(), b.translation.data(), n * sizeof(Vec3)) == 0 &&
         std::memcmp(a.rotation.data(), b.rotation.data(), n * sizeof(Quat)) == 0 &&
         std::memcmp(a.scale.data(), b.scale.data(), n * sizeof(Vec3)) == 0;
}

u64 hash_pose(const anim::Skeleton& skeleton, const anim::Pose& pose, u64 seed) {
  const u32 joints = pose.joint_count();
  Vector<Mat4> model(joints, Mat4::identity());
  Vector<anim::JointMatrix> matrices(joints, anim::JointMatrix{});
  anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
  u64 h = hash_bytes(pose.translation.data(), sizeof(Vec3) * joints, seed);
  h = hash_bytes(pose.rotation.data(), sizeof(Quat) * joints, h);
  h = hash_bytes(pose.scale.data(), sizeof(Vec3) * joints, h);
  return hash_bytes(matrices.data(), sizeof(anim::JointMatrix) * joints, h);
}

u64 next_random(u64& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

}  // namespace

TEST_CASE("animation graph pose: a Blend1D compiles to a program, and what it cannot be is named") {
  Rig rig;
  const CompiledGraph g =
      compiled(blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, true, true), &rig.library);
  // stand: sample, accumulate; move: two (sample, accumulate) pairs, normalize, accumulate; the
  // output's normalize.
  REQUIRE(g.program.size() == 9);
  CHECK(g.registers == 3);
  CHECK(g.program_why.empty());
  CHECK(g.state(0, 1).blend);
  CHECK(g.state(0, 1).synced);
  CHECK(g.pose_clips[g.pose_states[1].first_clip + 1].duration == test_fixture::k_clip_seconds);
  CHECK(g.program[8].kind == static_cast<u8>(PoseOpKind::Normalize));
  MESSAGE("compiled blend graph bytes: " << g.bytes() << ", program " << g.program.size()
                                         << " ops");

  std::string why;
  CompiledGraph out;
  const auto refused = [&](const GraphDesc& d, const char* says) {
    CHECK_FALSE(compile_graph(d, &rig.library, out, why));
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
  };
  {
    GraphDesc d = blend_graph({{"rig/idle", 0.5f}, {"rig/walk", 0.5f}}, false, true);
    refused(d, "thresholds are finite and increasing");
  }
  {
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true);
    d.nodes[1].record.thresholds.pop_back();
    refused(d, "inputs, thresholds and child nodes are one each");
  }
  {
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true);
    d.nodes[2].record.sync_group = 1;
    refused(d, "takes its sync group and looping from the Blend1D");
  }
  {
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true);
    d.nodes[1].record.param = "moving";
    refused(d, "reads a float parameter");
  }
  {  // a node under a Clip is no stage's (stage 3 places layer nodes under machines only)
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true);
    auto& extra = d.nodes.emplace_back();
    extra.id = Id128::from_parts(0x5b3, 1);
    extra.parent = d.nodes[2].id;
    refused(d, "or a machine's layer node is in no stage of the design yet");
  }
  {
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/sprint", 1.0f}}, false, true);
    refused(d, "plays clip 'rig/sprint', not in the library");
  }
  {  // seventeen inputs
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}}, false, true);
    for (u32 i = 1; i < 17; ++i) {
      auto& c = d.nodes.emplace_back();
      c.id = Id128::from_parts(0x5b4, i);
      c.parent = d.nodes[1].id;
      c.record.clip = "rig/walk";
      d.nodes[1].record.inputs.push_back(c.id);
      d.nodes[1].record.thresholds.push_back(static_cast<f32>(i));
    }
    refused(d, "a Blend1D of 17 inputs (1 to 16)");
  }
  {  // two machines and no layer node compile their tables, and no program
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true);
    auto& m = d.machines.emplace_back();
    m.id = Id128::from_parts(0x5b5, 1);
    m.parent = d.id;
    m.record.name = "upper";
    auto& s = d.states.emplace_back();
    s.id = Id128::from_parts(0x5b5, 2);
    s.parent = m.id;
    s.record.name = "hold";
    auto& n = d.nodes.emplace_back();
    n.id = Id128::from_parts(0x5b5, 3);
    n.parent = s.id;
    m.record.entry = s.id;
    const CompiledGraph two = compiled(d, &rig.library);
    CHECK(two.program.empty());
    CHECK(two.program_why.find("no layer node: composing their poses is a layer's") !=
          std::string::npos);
  }
  {  // a state cross-faded into itself
    GraphDesc d = blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true);
    d.transitions[0].record.to = d.transitions[0].record.from;
    const CompiledGraph self = compiled(d, &rig.library);
    CHECK(self.program.empty());
    CHECK(self.program_why.find("cross-fades into itself") != std::string::npos);
  }
}

TEST_CASE(
    "animation graph pose: at a weight of exactly 0 or 1 a blend is the one clip, to the bit") {
  Rig rig;
  const anim::Skeleton& skel = rig.skel();
  const auto direct = [&](u32 clip, f32 time, bool looping) {
    anim::Pose p;
    anim::rest_pose(skel, p);
    rig.library.clip_data(clip).sample(time, p, looping);
    return p;
  };
  const CompiledGraph two =
      compiled(blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, true), &rig.library);
  const CompiledGraph three = compiled(
      blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 0.5f}, {"rig/idle", 1.0f}}, false, true),
      &rig.library);
  anim::Pose got;
  for (const f32 time : {0.0f, 0.3f, 0.71875f, 1.5f}) {
    evaluate_one(rig, two, 0.0f, 1, time, got);
    CHECK(same_pose(got, direct(rig.idle, time, true)));
    evaluate_one(rig, two, 1.0f, 1, time, got);
    CHECK(same_pose(got, direct(rig.walk, time, true)));
    evaluate_one(rig, two, -3.0f, 1, time, got);  // beyond an end: that end's clip
    CHECK(same_pose(got, direct(rig.idle, time, true)));
    evaluate_one(rig, three, 0.5f, 1, time, got);  // exactly at an inner threshold
    CHECK(same_pose(got, direct(rig.walk, time, true)));
    // A Clip state with no transition in flight is the clip, as a player's sample is.
    evaluate_one(rig, two, 0.5f, 0, time, got);
    CHECK(same_pose(got, direct(rig.idle, time, true)));
  }
  // Between thresholds it is neither clip; at the midpoint of two rotations the normalized sum is
  // the slerp's midpoint (both bisect the arc), to the float's rounding.
  evaluate_one(rig, two, 0.5f, 1, 1.0f, got);
  anim::Pose a = direct(rig.idle, 1.0f, false);
  anim::Pose b = direct(rig.walk, 1.0f, false);
  CHECK_FALSE(same_pose(got, a));
  CHECK_FALSE(same_pose(got, b));
}

TEST_CASE("animation graph pose: a blend of two clips at three weights is pinned (MSVC)") {
  // The pin: the Blend1D of idle and walk, not looping, at one second — both clips at their last
  // key, so the sampler takes no slerp and the pose is the program's arithmetic alone
  // (+ - * / sqrt, no C library). Taken on MSVC; GCC and Clang are the stack's to confirm.
  Rig rig;
  const CompiledGraph g = compiled(
      blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 1.0f}}, false, false, false), &rig.library);
  anim::Pose pose;
  u64 h = 0;
  for (const f32 w : {0.25f, 0.5f, 0.75f}) {
    evaluate_one(rig, g, w, 0, 1.0f, pose);
    h = hash_pose(rig.skel(), pose, h);
  }
  // Pinned 2026-11-18 on MSVC (msvc-debug, x86-64-v3) when the program was written (S35 stage 2).
  MESSAGE("blend pin: " << h);
  CHECK(h == 2432560345335567898ull);

  // And what it is: the tip's rotation is the normalized sum of the identity and a quarter turn,
  // the root's translation the weighted sum of the origin and (0, 4, 0).
  evaluate_one(rig, g, 0.5f, 0, 1.0f, pose);
  CHECK(pose.translation[0].y == 2.0f);
  const f32 s = std::sqrt(0.5f);  // the quarter turn's components
  const f32 len = std::sqrt((1.0f + s) * (1.0f + s) + s * s);
  CHECK(std::fabs(pose.rotation[1].z - s / len) < 1e-6f);
}

TEST_CASE("animation graph pose: a stage-1 graph through the program is the pose its player gets") {
  // Two entities on one rig and one stage-1 graph (Clip states, own clocks, cross-fades): one
  // graph-driven and sampled by the program, the other driven by the same graph into a player as
  // stage 1 does. At every tick with no cross-fade in flight their bone matrices are the same
  // bytes; during a fade the program's weighted sum and the player's slerp may differ.
  Rig rig;
  ecs::SimWorldConfig config;
  config.hz = 64;
  ecs::SimWorld sim(config);
  AnimationSystem animation(rig.library);
  animation.install(sim);
  ecs::WorldCommands commands(sim.world());
  const Id128 sampled = Id128::from_seed(0x5b6, 1);
  const Id128 played = Id128::from_seed(0x5b6, 2);
  commands.create(sampled);
  commands.create(played);
  commands.apply();

  // Clip states only: `move` is a Clip of the walk (a Blend1D of one input is not a stage-1 graph).
  GraphDesc d = blend_graph({{"rig/walk", 0.0f}}, false, true);
  d.nodes[1].record = d.nodes[2].record;  // the Blend1D node becomes the Clip
  d.nodes.pop_back();
  std::string why;
  REQUIRE_MESSAGE(animation.add_graph(d, why), why);
  REQUIRE(animation.attach(played, rig.skeleton, Id128{}));
  REQUIRE(animation.attach_graph(played, d.id));
  CHECK_FALSE(animation.attach_graph(played, d.id, rig.skeleton));  // it has a player
  REQUIRE(animation.attach_graph(sampled, d.id, rig.skeleton));

  u64 seed = 0x77u;
  bool moving = false;
  u32 compared = 0;
  for (u32 tick = 0; tick < 600; ++tick) {
    if ((next_random(seed) % 41u) == 0) moving = !moving;
    const f32 values[2] = {moving ? 1.0f : 0.0f, 0.0f};
    REQUIRE(animation.set_graph_inputs(sampled, values, 0));
    GraphEnter entered[k_graph_max_machines];
    if (animation.control_graph(played, values, 0, k_dt, entered) != 0)
      REQUIRE(animation.apply_graph_step(played, entered[0]));
    sim.step();
    AnimationPlayer p;
    REQUIRE(animation.player(played, p));
    if (!p.fade_clip.is_null()) continue;
    u32 fa = 0;
    u32 ca = 0;
    u32 fb = 0;
    u32 cb = 0;
    REQUIRE(animation.joint_run(sampled, fa, ca));
    REQUIRE(animation.joint_run(played, fb, cb));
    REQUIRE(ca == cb);
    CHECK(std::memcmp(animation.joint_matrices().data() + fa,
                      animation.joint_matrices().data() + fb, ca * sizeof(anim::JointMatrix)) == 0);
    ++compared;
  }
  CHECK(animation.stats().graph_controlled == 1);
  CHECK(animation.stats().graph_sampled == 1);
  MESSAGE("ticks compared with no fade in flight: " << compared);
  CHECK(compared > 300);
}

TEST_CASE("animation graph pose: a led sync group advances at its leader's rate") {
  Rig rig;
  // idle at rate 2 below, walk at rate 1 above: the clip of the larger weight leads.
  const CompiledGraph g =
      compiled(blend_graph({{"rig/idle", 0.0f, 2.0f}, {"rig/walk", 1.0f, 1.0f}}, true, true, false),
               &rig.library);
  CHECK(g.machines[0].phase_param == k_graph_no_param);
  CHECK(g.machines[0].sync_group == 0);
  for (const f32 speed : {0.25f, 0.75f}) {
    AnimationGraphInput in;
    AnimationGraphState st;
    init_graph_instance(g, in, st);
    in.values[1] = speed;
    GraphStep steps[k_graph_max_machines];
    for (u32 i = 0; i < 8; ++i)
      (void)graph_control(g, in, st, k_dt, steps);
    // Eight ticks of 1/64 s at 2 or 1 cycles a second (each clip lasts one second).
    CHECK(st.phase[0] == (speed < 0.5f ? 0.25f : 0.125f));
  }
}

TEST_CASE("animation graph pose: one worker and eight, any batch, two runs: the same bytes") {
  Rig rig;
  const GraphDesc desc =
      blend_graph({{"rig/idle", 0.0f, 2.0f}, {"rig/walk", 1.0f, 1.0f}}, true, true);
  constexpr u32 k_instances = 96;

  const auto run = [&](u32 workers, i64 batch, std::vector<anim::JointMatrix>& out,
                       std::vector<AnimationGraphState>& states) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    ecs::JobOsApi adapter(js);
    ecs::SimWorldConfig wc;
    wc.hz = 64;
    ecs::SimWorld sim(wc);
    AnimationSystem animation(rig.library);
    animation.install(sim);
    if (workers > 1) {
      const u32 requested = workers <= adapter.max_workers() ? workers : adapter.max_workers();
      REQUIRE(ecs::set_workers(sim.world(), requested));
    }
    REQUIRE(tunables::find("animation.graph.batch") != nullptr);
    REQUIRE(tunables::find("animation.graph.batch")->set_from_text(std::to_string(batch), nullptr));
    std::string why;
    REQUIRE_MESSAGE(animation.add_graph(desc, why), why);
    ecs::WorldCommands commands(sim.world());
    std::vector<Id128> ids;
    for (u32 i = 0; i < k_instances; ++i) {
      ids.push_back(Id128::from_seed(0x5b7, i));
      commands.create(ids.back());
    }
    commands.apply();
    for (u32 i = 0; i < k_instances; ++i) {
      REQUIRE(animation.attach_graph(ids[i], desc.id, rig.skeleton));
      if (i % 5 == 4) animation.set_tier(ids[i], 1);  // a cross-fade cut at its midpoint
    }
    u64 seed = 0x5eedu;
    for (u32 tick = 0; tick < 48; ++tick) {
      for (u32 i = 0; i < k_instances; ++i) {
        const u64 r = next_random(seed);
        const f32 values[2] = {((r >> 5) % 7u) != 0 ? 1.0f : 0.0f,
                               static_cast<f32>(r % 1000u) / 999.0f};
        REQUIRE(animation.set_graph_inputs(ids[i], values, 0));
      }
      sim.step();
    }
    const std::span<const anim::JointMatrix> m = animation.joint_matrices();
    out.assign(m.begin(), m.end());
    states.clear();
    for (const Id128& id : ids) {
      AnimationGraphState st;
      REQUIRE(animation.graph_state(id, st));
      states.push_back(st);
    }
  };
  std::vector<anim::JointMatrix> one;
  std::vector<anim::JointMatrix> again;
  std::vector<anim::JointMatrix> eight;
  std::vector<anim::JointMatrix> small;
  std::vector<AnimationGraphState> s1;
  std::vector<AnimationGraphState> s2;
  std::vector<AnimationGraphState> s8;
  std::vector<AnimationGraphState> s7;
  run(1, 64, one, s1);
  run(1, 64, again, s2);
  run(8, 64, eight, s8);
  run(1, 7, small, s7);
  REQUIRE(tunables::find("animation.graph.batch")->set_from_text("64", nullptr));
  REQUIRE(one.size() == k_instances * 2);
  REQUIRE(eight.size() == one.size());
  REQUIRE(small.size() == one.size());
  const usize bytes = one.size() * sizeof(anim::JointMatrix);
  CHECK(std::memcmp(one.data(), again.data(), bytes) == 0);
  CHECK(std::memcmp(one.data(), eight.data(), bytes) == 0);
  CHECK(std::memcmp(one.data(), small.data(), bytes) == 0);
  const usize sbytes = s1.size() * sizeof(AnimationGraphState);
  CHECK(std::memcmp(s1.data(), s8.data(), sbytes) == 0);
  CHECK(std::memcmp(s1.data(), s7.data(), sbytes) == 0);
}

TEST_CASE(
    "animation graph pose: a graph-driven entity promoted from LOD3 lands where ticking would") {
  Rig rig;
  ecs::SimWorldConfig config;
  config.hz = 64;
  ecs::SimWorld sim(config);
  AnimationSystem animation(rig.library);
  animation.install(sim);
  const GraphDesc desc =
      blend_graph({{"rig/idle", 0.0f, 1.0f}, {"rig/walk", 1.0f, 1.0f}}, true, true, false);
  std::string why;
  REQUIRE_MESSAGE(animation.add_graph(desc, why), why);
  ecs::WorldCommands commands(sim.world());
  const Id128 ticked = Id128::from_seed(0x5b8, 1);
  const Id128 frozen = Id128::from_seed(0x5b8, 2);
  commands.create(ticked);
  commands.create(frozen);
  commands.apply();
  REQUIRE(animation.attach_graph(ticked, desc.id, rig.skeleton));
  REQUIRE(animation.attach_graph(frozen, desc.id, rig.skeleton));
  const f32 values[2] = {1.0f, 0.375f};
  REQUIRE(animation.set_graph_inputs(ticked, values, 0));
  REQUIRE(animation.set_graph_inputs(frozen, values, 0));
  for (u32 i = 0; i < 5; ++i)
    sim.step();
  animation.set_tier(frozen, 3);
  u32 first = 0;
  u32 count = 0;
  CHECK_FALSE(animation.joint_run(frozen, first, count));  // nothing held but its components
  for (u32 i = 0; i < 97; ++i)
    sim.step();
  animation.set_tier(frozen, 0);
  sim.step();
  AnimationGraphState a;
  AnimationGraphState b;
  REQUIRE(animation.graph_state(ticked, a));
  REQUIRE(animation.graph_state(frozen, b));
  CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
  u32 fa = 0;
  u32 fb = 0;
  REQUIRE(animation.joint_run(ticked, fa, count));
  REQUIRE(animation.joint_run(frozen, fb, count));
  CHECK(std::memcmp(animation.joint_matrices().data() + fa, animation.joint_matrices().data() + fb,
                    count * sizeof(anim::JointMatrix)) == 0);
}

// Review of batch 69, finding 2 (b70): a graph replaced in place under its id left its instances'
// indices pointing into the old compile, and a smaller one was read out of range — the control
// pass's `g.states[...]` and the evaluator's `pose_states[...]` (each an `ENGINE_ASSERT` of
// `Vector` in a debug build, so the old fault aborts this case there), and the `code_` write past
// the program those drove (a raw pointer: only AddressSanitizer would see that one in a release
// build). Now every instance of the replaced graph starts again at the entries before an index is
// read: the pose evaluated before the next control tick is the entry's, and after it the state is.
TEST_CASE("animation graph pose: a graph replaced by a smaller one restarts its instances") {
  Rig rig;
  GraphLibrary graphs;
  const GraphDesc big =
      blend_graph({{"rig/idle", 0.0f}, {"rig/walk", 0.5f}, {"rig/idle", 0.75f}, {"rig/walk", 1.0f}},
                  true, true);
  const GraphDesc small = blend_graph({{"rig/idle", 0.0f}}, false, true, false);
  REQUIRE(big.id == small.id);
  const u32 at = graphs.add(compiled(big, &rig.library));
  const CompiledGraph* g = graphs.find(big.id);
  REQUIRE(g != nullptr);
  const u32 old_revision = g->revision;
  CHECK(old_revision != 0);
  REQUIRE(g->machines[0].state_count == 2);

  // Eight instances, into `move` (index 1) by a cross-fade still in flight.
  constexpr u32 k_n = 8;
  std::vector<AnimationGraphInput> inputs(k_n);
  std::vector<AnimationGraphState> states(k_n);
  std::vector<GraphStep> steps(k_n);
  for (u32 i = 0; i < k_n; ++i)
    init_graph_instance(*g, inputs[i], states[i]);
  graph_control(*g, inputs, states, k_dt, steps);
  for (u32 i = 0; i < k_n; ++i) {
    const f32 values[2] = {1.0f, 0.125f * static_cast<f32>(i)};
    set_graph_inputs(*g, inputs[i], values, 0);
  }
  for (u32 t = 0; t < 3; ++t)
    graph_control(*g, inputs, states, k_dt, steps);
  for (const AnimationGraphState& st : states) {
    REQUIRE(st.next[0] == 2);  // heading to `move`, past the one state the replacement has
    REQUIRE(st.revision == old_revision);
  }

  // Replaced in place: the same index, the same object, another revision.
  CHECK(graphs.add(compiled(small, &rig.library)) == at);
  CHECK(graphs.find(small.id) == g);
  REQUIRE(g->machines[0].state_count == 1);
  REQUIRE(g->pose_clips.size() == 1);
  CHECK(g->revision != old_revision);

  // Evaluated before any control tick: every pose is the entry's, as a fresh instance's is.
  const u32 joints = rig.skel().joint_count();
  std::vector<anim::Pose> poses(k_n);
  std::vector<GraphInstance> instances(k_n);
  for (u32 i = 0; i < k_n; ++i) {
    poses[i].resize(joints);
    instances[i].input = &inputs[i];
    instances[i].state = &states[i];
    instances[i].lead = k_dt;
    instances[i].out = anim::PoseView(poses[i]);
  }
  PoseEvaluator evaluator;
  evaluator.evaluate(*g, rig.library, rig.skel(), instances, 4);
  AnimationGraphInput fresh_in;
  AnimationGraphState fresh;
  init_graph_instance(*g, fresh_in, fresh);
  anim::Pose entry;
  entry.resize(joints);
  GraphInstance one;
  one.input = &fresh_in;
  one.state = &fresh;
  one.lead = k_dt;
  one.out = anim::PoseView(entry);
  evaluator.evaluate(*g, rig.library, rig.skel(), std::span<const GraphInstance>(&one, 1), 4);
  for (u32 i = 0; i < k_n; ++i)
    CHECK(same_pose(poses[i], entry));

  // The next control tick starts every machine at its entry, with a cut, under the new revision.
  graph_control(*g, inputs, states, k_dt, steps);
  for (u32 i = 0; i < k_n; ++i) {
    CHECK(states[i].revision == g->revision);
    CHECK(states[i].state[0] == g->machines[0].entry + 1u);
    CHECK(states[i].next[0] == 0);
    CHECK(steps[i].first == 1);
    CHECK(steps[i].blend == static_cast<u8>(GraphBlend::Cut));
  }
  // A catch-up for a state of another revision does nothing (the next control tick resets it).
  AnimationGraphState stale = states[0];
  stale.revision = old_revision;
  stale.state[0] = 2;
  const AnimationGraphState before = stale;
  graph_catch_up(*g, inputs[0], stale, 1.0f);
  CHECK(std::memcmp(&stale, &before, sizeof(stale)) == 0);
}
