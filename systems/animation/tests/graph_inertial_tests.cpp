// Inertialization in the graph, S35 stage 4 (docs/subsystems/animation.md, "Graphs:
// inertialization"): an `Inertialize` transition through the scheduled control and sample passes,
// against the same graph with `Cut` transitions — the cut jumps where the inertialization does
// not, the two are the same bits once the duration has passed, an interruption carries on, and at
// LOD1 an inertialization is the cut and holds no slot.
#include "animation_glb.h"

#include <core/hash/hash.h>
#include <core/ids/id128.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/world_commands.h>
#include <systems/animation/animation.h>
#include <systems/animation/graph.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

struct Rig {
  engine::test::TempDir tmp{"engine_animation_graph_inertial"};
  Library library;
  Id128 skeleton = Library::skeleton_id("rig/bar_skin");
  Rig() {
    const std::string path = tmp.file("rig.glb");
    REQUIRE(test_fixture::write_animated_glb(path));
    std::string why;
    REQUIRE_MESSAGE(library.load_gltf(path, "rig", nullptr, &why), why);
  }
};

// One machine: `stand` (the idle clip) and `move` (the walk clip), stand -> move while `moving`
// and move -> stand while not, each a `blend` of 0.25 s.
GraphDesc two_states(u64 seed, GraphBlend blend) {
  GraphDesc d;
  u64 low = 1;
  const auto id = [&] { return Id128::from_parts(seed, low++); };
  d.id = id();
  d.graph.name = blend == GraphBlend::Inertialize ? "inertial" : "cut";
  auto& p = d.params.emplace_back();
  p.id = id();
  p.parent = d.id;
  p.record.name = "moving";
  p.record.kind = GraphParamKind::Bool;
  auto& m = d.machines.emplace_back();
  m.id = id();
  m.parent = d.id;
  m.record.name = "body";
  const auto state = [&](const char* name, const char* clip) {
    auto& s = d.states.emplace_back();
    s.id = id();
    s.parent = m.id;
    s.record.name = name;
    auto& n = d.nodes.emplace_back();
    n.id = id();
    n.parent = s.id;
    n.record.clip = clip;
    return s.id;
  };
  const Id128 stand = state("stand", "rig/idle");
  const Id128 move = state("move", "rig/walk");
  d.machines[0].record.entry = stand;
  const auto into = [&](const Id128& from, const Id128& to, f32 moving) {
    auto& t = d.transitions.emplace_back();
    t.id = id();
    t.parent = d.machines[0].id;
    t.record.from = from;
    t.record.to = to;
    t.record.duration_s = 0.25f;
    t.record.blend = blend;
    t.record.conditions.push_back(GraphCondition{"moving", GraphCompare::Equal, moving});
  };
  into(stand, move, 1.0f);
  into(move, stand, 0.0f);
  return d;
}

using Matrices = std::vector<f32>;

Matrices matrices_of(const AnimationSystem& animation, const Id128& entity) {
  u32 first = 0;
  u32 count = 0;
  REQUIRE(animation.joint_run(entity, first, count));
  const f32* p = reinterpret_cast<const f32*>(animation.joint_matrices().data() + first);
  return Matrices(p, p + count * (sizeof(anim::JointMatrix) / sizeof(f32)));
}

f32 largest_change(const Matrices& a, const Matrices& b) {
  f32 d = 0.0f;
  for (usize i = 0; i < a.size(); ++i)
    d = std::max(d, std::fabs(a[i] - b[i]));
  return d;
}

bool same_bits(const Matrices& a, const Matrices& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0;
}

