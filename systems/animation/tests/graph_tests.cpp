// Animation graphs as data, stage 1 (S35; docs/subsystems/animation.md, "Graphs"): the records, the
// compile to a control table, the control pass, and the walker's machine driven through it.
//
// The walker's own cases (apps/engine_view's player_gait, player_view, player_rest and walk tests)
// pin its summary and pose hashes through a device; the case here that stands for them on the CPU
// is "the walker's graph makes the calls tick_gait's machine made": the machine engine-view had
// before stage 1, written out again below, and the graph, fed the same ticks, issue the same calls
// in the same order with the same arguments, so the player receives the same calls in the same
// ticks and every hash downstream of it is unchanged by construction.
#include "animation_glb.h"
#include "pose_hash.h"

#include <core/ids/id128.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/world_commands.h>
#include <systems/animation/animation.h>
#include <systems/animation/graph.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

// ---- writing a graph into a document as records ------------------------------------------------

template <class T>
void create(doc::Document& d, const Id128& id, const Id128& parent, const T& record) {
  REQUIRE(d.apply(doc::cmd_create(id, schema::type_of<T>().qualified_name, parent,
                                  schema::to_json(record)),
                  nullptr, nullptr));
}

void write_records(doc::Document& d, const GraphDesc& g) {
  create(d, g.id, Id128{}, g.graph);
  for (const auto& p : g.params)
    create(d, p.id, p.parent, p.record);
  for (const auto& m : g.machines)
    create(d, m.id, m.parent, m.record);
  for (const auto& s : g.states)
    create(d, s.id, s.parent, s.record);
  for (const auto& n : g.nodes)
    create(d, n.id, n.parent, n.record);
  for (const auto& t : g.transitions)
    create(d, t.id, t.parent, t.record);
}