struct World {
  Rig rig;
  ecs::SimWorld sim;
  AnimationSystem animation;
  GraphDesc inertial = two_states(0x5d40, GraphBlend::Inertialize);
  GraphDesc cut = two_states(0x5d41, GraphBlend::Cut);
  static ecs::SimWorldConfig config() {
    ecs::SimWorldConfig c;
    c.hz = 64;
    return c;
  }
  World() : sim(config()), animation(rig.library) {
    animation.install(sim);
    std::string why;
    REQUIRE_MESSAGE(animation.add_graph(inertial, why), why);
    REQUIRE_MESSAGE(animation.add_graph(cut, why), why);
  }
  Id128 entity(u64 n, const GraphDesc& graph) {
    ecs::WorldCommands commands(sim.world());
    const Id128 e = Id128::from_seed(0x5d42, n);
    commands.create(e);
    commands.apply();
    REQUIRE(animation.attach_graph(e, graph.id, rig.skeleton));
    return e;
  }
  void moving(std::initializer_list<Id128> entities, bool on) {
    const f32 values[1] = {on ? 1.0f : 0.0f};
    for (const Id128& e : entities)
      REQUIRE(animation.set_graph_inputs(e, values, 0));
  }
};

}  // namespace

TEST_CASE(
    "animation graph inertial: the cut jumps, the inertialization does not, then they agree") {
  World w;
  const Id128 c = w.entity(1, w.cut);
  CHECK(w.animation.inertial_bytes() == 0);  // a world whose graphs inertialize nothing holds none
  const Id128 a = w.entity(2, w.inertial);
  w.moving({a, c}, false);
  std::vector<Matrices> as;
  std::vector<Matrices> cs;
  const auto tick = [&] {
    w.sim.step();
    as.push_back(matrices_of(w.animation, a));
    cs.push_back(matrices_of(w.animation, c));
  };
  for (u32 i = 0; i < 12; ++i)
    tick();
  // Before any transition the two graphs are one pose.
  CHECK(same_bits(as.back(), cs.back()));
  f32 a_before = 0.0f;
  f32 c_before = 0.0f;
  for (usize k = as.size() - 4; k < as.size(); ++k) {
    a_before = std::max(a_before, largest_change(as[k - 1], as[k]));
    c_before = std::max(c_before, largest_change(cs[k - 1], cs[k]));
  }

  // The transition's tick: the cut shows the walk at once; the inertialization shows the idle
  // moving on as it was (its offsets put the source back), so it moves no more than it did.
  w.moving({a, c}, true);
  tick();
  const f32 a_switch = largest_change(as[as.size() - 2], as.back());
  const f32 c_switch = largest_change(cs[cs.size() - 2], cs.back());
  MESSAGE("steps before (inertial, cut): " << a_before << ", " << c_before
                                           << "; at the switch: " << a_switch << ", " << c_switch);
  CHECK(c_switch > 4.0f * c_before + 1.0e-4f);  // the fixture's walk is not its idle
  CHECK(a_switch <= 2.0f * a_before + 1.0e-5f);
  CHECK(w.animation.inertial_slots_held() == 1);
  // While it lasts the inertialized pose is not the cut's; past its 0.25 s (16 ticks), the same
  // bits, and the slot is back.
  for (u32 i = 0; i < 8; ++i)
    tick();
  CHECK_FALSE(same_bits(as.back(), cs.back()));
  for (u32 i = 0; i < 10; ++i)
    tick();
  CHECK(same_bits(as.back(), cs.back()));
  CHECK(w.animation.inertial_slots_held() == 0);

  // Interrupted: back to stand, and four ticks into that, back to move again. The second
  // transition's source is what the first showed, so neither change jumps.
  w.moving({a, c}, false);
  tick();
  for (u32 i = 0; i < 4; ++i)
    tick();
  f32 a_flight = 0.0f;
  for (usize k = as.size() - 3; k < as.size(); ++k)
    a_flight = std::max(a_flight, largest_change(as[k - 1], as[k]));
  w.moving({a, c}, true);
  tick();
  const f32 a_interrupt = largest_change(as[as.size() - 2], as.back());
  MESSAGE("steps in flight: " << a_flight << "; at the interruption: " << a_interrupt);
  CHECK(a_interrupt <= 3.0f * a_flight + 1.0e-5f);
  CHECK(w.animation.inertial_slots_held() == 1);
  for (u32 i = 0; i < 18; ++i)
    tick();
  CHECK(same_bits(as.back(), cs.back()));
  CHECK(w.animation.inertial_slots_held() == 0);
}

TEST_CASE("animation graph inertial: at LOD1 an inertialization is the cut and holds no slot") {
  // Two worlds, one entity each under the same id (so the tier's divisor visits both on the same
  // ticks): one inertialized, one cut. At LOD1 (no interpolation) the first is the second's pose
  // at every tick, and holds no slot.
  World wi;
  World wc;
  const Id128 a = wi.entity(1, wi.inertial);
  const Id128 c = wc.entity(1, wc.cut);
  REQUIRE(a == c);
  wi.animation.set_tier(a, 1);
  wc.animation.set_tier(c, 1);
  wi.moving({a}, false);
  wc.moving({c}, false);
  for (u32 i = 0; i < 9; ++i) {
    wi.sim.step();
    wc.sim.step();
  }
  wi.moving({a}, true);
  wc.moving({c}, true);
  for (u32 i = 0; i < 24; ++i) {
    wi.sim.step();
    wc.sim.step();
    CHECK(same_bits(matrices_of(wi.animation, a), matrices_of(wc.animation, c)));
    CHECK(wi.animation.inertial_slots_held() == 0);
  }
}
TEST_CASE("animation graph inertial: Inertialize compiles on the base machine, kept as the blend") {
  // (On a layer machine it is refused by name: the offsets are the base machine's pose's.)
  GraphDesc d = two_states(0x5d43, GraphBlend::Inertialize);
  CompiledGraph g;
  std::string why;
  CHECK_MESSAGE(compile_graph(d, nullptr, g, why), why);
  CHECK(g.transitions[0].blend == static_cast<u8>(GraphBlend::Inertialize));
}

// ---- the walker's graph through its program (engine-view's `animation.graph.inertialize_walker`)