template <class T>
bool same_bytes(const Vector<T>& a, const Vector<T>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool same_tables(const CompiledGraph& a, const CompiledGraph& b) {
  if (a.id != b.id || a.name != b.name || a.params.size() != b.params.size()) return false;
  for (u32 i = 0; i < a.params.size(); ++i) {
    if (a.params[i].name != b.params[i].name || a.params[i].kind != b.params[i].kind ||
        a.params[i].lo != b.params[i].lo || a.params[i].hi != b.params[i].hi)
      return false;
  }
  if (a.state_info.size() != b.state_info.size()) return false;
  for (u32 i = 0; i < a.state_info.size(); ++i) {
    const StateInfo& x = a.state_info[i];
    const StateInfo& y = b.state_info[i];
    if (x.id != y.id || x.name != y.name || x.clip != y.clip || x.rate != y.rate ||
        x.looping != y.looping || x.synced != y.synced)
      return false;
  }
  return same_bytes(a.machines, b.machines) && same_bytes(a.states, b.states) &&
         same_bytes(a.transitions, b.transitions) && same_bytes(a.conditions, b.conditions);
}

// ---- the machine engine-view's PlayerBody::tick_gait had before stage 1 --------------------------
//
// Copied from apps/engine_view/player.cpp at 5d0634bb (the block between `const u8 gait` and the
// `if (!moving) return;` that follows it), with the actors' calls recorded instead of made.

struct Call {
  enum Kind : u8 { Play, Speed, Stop } kind = Play;
  std::string clip;
  f32 value = 0.0f;  // the fade for Play and Stop, the rate for Speed
  bool looping = false;
  bool operator==(const Call&) const = default;
};

struct OldMachine {
  u8 gait = 0;
  u64 changes = 0;
  void tick(bool moving, bool sprint, const LocomotionClips& c, std::vector<Call>& out) {
    const u8 g = moving ? (sprint ? u8{3} : u8{1}) : u8{2};
    if (g == gait) return;
    const f32 fade = gait == 0 ? 0.0f : c.fade_s;
    if (gait != 0) ++changes;
    gait = g;
    if (moving) {
      out.push_back({Call::Play, std::string(sprint ? c.run : c.walk), fade, true});
    } else if (!c.idle.empty()) {
      out.push_back({Call::Play, std::string(c.idle), fade, true});
      out.push_back({Call::Speed, {}, 1.0f, false});
    } else {
      out.push_back({Call::Stop, {}, fade, false});
    }
  }
};

// The graph's side: the hook engine-view's tick_gait now has, with the same calls recorded.
struct GraphMachineHost {
  CompiledGraph graph;
  AnimationGraphInput input;
  AnimationGraphState state;
  u64 changes = 0;
  explicit GraphMachineHost(const LocomotionClips& c) {
    std::string why;
    REQUIRE_MESSAGE(compile_graph(locomotion_graph(c), nullptr, graph, why), why);
    init_graph_instance(graph, input, state);
  }
  void tick(bool moving, bool sprint, f32 dt, std::vector<Call>& out) {
    const f32 values[k_locomotion_params] = {moving ? 1.0f : 0.0f, sprint ? 1.0f : 0.0f, 0.0f};
    set_graph_inputs(graph, input, values, 0);
    GraphStep steps[k_graph_max_machines];
    if (graph_control(graph, input, state, dt, steps) == 0) return;
    const GraphEnter e = graph_enter(graph, 0, steps[0]);
    if (!e.first) ++changes;
    if (e.state->clip.empty()) {
      out.push_back({Call::Stop, {}, e.fade_s, false});
    } else {
      out.push_back({Call::Play, e.state->clip, e.fade_s, e.state->looping});
      if (!e.state->synced) out.push_back({Call::Speed, {}, e.state->rate, false});
    }
  }
};

u64 next_random(u64& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

GraphDesc two_state_graph() {
  GraphDesc d;
  d.id = Id128::from_parts(0x9a, 1);
  d.graph.name = "test";
  auto& p = d.params.emplace_back();
  p.id = Id128::from_parts(0x9a, 2);
  p.parent = d.id;
  p.record.name = "x";
  auto& m = d.machines.emplace_back();
  m.id = Id128::from_parts(0x9a, 3);
  m.parent = d.id;
  m.record.name = "m";
  for (u64 i = 0; i < 3; ++i) {
    auto& s = d.states.emplace_back();
    s.id = Id128::from_parts(0x9a, 10 + i);
    s.parent = m.id;
    s.record.name = std::string(1, static_cast<char>('a' + i));
    auto& n = d.nodes.emplace_back();
    n.id = Id128::from_parts(0x9a, 20 + i);
    n.parent = s.id;
  }
  d.machines[0].record.entry = d.states[0].id;
  return d;
}

GraphItem<GraphTransition>& add_transition(GraphDesc& d, u64 low, const Id128& from, const Id128& to,
                                           i32 priority) {
  auto& t = d.transitions.emplace_back();
  t.id = Id128::from_parts(0x9a, low);
  t.parent = d.machines[0].id;
  t.record.from = from;
  t.record.to = to;
  t.record.priority = priority;
  t.record.duration_s = 0.25f;
  t.record.conditions.push_back(GraphCondition{"x", GraphCompare::Greater, 0.5f});
  return t;
}

}  // namespace

TEST_CASE("animation graph: the walker's graph as records in a document compiles as the one built in code") {
  const LocomotionClips clips{"rig/idle", "rig/walk", "rig/run", 0.2f};
  const GraphDesc built = locomotion_graph(clips);
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  write_records(d, built);

  GraphDesc read;
  std::string why;
  REQUIRE_MESSAGE(read_graph(d, built.id, read, why), why);
  CHECK(read.params.size() == 3);
  CHECK(read.machines.size() == 1);
  CHECK(read.states.size() == 3);
  CHECK(read.nodes.size() == 3);
  CHECK(read.transitions.size() == 3);

  CompiledGraph a;
  CompiledGraph b;
  REQUIRE_MESSAGE(compile_graph(built, nullptr, a, why), why);
  REQUIRE_MESSAGE(compile_graph(read, nullptr, b, why), why);
  CHECK(same_tables(a, b));

  // The parameters' slots are their declaration order, and the states sort stand, walk, run.
  CHECK(a.param_index("moving") == k_locomotion_moving);
  CHECK(a.param_index("fast") == k_locomotion_fast);
  CHECK(a.param_index("phase") == k_locomotion_phase);
  CHECK(a.state_index(0, "stand") == 0);
  CHECK(a.state_index(0, "walk") == 1);
  CHECK(a.state_index(0, "run") == 2);
  CHECK(a.machines[0].any_count == 3);
  CHECK(a.state(0, 1).synced);
  CHECK(a.state(0, 2).synced);
  CHECK_FALSE(a.state(0, 0).synced);

  // A record that is not a graph, and an id that names nothing, are refused by name.
  CHECK_FALSE(read_graph(d, built.states[0].id, read, why));
  CHECK(why.find("not an engine.animation.Graph") != std::string::npos);
  CHECK_FALSE(read_graph(d, Id128::from_parts(7, 7), read, why));

  // What a compiled graph and an instance hold (docs/subsystems/animation.md, "Graphs").
  MESSAGE("compiled walker graph bytes: " << a.bytes());
  CHECK(a.bytes() < 2048);
  CHECK(sizeof(AnimationGraphInput) + sizeof(AnimationGraphState) == 120);
  GraphLibrary empty;
  CHECK(empty.bytes() == 0);
}

TEST_CASE("animation graph: the walker's graph makes the calls tick_gait's machine made") {
  for (const bool with_idle : {true, false}) {
    const LocomotionClips clips{with_idle ? "rig/idle" : "", "rig/gait-walk", "rig/gait-run", 0.2f};
    OldMachine old;
    GraphMachineHost graph(clips);
    std::vector<Call> want;
    std::vector<Call> got;
    u64 seed = with_idle ? 0x5eedu : 0xfeedu;
    bool moving = false;
    bool sprint = false;
    for (u32 tick = 0; tick < 20000; ++tick) {
      // Runs of a few ticks each, so a fade is often changed mid-flight (the 0.2 s fade is 12.8
      // ticks at 64 Hz) and every pair of the three gaits follows every other.
      const u64 r = next_random(seed);
      if ((r & 7u) == 0) moving = !moving;
      if (((r >> 3) & 7u) == 0) sprint = !sprint;
      old.tick(moving, moving && sprint, clips, want);
      graph.tick(moving, moving && sprint, 1.0f / 64.0f, got);
    }
    REQUIRE(want.size() == got.size());
    CHECK(want == got);
    CHECK(old.changes == graph.changes);
    CHECK(old.changes > 1000);
  }
}

TEST_CASE("animation graph: the first tick enters with a cut, at the entry or past it") {
  const LocomotionClips clips{"rig/idle", "rig/walk", "rig/run", 0.2f};
  for (const bool moving : {false, true}) {
    GraphMachineHost host(clips);
    std::vector<Call> calls;
    host.tick(moving, false, 1.0f / 64.0f, calls);
    REQUIRE(!calls.empty());
    CHECK(calls[0].kind == Call::Play);
    CHECK(calls[0].clip == (moving ? "rig/walk" : "rig/idle"));
    CHECK(calls[0].value == 0.0f);
    CHECK(host.changes == 0);
    CHECK(host.state.state[0] == (moving ? 2 : 1));
    // Nothing changes while the inputs do not.
    calls.clear();
    host.tick(moving, false, 1.0f / 64.0f, calls);
    CHECK(calls.empty());
  }
}

TEST_CASE("animation graph: transitions are taken by priority, then declaration order, any first") {
  GraphDesc d = two_state_graph();
  const Id128 a = d.states[0].id;
  const Id128 b = d.states[1].id;
  const Id128 c = d.states[2].id;
  add_transition(d, 50, a, b, 1);  // a -> b at priority 1
  add_transition(d, 40, a, c, 0);  // a -> c at priority 0: wins over a -> b
  CompiledGraph g;
  std::string why;
  REQUIRE_MESSAGE(compile_graph(d, nullptr, g, why), why);
  AnimationGraphInput in;
  AnimationGraphState st;
  init_graph_instance(g, in, st);
  GraphStep step[k_graph_max_machines];
  CHECK(graph_control(g, in, st, 0.01f, step) == 1u);  // the first tick: the entry, a
  CHECK(step[0].entered == 1);
  in.values[0] = 1.0f;
  CHECK(graph_control(g, in, st, 0.01f, step) == 1u);
  CHECK(step[0].entered == 3);  // c, by priority
  CHECK(step[0].fade_s == 0.25f);
  CHECK(step[0].blend == static_cast<u8>(GraphBlend::CrossFade));

  // At one priority, the transition declared first (the smaller id) wins.
  GraphDesc e = two_state_graph();
  add_transition(e, 60, e.states[0].id, e.states[2].id, 0);
  add_transition(e, 30, e.states[0].id, e.states[1].id, 0);
  REQUIRE_MESSAGE(compile_graph(e, nullptr, g, why), why);
  init_graph_instance(g, in, st);
  in.values[0] = 1.0f;
  CHECK(graph_control(g, in, st, 0.01f, step) == 1u);
  CHECK(step[0].first == 1);
  CHECK(step[0].entered == 2);  // b, declared first; and taken at once on the first tick, a cut
  CHECK(step[0].fade_s == 0.0f);

  // An `any` transition is evaluated before the state's own, whatever their priorities.
  GraphDesc f = two_state_graph();
  add_transition(f, 70, f.states[0].id, f.states[1].id, -5);
  add_transition(f, 80, Id128{}, f.states[2].id, 9);
  REQUIRE_MESSAGE(compile_graph(f, nullptr, g, why), why);
  init_graph_instance(g, in, st);
  CHECK(graph_control(g, in, st, 0.01f, step) == 1u);
  in.values[0] = 1.0f;
  CHECK(graph_control(g, in, st, 0.01f, step) == 1u);
  CHECK(step[0].entered == 3);
  // ... and never into the state the machine is heading to.
  CHECK(graph_control(g, in, st, 0.01f, step) == 0u);
}

TEST_CASE("animation graph: a transition in flight finishes into its state; a trigger is taken once") {
  GraphDesc d = two_state_graph();
  auto& tp = d.params.emplace_back();
  tp.id = Id128::from_parts(0x9a, 5);
  tp.parent = d.id;
  tp.record.name = "go";
  tp.record.kind = GraphParamKind::Trigger;
  auto& t = add_transition(d, 90, d.states[0].id, d.states[1].id, 0);
  t.record.conditions[0] = GraphCondition{"go", GraphCompare::Equal, 1.0f};
  CompiledGraph g;
  std::string why;
  REQUIRE_MESSAGE(compile_graph(d, nullptr, g, why), why);
  REQUIRE(g.transitions[0].clears == 2u);
  AnimationGraphInput in;
  AnimationGraphState st;
  init_graph_instance(g, in, st);
  GraphStep step[k_graph_max_machines];
  (void)graph_control(g, in, st, 0.0625f, step);
  const f32 none[1] = {0.0f};
  set_graph_inputs(g, in, none, 2u);
  CHECK(graph_control(g, in, st, 0.0625f, step) == 1u);
  CHECK(in.triggers == 0u);
  CHECK(st.state[0] == 1);
  CHECK(st.next[0] == 2);
  for (u32 i = 0; i < 4; ++i)
    CHECK(graph_control(g, in, st, 0.0625f, step) == 0u);
  CHECK(st.state[0] == 2);  // 0.25 s in four ticks of 1/16: done
  CHECK(st.next[0] == 0);

  // Values are clamped as they are written; a bool is 0 or 1.
  GraphDesc l = locomotion_graph({"", "w", "r", 0.2f});
  REQUIRE_MESSAGE(compile_graph(l, nullptr, g, why), why);
  init_graph_instance(g, in, st);
  const f32 values[3] = {7.0f, 0.0f, 1.5f};
  set_graph_inputs(g, in, values, 0);
  CHECK(in.values[0] == 1.0f);
  CHECK(in.values[2] == 1.0f);
  (void)graph_control(g, in, st, 0.0625f, step);
  CHECK(st.phase[0] == 0.0f);  // the phase wraps into [0, 1)
}

TEST_CASE("animation graph: the compile refuses what stage 1 does not build, by name") {
  std::string why;
  CompiledGraph g;
  const auto refused = [&](const GraphDesc& d, const char* says) {
    CHECK_FALSE(compile_graph(d, nullptr, g, why));
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
  };
  {  // a ninth parameter
    GraphDesc d = two_state_graph();
    for (u64 i = 0; i < 8; ++i) {
      auto& p = d.params.emplace_back();
      p.id = Id128::from_parts(0x9b, i);
      p.parent = d.id;
      p.record.name = "p" + std::to_string(i);
    }
    refused(d, "9 parameters, past the limit of 8");
  }
  {  // a third machine
    GraphDesc d = two_state_graph();
    for (u64 i = 0; i < 2; ++i) {
      auto& m = d.machines.emplace_back();
      m.id = Id128::from_parts(0x9c, i);
      m.parent = d.id;
      m.record.name = "extra";
    }
    refused(d, "3 machines, past the limit of 2");
  }
  {  // a transition to a state of another machine
    GraphDesc d = two_state_graph();
    auto& m = d.machines.emplace_back();
    m.id = Id128::from_parts(0x9c, 9);
    m.parent = d.id;
    m.record.name = "other";
    auto& s = d.states.emplace_back();
    s.id = Id128::from_parts(0x9c, 10);
    s.parent = m.id;
    s.record.name = "elsewhere";
    auto& n = d.nodes.emplace_back();
    n.id = Id128::from_parts(0x9c, 11);
    n.parent = s.id;
    m.record.entry = s.id;
    add_transition(d, 99, d.states[0].id, s.id, 0);
    refused(d, "goes to a state not of this machine");
  }
  {  // stage 2's and 4's
    GraphDesc d = two_state_graph();
    add_transition(d, 99, d.states[0].id, d.states[1].id, 0).record.blend = GraphBlend::Inertialize;
    refused(d, "Inertialize is stage 4's");
    d.transitions[0].record.blend = GraphBlend::CrossFade;
    d.transitions[0].record.exit_phase = 0.5f;
    refused(d, "exit_phase is stage 2's");
    d.transitions[0].record.exit_phase.reset();
    d.nodes[0].record.kind = GraphNodeKind::Blend1D;
    refused(d, "a node other than a Clip");
  }
  {  // a condition on a parameter that is not declared
    GraphDesc d = two_state_graph();
    add_transition(d, 99, d.states[0].id, d.states[1].id, 0).record.conditions[0].param = "y";
    refused(d, "reads parameter 'y', not declared");
  }
}

TEST_CASE("animation graph: an unknown clip is refused at compile against the library") {
  engine::test::TempDir tmp{"engine_animation_graph"};
  const std::string path = tmp.file("rig.glb");
  REQUIRE(test_fixture::write_animated_glb(path));
  Library library;
  std::string why;
  REQUIRE_MESSAGE(library.load_gltf(path, "rig", nullptr, &why), why);
  CompiledGraph g;
  REQUIRE_MESSAGE(compile_graph(locomotion_graph({"rig/idle", "rig/walk", "rig/walk", 0.2f}),
                                &library, g, why),
                  why);
  CHECK(g.state(0, 1).clip_index != Library::k_not_found);
  CHECK_FALSE(compile_graph(locomotion_graph({"rig/idle", "rig/walk", "rig/sprint", 0.2f}), &library,
                            g, why));
  CHECK(why.find("plays clip 'rig/sprint', not in the library") != std::string::npos);
}

TEST_CASE("animation graph: one worker and eight give the same bytes") {
  const CompiledGraph g = [] {
    CompiledGraph out;
    std::string why;
    REQUIRE(compile_graph(locomotion_graph({"i", "w", "r", 0.2f}), nullptr, out, why));
    return out;
  }();
  constexpr u32 k_n = 4096;
  std::vector<AnimationGraphInput> in1(k_n);
  std::vector<AnimationGraphState> st1(k_n);
  for (u32 i = 0; i < k_n; ++i)
    init_graph_instance(g, in1[i], st1[i]);
  std::vector<AnimationGraphInput> in8 = in1;
  std::vector<AnimationGraphState> st8 = st1;
  std::vector<GraphStep> steps1(k_n);
  std::vector<GraphStep> steps8(k_n);
  u64 seed = 0xabcdefu;
  for (u32 tick = 0; tick < 64; ++tick) {
    for (u32 i = 0; i < k_n; ++i) {
      const u64 r = next_random(seed);
      const f32 v[3] = {static_cast<f32>(r & 1u), static_cast<f32>((r >> 1) & 1u),
                        static_cast<f32>(r % 1000u) / 1000.0f};
      set_graph_inputs(g, in1[i], v, 0);
      set_graph_inputs(g, in8[i], v, 0);
    }
    (void)graph_control(g, in1, st1, 1.0f / 64.0f, steps1);
    std::vector<std::thread> workers;
    for (u32 w = 0; w < 8; ++w) {
      workers.emplace_back([&, w] {
        const usize first = usize{w} * (k_n / 8);
        (void)graph_control(g, std::span(in8).subspan(first, k_n / 8),
                            std::span(st8).subspan(first, k_n / 8), 1.0f / 64.0f,
                            std::span(steps8).subspan(first, k_n / 8));
      });
    }
    for (std::thread& t : workers)
      t.join();
    REQUIRE(std::memcmp(steps1.data(), steps8.data(), k_n * sizeof(GraphStep)) == 0);
  }
  CHECK(std::memcmp(st1.data(), st8.data(), k_n * sizeof(AnimationGraphState)) == 0);
  CHECK(std::memcmp(in1.data(), in8.data(), k_n * sizeof(AnimationGraphInput)) == 0);
}

TEST_CASE("animation graph: a graph-driven entity's player and pose are what play() gives") {
  engine::test::TempDir tmp{"engine_animation_graph_entity"};
  const std::string path = tmp.file("rig.glb");
  REQUIRE(test_fixture::write_animated_glb(path));
  Library library;
  std::string why;
  REQUIRE_MESSAGE(library.load_gltf(path, "rig", nullptr, &why), why);
  const Id128 skeleton = Library::skeleton_id("rig/bar_skin");
  ecs::SimWorldConfig config;
  config.hz = 64;
  ecs::SimWorld sim(config);
  AnimationSystem animation(library);
  animation.install(sim);
  CHECK(animation.graphs().size() == 0);
  CHECK(animation.graphs().bytes() == 0);

  ecs::WorldCommands commands(sim.world());
  const Id128 driven = Id128::from_seed(0x9a9a, 1);
  const Id128 played = Id128::from_seed(0x9a9a, 2);
  commands.create(driven);
  commands.create(played);
  commands.apply();
  REQUIRE(animation.attach(driven, skeleton, Id128{}));
  REQUIRE(animation.attach(played, skeleton, Id128{}));

  const GraphDesc desc = locomotion_graph({"rig/idle", "rig/walk", "rig/walk", 0.2f});
  REQUIRE_MESSAGE(animation.add_graph(desc, why), why);
  CHECK_FALSE(animation.has_graph(driven));
  REQUIRE(animation.attach_graph(driven, desc.id));
  CHECK(animation.has_graph(driven));
  CHECK_FALSE(animation.attach_graph(driven, Id128::from_parts(1, 1)));  // not in the library

  // Stand, walk, stand: the graph's steps applied by the capability, beside the same play() calls.
  const bool moving[] = {false, false, true, true, true, false, false, false};
  for (const bool m : moving) {
    const f32 values[k_locomotion_params] = {m ? 1.0f : 0.0f, 0.0f, 0.0f};
    GraphEnter entered[k_graph_max_machines];
    if (animation.control_graph(driven, values, 0, 1.0f / 64.0f, entered) != 0) {
      REQUIRE(animation.apply_graph_step(driven, entered[0]));
      const Id128 clip = entered[0].state->clip_id;
      REQUIRE(animation.play(played, clip, entered[0].fade_s, true));
      if (!entered[0].state->synced) {
        AnimationPlayer p;
        REQUIRE(animation.player(played, p));
        REQUIRE(animation.set_playhead(played, p.time, entered[0].state->rate));
      }
    }
    sim.step();
    AnimationPlayer a;
    AnimationPlayer b;
    REQUIRE(animation.player(driven, a));
    REQUIRE(animation.player(played, b));
    CHECK(a == b);
    u32 fa = 0;
    u32 ca = 0;
    u32 fb = 0;
    u32 cb = 0;
    REQUIRE(animation.joint_run(driven, fa, ca));
    REQUIRE(animation.joint_run(played, fb, cb));
    REQUIRE(ca == cb);
    CHECK(std::memcmp(animation.joint_matrices().data() + fa, animation.joint_matrices().data() + fb,
                      ca * sizeof(anim::JointMatrix)) == 0);
  }
  MESSAGE("graph library bytes with the walker's graph: " << animation.graphs().bytes());
}