namespace {

// Three bodies on one 60 Hz world, as engine-view's animated world steps: `off`, the walker as it
// is with the tunable off — player-driven, each step of `locomotion_graph` played into its player
// and the phase-locked clips' playheads set from the phase (player.cpp, `tick_gait`); `on`, the
// same body made graph-driven by the graph with `Inertialize` transitions (`drive_by_graph`, the
// tunable on); `cut`, the same with `Cut` transitions, the target the inertialization lands on; and
// `fade`, made graph-driven by the off body's own cross-fade graph: what sampling the walker
// through its graph with the tunable off would show, which is not the off body's bits.
// The fixture's clips: `stand` the idle, `walk` the walk, `run` the idle again on the walk's phase
// (a pose unlike the walk's, so walk-to-run is a change too).
struct WalkerWorld {
  Rig rig;
  ecs::SimWorld sim;
  AnimationSystem animation;
  GraphDesc off = locomotion_graph({"rig/idle", "rig/walk", "rig/idle", 0.2f});
  GraphDesc on =
      locomotion_graph({"rig/idle", "rig/walk", "rig/idle", 0.2f, GraphBlend::Inertialize});
  GraphDesc cut = locomotion_graph({"rig/idle", "rig/walk", "rig/idle", 0.2f, GraphBlend::Cut});
  Id128 a = Id128::from_seed(0x5d50, 1);  // off
  Id128 b = Id128::from_seed(0x5d50, 2);  // on
  Id128 c = Id128::from_seed(0x5d50, 3);  // cut
  Id128 d = Id128::from_seed(0x5d50, 4);  // fade
  f64 phase = 0.0;
  static ecs::SimWorldConfig config() {
    ecs::SimWorldConfig k;
    k.hz = 60;
    return k;
  }
  WalkerWorld() : sim(config()), animation(rig.library) {
    animation.install(sim);
    ecs::WorldCommands commands(sim.world());
    for (const Id128& e : {a, b, c, d})
      commands.create(e);
    commands.apply();
    const Id128 idle = Library::clip_id("rig/idle");
    std::string why;
    REQUIRE_MESSAGE(animation.add_graph(off, why), why);
    REQUIRE_MESSAGE(animation.add_graph(on, rig.skeleton, why), why);
    REQUIRE_MESSAGE(animation.add_graph(cut, rig.skeleton, why), why);
    for (const Id128& e : {a, b, c, d})
      REQUIRE(animation.attach(e, rig.skeleton, idle));
    REQUIRE(animation.attach_graph(a, off.id));
    REQUIRE(animation.drive_by_graph(b, on.id, rig.skeleton));
    REQUIRE(animation.drive_by_graph(c, cut.id, rig.skeleton));
    REQUIRE(animation.drive_by_graph(d, off.id, rig.skeleton));
  }
  // One tick: the off body's control pass and steps as `tick_gait` makes them (the phase as the
  // tick began, the playheads at the phase after it), the graph-driven bodies' inputs (the phase
  // after it), then the world's step.
  void tick(bool moving, bool fast) {
    const f32 dt = 1.0f / 60.0f;
    const f32 start[k_locomotion_params] = {moving ? 1.0f : 0.0f, fast ? 1.0f : 0.0f,
                                            static_cast<f32>(phase)};
    GraphEnter enter[k_graph_max_machines];
    if (animation.control_graph(a, start, 0u, dt, enter) != 0)
      REQUIRE(animation.apply_graph_step(a, enter[0]));
    if (moving) {
      phase = phase + 0.005;  // 60 moving ticks: 0.3 of a cycle, so no clip wraps in the case
      phase = phase - std::floor(phase);
      AnimationPlayer p;
      REQUIRE(animation.player(a, p));
      const auto time_of = [&](const Id128& clip) {
        const u32 index = rig.library.clip_index(clip);
        return index == Library::k_not_found
                   ? 0.0f
                   : static_cast<f32>(phase *
                                      static_cast<f64>(rig.library.clip_data(index).duration));
      };
      REQUIRE(animation.set_playhead(a, time_of(p.clip), time_of(p.fade_clip), 0.0f));
    }
    const f32 end[k_locomotion_params] = {moving ? 1.0f : 0.0f, fast ? 1.0f : 0.0f,
                                          static_cast<f32>(phase)};
    REQUIRE(animation.set_graph_inputs(b, end, 0u));
    REQUIRE(animation.set_graph_inputs(c, end, 0u));
    REQUIRE(animation.set_graph_inputs(d, end, 0u));
    sim.step();
  }
};

// The largest change of one tick's change: |m(k+1) - 2 m(k) + m(k-1)| over the matrices' floats.
f32 largest_bend(const Matrices& before, const Matrices& at, const Matrices& after) {
  f32 d = 0.0f;
  for (usize i = 0; i < at.size(); ++i)
    d = std::max(d, std::fabs(after[i] - 2.0f * at[i] + before[i]));
  return d;
}

}  // namespace

TEST_CASE("animation graph inertial: the walker's graph, sampled and inertialized, has no jump") {
  WalkerWorld w;
  std::vector<Matrices> as;
  std::vector<Matrices> bs;
  std::vector<Matrices> cs;
  std::vector<Matrices> ds;
  const auto tick = [&](bool moving, bool fast) {
    w.tick(moving, fast);
    ds.push_back(matrices_of(w.animation, w.d));
    as.push_back(matrices_of(w.animation, w.a));
    bs.push_back(matrices_of(w.animation, w.b));
    cs.push_back(matrices_of(w.animation, w.c));
  };
  for (u32 i = 0; i < 16; ++i)
    tick(false, false);
  // Standing, the three are one pose: no transition has been taken past the first tick's cut.
  CHECK(same_bits(as.back(), bs.back()));
  CHECK(same_bits(bs.back(), cs.back()));
  CHECK(w.animation.inertial_slots_held() == 0);

  // The walker's three changes: stand to walk, walk to run, run to stand, each held 30 ticks
  // (half a second; the transitions last 0.2 s, 12 ticks).
  struct Change {
    const char* name;
    bool moving;
    bool fast;
  };
  const Change changes[] = {
      {"stand to walk", true, false}, {"walk to run", true, true}, {"run to stand", false, false}};
  for (const Change& change : changes) {
    const usize s = bs.size();  // the switch's tick
    f32 b_step = 0.0f;          // the largest step the on body took in the four ticks before
    f32 b_bend = 0.0f;          // and the largest change of its step
    for (usize k = s - 4; k < s; ++k) {
      b_step = std::max(b_step, largest_change(bs[k - 1], bs[k]));
      b_bend = std::max(b_bend, largest_bend(bs[k - 2], bs[k - 1], bs[k]));
    }
    for (u32 i = 0; i < 30; ++i)
      tick(change.moving, change.fast);
    const f32 c_jump = largest_change(cs[s - 1], cs[s]);
    const f32 b_switch = largest_change(bs[s - 1], bs[s]);
    const f32 b_switch_bend = largest_bend(bs[s - 1], bs[s], bs[s + 1]);
    f32 c_after = 0.0f;  // the largest step of the target (the cut body) after the switch
    for (usize k = s + 1; k < s + 13; ++k)
      c_after = std::max(c_after, largest_change(cs[k - 1], cs[k]));
    bool differs = false;  // from the off body, somewhere in the 0.2 s
    for (usize k = s; k < s + 12; ++k)
      differs = differs || !same_bits(as[k], bs[k]);
    MESSAGE(std::string(change.name)
            << ": cut jump " << c_jump << "; on: steps before " << b_step << ", at the switch "
            << b_switch << "; target steps " << c_after << "; bends before " << b_bend
            << ", at the switch " << b_switch_bend);
    // The fixture's clips differ, so the cut jumps.
    CHECK(c_jump > 4.0f * b_step + 1.0e-4f);
    // The tunable's side is not the off side's.
    CHECK(differs);
    // Continuous in value: at the switch the on body shows the source moving on as it was.
    CHECK(b_switch <= 2.0f * b_step + 1.0e-5f);
    // Continuous in velocity: the step after the switch is the step at it changed by no more than
    // the curve's own start can change it. The quintic starts at the offset x0 with the source's
    // velocity and an acceleration of (-8 v0 t1 - 20 x0) / t1^2 (inertialize.h), so over one tick
    // of dt it bends the step by at most (20 x0 + 8 |v0| t1) dt^2 / t1^2: at 60 Hz and t1 = 0.2 s,
    // x0 / 7.2 plus 0.67 |v0| dt. x0 is at most the cut's jump and |v0| dt at most the source's
    // and the target's steps, `b_step` and `c_after`; the rest is the bodies' own bending.
    const f32 curve = c_jump / 7.2f + 0.67f * (b_step + c_after);
    CHECK(b_switch_bend <= curve + 2.0f * b_bend + 1.0e-5f);
    // A cut bends by the whole jump: the bound is under it, so it tells the two apart.
    CHECK(curve + 2.0f * b_bend < c_jump);
    // After the duration (0.2 s: 12 ticks, the 13th past the switch on), the target to the bit.
    CHECK(same_bits(bs[s + 13], cs[s + 13]));
    CHECK(same_bits(bs.back(), cs.back()));
  }
  CHECK(w.animation.inertial_slots_held() == 0);

  // Why the tunable's off side keeps the player: the same cross-fade graph sampled through the
  // program is the player's pose to the bit while no fade is in flight (standing, above, and
  // graph_pose_tests.cpp), and not during one: the program blends by the normalized sum where the
  // player slerps (animation.md, "Graphs: the pose program"), a state keeps its own clock where
  // the walker's host puts both of a fade's playheads at the gait's phase (so the idle fading out
  // jumps to it, by 1.05 here), and the program's phase times a duration in f32 where the host
  // does it in f64 — so the walker's hashes would move.
  usize first = as.size();
  for (usize k = 0; k < as.size() && first == as.size(); ++k) {
    if (!same_bits(as[k], ds[k])) first = k;
  }
  REQUIRE(first < as.size());
  const auto hash = [](const Matrices& m) { return hash_bytes(m.data(), m.size() * sizeof(f32)); };
  MESSAGE("the off body and its graph sampled with the same cross-fades first differ at tick "
          << first << ": matrices hash " << hash(as[first]) << " (player) against "
          << hash(ds[first]) << " (program), largest difference "
          << largest_change(as[first], ds[first]));
  CHECK(first >= 16);  // not while standing
}
