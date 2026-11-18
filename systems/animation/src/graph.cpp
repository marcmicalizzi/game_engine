#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document.h>
#include <systems/animation/graph.h>
#include <systems/animation/library.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace engine::animation {

namespace {

constexpr u32 k_absent = 0xFFFF'FFFFu;

std::string hex(const Id128& id) {
  char text[Id128::k_hex_length + 1];  // to_hex writes a terminator
  id.to_hex(text);
  return std::string(text, Id128::k_hex_length);
}

// ---- reading the records -----------------------------------------------------------------------

template <class T>
bool read_record(const doc::ResolvedObject& object, GraphItem<T>& out, std::string& why) {
  JsonValue properties = JsonValue::object();
  for (const auto& kv : object.properties) {
    if (kv.second != nullptr) properties.set(kv.first, *kv.second);
  }
  schema::ReadContext ctx;
  out.id = object.id;
  out.parent = object.parent;
  if (!schema::from_json(out.record, properties, ctx) || !ctx.ok()) {
    why = std::string(object.type) + " " + hex(object.id) + ": " +
          (ctx.diagnostics.empty() ? std::string("unreadable")
                                   : ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message);
    return false;
  }
  return true;
}

template <class T>
bool is_type(const doc::ResolvedObject& object) noexcept {
  return object.type == std::string_view(schema::type_of<T>().qualified_name);
}

// Every record under `parent`, depth first, children in id order (the document's order).
bool read_children(const doc::Document& document, const Id128& parent, GraphDesc& out,
                   std::string& why, u32 depth) {
  if (depth > 64) {
    why = "graph " + hex(out.id) + ": records nest deeper than 64";
    return false;
  }
  for (const Id128& id : document.children(parent)) {
    doc::ResolvedObject object;
    if (!document.resolve(id, object) || object.deleted) continue;
    bool ok = true;
    if (is_type<GraphParam>(object)) {
      ok = read_record(object, out.params.emplace_back(), why);
    } else if (is_type<GraphMachine>(object)) {
      ok = read_record(object, out.machines.emplace_back(), why);
    } else if (is_type<GraphState>(object)) {
      ok = read_record(object, out.states.emplace_back(), why);
    } else if (is_type<GraphTransition>(object)) {
      ok = read_record(object, out.transitions.emplace_back(), why);
    } else if (is_type<GraphNode>(object)) {
      ok = read_record(object, out.nodes.emplace_back(), why);
    } else {
      continue;  // not a part of a graph: skipped, and so is what is under it
    }
    if (!ok || !read_children(document, id, out, why, depth + 1)) return false;
  }
  return true;
}

// ---- the compile
// ---------------------------------------------------------------------------------

template <class T>
void sort_by_id(Vector<const GraphItem<T>*>& items) {
  std::sort(items.begin(), items.end(),
            [](const GraphItem<T>* a, const GraphItem<T>* b) { return a->id < b->id; });
}

template <class T>
Vector<const GraphItem<T>*> sorted(const Vector<GraphItem<T>>& items, const Id128& parent) {
  Vector<const GraphItem<T>*> out;
  for (const GraphItem<T>& item : items) {
    if (item.parent == parent) out.push_back(&item);
  }
  sort_by_id(out);
  return out;
}

bool refuse(std::string& why, const GraphDesc& desc, std::string_view what) {
  why = "graph '" + desc.graph.name + "': ";
  why.append(what);
  return false;
}

f32 clamp_param(const ParamInfo& p, f32 v) noexcept {
  if (p.kind != GraphParamKind::Float) return v != 0.0f ? 1.0f : 0.0f;
  if (!p.clamped) return v;
  return v < p.lo ? p.lo : (v > p.hi ? p.hi : v);
}

bool holds(const ControlCondition& c, const AnimationGraphInput& input) noexcept {
  if (c.trigger != 0) return ((input.triggers >> c.param) & 1u) != 0;
  const f32 v = input.values[c.param];
  switch (static_cast<GraphCompare>(c.op)) {
    case GraphCompare::Less: return v < c.value;
    case GraphCompare::LessEqual: return v <= c.value;
    case GraphCompare::Greater: return v > c.value;
    case GraphCompare::GreaterEqual: return v >= c.value;
    case GraphCompare::Equal: return v == c.value;
    case GraphCompare::NotEqual: return v != c.value;
  }
  return false;
}

// The first transition of `[first, first + count)` whose conditions all hold, skipping an `any`
// transition into `current` (the state the machine is in or heading to); `k_absent` when none.
u32 first_holding(const CompiledGraph& g, u32 first, u32 count, u32 current,
                  const AnimationGraphInput& input) noexcept {
  for (u32 t = first; t < first + count; ++t) {
    const ControlTransition& tr = g.transitions[t];
    if (tr.any != 0 && tr.to == current) continue;
    bool all = true;
    for (u32 c = tr.first_condition; all && c < u32{tr.first_condition} + tr.condition_count; ++c)
      all = holds(g.conditions[c], input);
    if (all) return t;
  }
  return k_absent;
}

u32 find_transition(const CompiledGraph& g, const ControlMachine& m, u32 current,
                    const AnimationGraphInput& input) noexcept {
  const u32 any = first_holding(g, m.any_first, m.any_count, current, input);
  if (any != k_absent) return any;
  const ControlState& s = g.states[m.first_state + current];
  return first_holding(g, s.first_transition, s.transition_count, current, input);
}

f32 clamp01(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// A state's clips' weights: 1 for a Clip state; for a Blend1D, the two inputs whose thresholds
// bracket the parameter, linearly (Unity's 1D blend tree), and one input alone at or beyond an
// end. An input exactly at the parameter has weight 1 and its neighbour 0: (p - t) / (t' - t) is 0
// or 1 exactly there, so a blend at a threshold is that one clip. A NaN parameter is the first.
void clip_weights(const CompiledGraph& g, const PoseState& ps, const AnimationGraphInput& input,
                  f32* w) noexcept {
  const u32 n = ps.clip_count;
  for (u32 i = 0; i < n; ++i)
    w[i] = 0.0f;
  if ((ps.flags & k_pose_state_blend) == 0) {
    w[0] = 1.0f;
    return;
  }
  const PoseClip* clips = g.pose_clips.data() + ps.first_clip;
  const f32 p = input.values[ps.param];
  if (!(p > clips[0].threshold)) {
    w[0] = 1.0f;
    return;
  }
  if (!(p < clips[n - 1].threshold)) {
    w[n - 1] = 1.0f;
    return;
  }
  u32 i = 0;
  while (i + 2 < n && !(p < clips[i + 1].threshold))
    ++i;
  const f32 f = (p - clips[i].threshold) / (clips[i + 1].threshold - clips[i].threshold);
  w[i] = 1.0f - f;
  w[i + 1] = f;
}

// The weights of the state a machine is in and of the one it is heading to, at `lead` past the
// tick's start: a cross-fade's fraction (elapsed + lead) / duration, or with `interpolate` off a
// cut at its midpoint (LodPlan::interpolate).
void state_weights(const AnimationGraphState& st, u32 m, f32 lead, bool interpolate, f32& w_cur,
                   f32& w_next) noexcept {
  w_cur = 1.0f;
  w_next = 0.0f;
  if (st.next[m] == 0) return;
  f32 t = st.duration[m] > 0.0f ? clamp01((st.elapsed[m] + lead) / st.duration[m]) : 1.0f;
  if (!interpolate) t = t >= 0.5f ? 1.0f : 0.0f;
  w_cur = 1.0f - t;
  w_next = t;
}

// The cycles per second a led sync group advances at: the rate over the duration of its clip of
// highest weight, over the state the machine is in and the one it is heading to (the first in the
// program's order on a tie); 0 when none of the group's clips plays.
f32 leader_cycles(const CompiledGraph& g, u32 m, const AnimationGraphInput& input,
                  const AnimationGraphState& st, f32 lead) noexcept {
  const ControlMachine& machine = g.machines[m];
  f32 w_state[2];
  state_weights(st, m, lead, true, w_state[0], w_state[1]);
  const u32 at[2] = {u32{st.state[m]}, u32{st.next[m]}};
  f32 best = 0.0f;
  f32 cycles = 0.0f;
  f32 w[k_graph_max_blend_inputs];
  for (u32 k = 0; k < 2; ++k) {
    if (at[k] == 0 || !(w_state[k] > 0.0f)) continue;
    const PoseState& ps = g.pose_states[machine.first_state + at[k] - 1];
    if ((ps.flags & k_pose_state_synced) == 0) continue;
    clip_weights(g, ps, input, w);
    for (u32 c = 0; c < ps.clip_count; ++c) {
      const PoseClip& clip = g.pose_clips[ps.first_clip + c];
      const f32 weight = w_state[k] * w[c];
      if (clip.duration > 0.0f && weight > best) {
        best = weight;
        cycles = clip.rate / clip.duration;
      }
    }
  }
  return cycles;
}

f32 wrap01(f32 v) noexcept { return v - std::floor(v); }

// The pose program for machine 0 (graph.h, "the pose program"): per state in index order its ops,
// then the output's normalize. Register 0 is the instance's slot, 1 a clip's sample, 2 a Blend1D's
// sum.
void compile_program(CompiledGraph& g) {
  g.program.clear();
  g.registers = 0;
  g.program_why.clear();
  if (g.machines.size() != 1) {
    g.program_why = "a graph of " + std::to_string(g.machines.size()) +
                    " machines: composing their poses is a layer's (stage 3)";
    return;
  }
  const ControlMachine& machine = g.machines[0];
  // A state cross-faded into itself would be two copies of one state on two clocks, which the
  // program's one op run a state cannot play.
  for (u32 s = 0; s < machine.state_count; ++s) {
    const ControlState& cs = g.states[machine.first_state + s];
    for (u32 t = cs.first_transition; t < u32{cs.first_transition} + cs.transition_count; ++t) {
      const ControlTransition& tr = g.transitions[t];
      if (tr.to == s && tr.blend != static_cast<u8>(GraphBlend::Cut)) {
        g.program_why = "state '" + g.state_info[machine.first_state + s].name +
                        "' cross-fades into itself: one clock a state cannot play two copies "
                        "(inertialization, stage 4, restarts a state)";
        return;
      }
    }
  }
  usize ops = 1;
  bool any_blend = false;
  for (u32 s = 0; s < machine.state_count; ++s) {
    const PoseState& ps = g.pose_states[machine.first_state + s];
    const bool blend = (ps.flags & k_pose_state_blend) != 0;
    any_blend = any_blend || blend;
    ops += blend ? 2u * ps.clip_count + 2u : 2u;
  }
  if (ops > k_graph_max_records) {
    g.program_why = std::to_string(ops) + " pose ops, past the limit of 65,535";
    return;
  }
  g.program.reserve(static_cast<u32>(ops));
  const auto sample = [&](u32 s, const PoseState& ps, u32 c) {
    const PoseClip& clip = g.pose_clips[ps.first_clip + c];
    GraphOp& op = g.program.emplace_back();
    op.kind = static_cast<u8>(PoseOpKind::Sample);
    op.dst = 1;
    op.b = static_cast<u8>(((ps.flags & k_pose_state_synced) != 0 ? k_pose_op_synced : 0u) |
                           ((ps.flags & k_pose_state_looping) != 0 ? k_pose_op_looping : 0u));
    op.node = static_cast<u16>(s);
    op.param = static_cast<u16>(c);
    op.data = clip.clip_index;
    op.k = (ps.flags & k_pose_state_synced) != 0 ? clip.duration : clip.rate;
  };
  const auto accumulate = [&](u32 s, u8 dst, u8 from, u32 c, u8 flags) {
    GraphOp& op = g.program.emplace_back();
    op.kind = static_cast<u8>(PoseOpKind::Accumulate);
    op.dst = dst;
    op.a = from;
    op.b = flags;
    op.node = static_cast<u16>(s);
    op.param = static_cast<u16>(c);
  };
  const auto normalize = [&](u32 s, u8 dst) {
    GraphOp& op = g.program.emplace_back();
    op.kind = static_cast<u8>(PoseOpKind::Normalize);
    op.dst = dst;
    op.node = static_cast<u16>(s);
  };
  for (u32 s = 0; s < machine.state_count; ++s) {
    PoseState& ps = g.pose_states[machine.first_state + s];
    ps.first_op = static_cast<u16>(g.program.size());
    if ((ps.flags & k_pose_state_blend) == 0) {
      sample(s, ps, 0);
      accumulate(s, 0, 1, 0, k_pose_op_state);
    } else {
      for (u32 c = 0; c < ps.clip_count; ++c) {
        sample(s, ps, c);
        accumulate(s, 2, 1, c, 0);
      }
      normalize(s, 2);
      accumulate(s, 0, 2, 0, k_pose_op_state);
    }
  }
  normalize(0xFFFFu, 0);
  g.registers = any_blend ? 3u : 2u;
}

}  // namespace

// ---- read_graph
// ----------------------------------------------------------------------------------

bool read_graph(const doc::Document& document, const Id128& graph, GraphDesc& out,
                std::string& why) {
  out = GraphDesc{};
  out.id = graph;
  doc::ResolvedObject root;
  if (!document.resolve(graph, root) || root.deleted) {
    why = "graph " + hex(graph) + ": no such live record";
    return false;
  }
  if (!is_type<Graph>(root)) {
    why = "graph " + hex(graph) + ": a " + std::string(root.type) + ", not an " +
          schema::type_of<Graph>().qualified_name;
    return false;
  }
  GraphItem<Graph> item;
  if (!read_record(root, item, why)) return false;
  out.graph = std::move(item.record);
  return read_children(document, graph, out, why, 0);
}

// ---- the walker's graph
// ----------------------------------------------------------------------------

GraphDesc locomotion_graph(const LocomotionClips& clips) {
  u64 seed = hash_bytes("engine.animation.locomotion", 27);
  for (const std::string_view name : {clips.idle, clips.walk, clips.run}) {
    seed = hash_combine(seed, hash_bytes(name.data(), name.size()));
  }
  seed = hash_combine(seed, std::bit_cast<u32>(clips.fade_s));
  // One high word and a counter for the low: the ids sort in the order they are made, so a
  // parameter's slot is its declaration order here (the `k_locomotion_*` indices) and the states
  // sort stand, walk, run.
  u64 counter = 1;
  const auto next_id = [&] { return Id128::from_parts(seed | 1u, counter++); };

  GraphDesc d;
  d.id = next_id();
  d.graph.name = "locomotion";
  const auto param = [&](const char* name, GraphParamKind kind) {
    GraphItem<GraphParam>& p = d.params.emplace_back();
    p.id = next_id();
    p.parent = d.id;
    p.record.name = name;
    p.record.kind = kind;
  };
  param("moving", GraphParamKind::Bool);
  param("fast", GraphParamKind::Bool);
  param("phase", GraphParamKind::Float);
  d.params[2].record.range_min = 0.0f;
  d.params[2].record.range_max = 1.0f;

  GraphItem<GraphMachine>& machine = d.machines.emplace_back();
  machine.id = next_id();
  machine.parent = d.id;
  machine.record.name = "gait";
  machine.record.phase_param = "phase";
  const Id128 machine_id = machine.id;

  const auto state = [&](const char* name, std::string_view clip, bool synced) {
    GraphItem<GraphState>& s = d.states.emplace_back();
    s.id = next_id();
    s.parent = machine_id;
    s.record.name = name;
    GraphItem<GraphNode>& n = d.nodes.emplace_back();
    n.id = next_id();
    n.parent = s.id;
    n.record.kind = GraphNodeKind::Clip;
    n.record.clip = std::string(clip);
    n.record.looping = true;
    n.record.rate = synced ? 0.0f : 1.0f;
    n.record.sync_group = synced ? u8{1} : u8{0};
    return s.id;
  };
  const Id128 stand = state("stand", clips.idle, false);
  const Id128 walk = state("walk", clips.walk, true);
  const Id128 run = state("run", clips.run, true);
  d.machines[0].record.entry = stand;

  const auto into = [&](const Id128& to, f32 moving, f32 fast, bool reads_fast) {
    GraphItem<GraphTransition>& t = d.transitions.emplace_back();
    t.id = next_id();
    t.parent = machine_id;
    t.record.to = to;
    t.record.duration_s = clips.fade_s;
    t.record.blend = GraphBlend::CrossFade;
    t.record.conditions.push_back(GraphCondition{"moving", GraphCompare::Equal, moving});
    if (reads_fast)
      t.record.conditions.push_back(GraphCondition{"fast", GraphCompare::Equal, fast});
  };
  into(walk, 1.0f, 0.0f, true);
  into(run, 1.0f, 1.0f, true);
  into(stand, 0.0f, 0.0f, false);
  return d;
}

// ---- the compiled graph ------------------------------------------------------------------------

u32 CompiledGraph::param_index(std::string_view param_name) const noexcept {
  for (u32 i = 0; i < params.size(); ++i) {
    if (params[i].name == param_name) return i;
  }
  return k_absent;
}

u32 CompiledGraph::state_index(u32 machine, std::string_view state_name) const noexcept {
  if (machine >= machines.size()) return k_absent;
  const ControlMachine& m = machines[machine];
  for (u32 i = 0; i < m.state_count; ++i) {
    if (state_info[m.first_state + i].name == state_name) return i;
  }
  return k_absent;
}

usize CompiledGraph::bytes() const noexcept {
  // A string's own heap block, if it has one: a short one lives inside the object, which the
  // `sizeof` of whatever holds it already counts.
  const usize inline_capacity = std::string().capacity();
  const auto heap = [&](const std::string& s) {
    return s.capacity() > inline_capacity ? s.capacity() + 1 : usize{0};
  };
  usize n = sizeof(CompiledGraph) + heap(name);
  n += params.capacity() * sizeof(ParamInfo);
  for (const ParamInfo& p : params)
    n += heap(p.name);
  n += machines.capacity() * sizeof(ControlMachine);
  n += states.capacity() * sizeof(ControlState);
  n += state_info.capacity() * sizeof(StateInfo);
  for (const StateInfo& s : state_info)
    n += heap(s.name) + heap(s.clip);
  n += transitions.capacity() * sizeof(ControlTransition);
  n += conditions.capacity() * sizeof(ControlCondition);
  n += pose_states.capacity() * sizeof(PoseState);
  n += pose_clips.capacity() * sizeof(PoseClip);
  n += program.capacity() * sizeof(GraphOp);
  n += heap(program_why);
  return n;
}

bool compile_graph(const GraphDesc& desc, const Library* library, CompiledGraph& out,
                   std::string& why) {
  out = CompiledGraph{};
  out.id = desc.id;
  out.name = desc.graph.name;
  if (desc.graph.skeleton_roles) {
    return refuse(why, desc,
                  "skeleton_roles is in no stage of the design yet (it waits for a "
                  "standard-skeleton clip set)");
  }
  if (desc.graph.root_motion)
    return refuse(why, desc, "root_motion is in no stage of the design yet (a RootMotion output)");

  // Parameters, in id order: a parameter's index is its slot in the input component.
  const Vector<const GraphItem<GraphParam>*> params = sorted(desc.params, desc.id);
  if (params.size() > k_graph_max_params) {
    return refuse(why, desc,
                  std::to_string(params.size()) + " parameters, past the limit of " +
                      std::to_string(k_graph_max_params));
  }
  for (const GraphItem<GraphParam>* p : params) {
    const GraphParam& r = p->record;
    if (r.name.empty()) return refuse(why, desc, "a parameter with no name");
    if (out.param_index(r.name) != k_absent)
      return refuse(why, desc, "parameter '" + r.name + "' named twice");
    ParamInfo& info = out.params.emplace_back();
    info.name = r.name;
    info.kind = r.kind;
    info.clamped = r.range_min.has_value() || r.range_max.has_value();
    info.lo = r.range_min.value_or(-3.4028235e38f);
    info.hi = r.range_max.value_or(3.4028235e38f);
    if (info.lo > info.hi) return refuse(why, desc, "parameter '" + r.name + "': an empty range");
    if (info.clamped && r.kind != GraphParamKind::Float)
      return refuse(why, desc, "parameter '" + r.name + "': a range on a bool or a trigger");
    info.default_value = clamp_param(info, r.default_value);
  }
  const auto param_of = [&](const std::string& name, u16& index) {
    const u32 i = out.param_index(name);
    if (i == k_absent) return false;
    index = static_cast<u16>(i);
    return true;
  };

  // Machines, in id order.
  const Vector<const GraphItem<GraphMachine>*> machines = sorted(desc.machines, desc.id);
  if (machines.empty()) return refuse(why, desc, "no machine");
  if (machines.size() > k_graph_max_machines) {
    return refuse(why, desc,
                  std::to_string(machines.size()) + " machines, past the limit of " +
                      std::to_string(k_graph_max_machines));
  }
  // A node is a state's pose or an input of a Blend1D that is one; anything else is a layer's.
  const auto is_state = [&](const Id128& id) {
    for (const GraphItem<GraphState>& s : desc.states) {
      if (s.id == id) return true;
    }
    return false;
  };
  for (const GraphItem<GraphNode>& n : desc.nodes) {
    bool placed = is_state(n.parent);
    for (const GraphItem<GraphNode>& p : desc.nodes) {
      placed = placed ||
               (p.id == n.parent && p.record.kind == GraphNodeKind::Blend1D && is_state(p.parent));
    }
    if (!placed) {
      return refuse(why, desc,
                    "a node outside a state's pose or a Blend1D's inputs is a layer's (stage 3)");
    }
  }
  // A clip by name: its library index and duration when a library is given.
  const auto resolve = [&](const std::string& where, const GraphNode& node, PoseClip& clip,
                           Id128& clip_id) {
    clip.rate = node.rate;
    if (node.clip.empty()) return true;
    clip_id = Library::clip_id(node.clip);
    if (library == nullptr) return true;
    clip.clip_index = library->clip_index(clip_id);
    if (clip.clip_index == Library::k_not_found)
      return refuse(why, desc, where + " plays clip '" + node.clip + "', not in the library");
    clip.duration = library->clip_data(clip.clip_index).duration;
    return true;
  };
  for (const GraphItem<GraphState>& s : desc.states) {
    bool known = false;
    for (const GraphItem<GraphMachine>* m : machines)
      known = known || m->id == s.parent;
    if (!known) return refuse(why, desc, "state '" + s.record.name + "' belongs to no machine");
  }

  u8 groups_driven = 0;
  for (u32 mi = 0; mi < machines.size(); ++mi) {
    const GraphItem<GraphMachine>& machine = *machines[mi];
    const std::string& mname = machine.record.name;
    const Vector<const GraphItem<GraphState>*> states = sorted(desc.states, machine.id);
    if (states.empty()) return refuse(why, desc, "machine '" + mname + "' has no state");
    if (states.size() > k_graph_max_states) {
      return refuse(why, desc,
                    "machine '" + mname + "': " + std::to_string(states.size()) +
                        " states, past the limit of " + std::to_string(k_graph_max_states));
    }
    ControlMachine& cm = out.machines.emplace_back();
    cm.first_state = out.states.size();
    cm.state_count = static_cast<u16>(states.size());
    const auto local = [&](const Id128& id) {
      for (u32 i = 0; i < states.size(); ++i) {
        if (states[i]->id == id) return i;
      }
      return k_absent;
    };
    const u32 entry = local(machine.record.entry);
    if (entry == k_absent)
      return refuse(why, desc, "machine '" + mname + "': its entry is not one of its states");
    cm.entry = static_cast<u16>(entry);
    if (!machine.record.phase_param.empty()) {
      if (!param_of(machine.record.phase_param, cm.phase_param) ||
          out.params[cm.phase_param].kind != GraphParamKind::Float) {
        return refuse(why, desc,
                      "machine '" + mname + "': phase_param '" + machine.record.phase_param +
                          "' is not a float parameter");
      }
    }

    // The states and what plays each.
    for (const GraphItem<GraphState>* s : states) {
      const Vector<const GraphItem<GraphNode>*> pose = sorted(desc.nodes, s->id);
      const std::string where = "state '" + mname + "." + s->record.name + "'";
      if (pose.size() != 1) {
        return refuse(why, desc,
                      where + " has " + std::to_string(pose.size()) +
                          " pose nodes; a state's pose is one node");
      }
      const GraphNode& node = pose[0]->record;
      if (node.kind == GraphNodeKind::Override || node.kind == GraphNodeKind::Additive)
        return refuse(why, desc, where + ": an Override or Additive node is stage 3's");
      if (node.kind != GraphNodeKind::Clip && node.kind != GraphNodeKind::Blend1D) {
        return refuse(why, desc,
                      where +
                          ": a Machine node (a machine nested in a state) is in no "
                          "stage of the design yet");
      }
      if (node.sync_group > k_graph_max_sync_groups) {
        return refuse(why, desc,
                      where + ": sync group " + std::to_string(node.sync_group) +
                          ", past the limit of " + std::to_string(k_graph_max_sync_groups));
      }
      if (!std::isfinite(node.rate) || !std::isfinite(node.weight) || node.weight != 1.0f)
        return refuse(why, desc, where + ": a weight other than 1 is a layer's (stage 3)");
      StateInfo& info = out.state_info.emplace_back();
      info.id = s->id;
      info.name = s->record.name;
      info.rate = node.rate;
      info.looping = node.looping;
      PoseState& ps = out.pose_states.emplace_back();
      ps.first_clip = static_cast<u16>(out.pose_clips.size());
      ps.flags = node.looping ? k_pose_state_looping : u8{0};
      const Vector<const GraphItem<GraphNode>*> children = sorted(desc.nodes, pose[0]->id);
      if (node.kind == GraphNodeKind::Clip) {
        if (!node.inputs.empty() || !node.thresholds.empty() || !node.param.empty() ||
            !children.empty()) {
          return refuse(why, desc,
                        where + ": a Clip node takes no inputs, thresholds or parameter");
        }
        info.clip = node.clip;
        PoseClip& clip = out.pose_clips.emplace_back();
        if (!resolve(where, node, clip, info.clip_id)) return false;
        info.clip_index = clip.clip_index;
        ps.clip_count = 1;
      } else {
        // A Blend1D: its inputs are its child Clip nodes, in the order `inputs` names them, each at
        // its threshold; the parameter is a float. Its clips loop (or not) and lock to a group (or
        // not) as the Blend1D says, so the inputs cannot disagree about either.
        const u32 n = node.inputs.size();
        if (n == 0 || n > k_graph_max_blend_inputs) {
          return refuse(why, desc,
                        where + ": a Blend1D of " + std::to_string(n) + " inputs (1 to " +
                            std::to_string(k_graph_max_blend_inputs) + ")");
        }
        if (node.thresholds.size() != n || children.size() != n) {
          return refuse(why, desc,
                        where +
                            ": a Blend1D's inputs, thresholds and child nodes are "
                            "one each");
        }
        if (!param_of(node.param, ps.param) || out.params[ps.param].kind != GraphParamKind::Float)
          return refuse(why, desc,
                        where + ": a Blend1D reads a float parameter, not '" + node.param + "'");
        if (out.pose_clips.size() + n > k_graph_max_records)
          return refuse(why, desc, "more pose clips than the limit of 65,535");
        ps.flags = static_cast<u8>(ps.flags | k_pose_state_blend);
        ps.clip_count = static_cast<u8>(n);
        info.blend = true;
        for (u32 i = 0; i < n; ++i) {
          const GraphItem<GraphNode>* input = nullptr;
          for (const GraphItem<GraphNode>* c : children) {
            if (c->id == node.inputs[i]) input = c;
          }
          if (input == nullptr)
            return refuse(why, desc,
                          where + ": a Blend1D input " + hex(node.inputs[i]) +
                              " is not one of its child nodes");
          const GraphNode& in = input->record;
          if (in.kind != GraphNodeKind::Clip || !in.inputs.empty() || !in.thresholds.empty() ||
              !in.param.empty() || !sorted(desc.nodes, input->id).empty()) {
            return refuse(why, desc,
                          where +
                              ": a Blend1D's inputs are Clip nodes (a blend of "
                              "blends is in no stage of the design yet)");
          }
          if (in.sync_group != 0 || in.looping != node.looping || in.weight != 1.0f ||
              !std::isfinite(in.rate)) {
            return refuse(why, desc,
                          where +
                              ": a Blend1D's input takes its sync group and "
                              "looping from the Blend1D, and a weight of 1");
          }
          const f32 at = node.thresholds[i];
          if (!std::isfinite(at) || (i > 0 && !(at > node.thresholds[i - 1])))
            return refuse(why, desc, where + ": a Blend1D's thresholds are finite and increasing");
          PoseClip& clip = out.pose_clips.emplace_back();
          Id128 unused;
          if (!resolve(where, in, clip, unused)) return false;
          clip.threshold = at;
        }
      }
      if (node.sync_group != 0) {
        const u8 group = static_cast<u8>(node.sync_group - 1);
        if (cm.sync_group != 0xFFu && cm.sync_group != group)
          return refuse(why, desc, "machine '" + mname + "' drives two sync groups");
        cm.sync_group = group;
        info.synced = true;
        ps.flags = static_cast<u8>(ps.flags | k_pose_state_synced);
      }
      out.states.emplace_back();
    }
    if (cm.sync_group != 0xFFu) {
      if ((groups_driven >> cm.sync_group) & 1u)
        return refuse(why, desc, "two machines drive one sync group");
      groups_driven = static_cast<u8>(groups_driven | (1u << cm.sync_group));
    }

    // The transitions: `any` first, then each state's run, each sorted by (priority, id).
    Vector<const GraphItem<GraphTransition>*> transitions = sorted(desc.transitions, machine.id);
    std::stable_sort(transitions.begin(), transitions.end(),
                     [](const GraphItem<GraphTransition>* a, const GraphItem<GraphTransition>* b) {
                       return a->record.priority < b->record.priority;
                     });
    const auto emit = [&](const GraphItem<GraphTransition>& t) {
      const GraphTransition& r = t.record;
      const std::string where = "machine '" + mname + "': transition " + hex(t.id);
      if (out.transitions.size() >= k_graph_max_records)
        return refuse(why, desc, "more transitions than the limit of 65,535");
      const u32 to = local(r.to);
      if (to == k_absent) return refuse(why, desc, where + " goes to a state not of this machine");
      if (r.exit_phase.has_value())
        return refuse(why, desc, where + ": exit_phase is in no stage of the design yet");
      if (r.blend == GraphBlend::Inertialize)
        return refuse(why, desc, where + ": Inertialize is stage 4's");
      if (r.offset != GraphOffset::Zero) {
        return refuse(why, desc,
                      where +
                          ": an offset other than Zero is in no stage of the design "
                          "yet (MatchPhase waits for clips with foot contacts)");
      }
      if (!std::isfinite(r.duration_s) || r.duration_s < 0.0f)
        return refuse(why, desc, where + ": a duration that is not a finite, non-negative number");
      if (r.conditions.size() > 255) return refuse(why, desc, where + ": over 255 conditions");
      ControlTransition& ct = out.transitions.emplace_back();
      ct.to = static_cast<u16>(to);
      ct.first_condition = static_cast<u16>(out.conditions.size());
      ct.condition_count = static_cast<u8>(r.conditions.size());
      ct.blend = static_cast<u8>(r.duration_s > 0.0f ? r.blend : GraphBlend::Cut);
      ct.any = r.from.is_null() ? u8{1} : u8{0};
      ct.duration_s = ct.blend == static_cast<u8>(GraphBlend::Cut) ? 0.0f : r.duration_s;
      for (const GraphCondition& c : r.conditions) {
        if (out.conditions.size() >= k_graph_max_records)
          return refuse(why, desc, "more conditions than the limit of 65,535");
        ControlCondition& cc = out.conditions.emplace_back();
        if (!param_of(c.param, cc.param))
          return refuse(why, desc, where + " reads parameter '" + c.param + "', not declared");
        cc.op = static_cast<u8>(c.op);
        cc.value = c.value;
        if (out.params[cc.param].kind == GraphParamKind::Trigger) {
          if (c.op != GraphCompare::Equal || c.value != 1.0f)
            return refuse(why, desc, where + ": a trigger's condition is 'Equal 1'");
          cc.trigger = 1;
          ct.clears |= 1u << cc.param;
        }
      }
      return true;
    };
    cm.any_first = static_cast<u16>(out.transitions.size());
    for (const GraphItem<GraphTransition>* t : transitions) {
      if (t->record.from.is_null() && !emit(*t)) return false;
    }
    cm.any_count = static_cast<u16>(out.transitions.size() - cm.any_first);
    for (u32 si = 0; si < states.size(); ++si) {
      ControlState& cs = out.states[cm.first_state + si];
      cs.first_transition = static_cast<u16>(out.transitions.size());
      for (const GraphItem<GraphTransition>* t : transitions) {
        if (t->record.from == states[si]->id && !emit(*t)) return false;
      }
      cs.transition_count = static_cast<u16>(out.transitions.size() - cs.first_transition);
    }
    for (const GraphItem<GraphTransition>* t : transitions) {
      if (!t->record.from.is_null() && local(t->record.from) == k_absent) {
        return refuse(why, desc,
                      "machine '" + mname + "': transition " + hex(t->id) +
                          " leaves a state not of this machine");
      }
    }
  }
  if (out.transitions.size() > k_graph_max_records)
    return refuse(why, desc, "more transitions than the limit of 65,535");
  compile_program(out);
  return true;
}

// ---- the control pass --------------------------------------------------------------------------

void set_graph_inputs(const CompiledGraph& graph, AnimationGraphInput& input,
                      std::span<const f32> values, u32 triggers) noexcept {
  const u32 n = std::min<u32>(static_cast<u32>(values.size()), graph.params.size());
  for (u32 i = 0; i < n; ++i)
    input.values[i] = clamp_param(graph.params[i], values[i]);
  input.triggers |= triggers;
}

void init_graph_instance(const CompiledGraph& graph, AnimationGraphInput& input,
                         AnimationGraphState& state) noexcept {
  input = AnimationGraphInput{};
  input.graph = graph.id;
  for (u32 i = 0; i < graph.params.size(); ++i)
    input.values[i] = graph.params[i].default_value;
  state = AnimationGraphState{};
  state.graph = graph.id;
}

u32 graph_control(const CompiledGraph& graph, AnimationGraphInput& input,
                  AnimationGraphState& state, f32 dt, GraphStep* steps) noexcept {
  if (state.graph != graph.id) {
    state = AnimationGraphState{};
    state.graph = graph.id;
  }
  u32 entered = 0;
  for (u32 m = 0; m < graph.machines.size(); ++m) {
    const ControlMachine& machine = graph.machines[m];
    GraphStep& step = steps[m];
    step = GraphStep{};
    if (state.state[m] == 0) {
      // The first tick enters at the entry and takes whatever transition holds from it at once:
      // nothing was ever shown, so it is a cut.
      u32 at = machine.entry;
      const u32 t = find_transition(graph, machine, at, input);
      if (t != k_absent) {
        at = graph.transitions[t].to;
        input.triggers &= ~graph.transitions[t].clears;
      }
      state.state[m] = static_cast<u16>(at + 1);
      state.next[m] = 0;
      state.elapsed[m] = 0.0f;
      state.duration[m] = 0.0f;
      state.time[m] = 0.0f;
      state.next_time[m] = 0.0f;
      step.entered = static_cast<u16>(at + 1);
      step.blend = static_cast<u8>(GraphBlend::Cut);
      step.first = 1;
      entered |= 1u << m;
    } else {
      state.time[m] += dt;
      if (state.next[m] != 0) {
        state.next_time[m] += dt;
        state.elapsed[m] += dt;
        if (state.elapsed[m] >= state.duration[m]) {
          state.state[m] = state.next[m];
          state.time[m] = state.next_time[m];
          state.next[m] = 0;
          state.elapsed[m] = 0.0f;
          state.duration[m] = 0.0f;
          state.next_time[m] = 0.0f;
        }
      }
      // Transitions are taken from where the machine is heading (Unity's "next state" interruption
      // source): a change of mind mid-fade starts a new fade from the one in flight, which the
      // player collapses first, as `play()` does.
      const u32 current =
          static_cast<u32>((state.next[m] != 0 ? state.next[m] : state.state[m]) - 1);
      const u32 t = find_transition(graph, machine, current, input);
      if (t != k_absent) {
        const ControlTransition& tr = graph.transitions[t];
        input.triggers &= ~tr.clears;
        if (state.next[m] != 0) {
          state.state[m] = state.next[m];
          state.time[m] = state.next_time[m];
        }
        if (tr.blend == static_cast<u8>(GraphBlend::Cut)) {
          state.state[m] = static_cast<u16>(tr.to + 1);
          state.time[m] = 0.0f;
          state.next[m] = 0;
          state.duration[m] = 0.0f;
        } else {
          state.next[m] = static_cast<u16>(tr.to + 1);
          state.duration[m] = tr.duration_s;
        }
        state.elapsed[m] = 0.0f;
        state.next_time[m] = 0.0f;
        step.entered = static_cast<u16>(tr.to + 1);
        step.blend = tr.blend;
        step.fade_s = tr.duration_s;
        entered |= 1u << m;
      }
    }
    if (machine.sync_group != 0xFFu) {
      if (machine.phase_param != k_graph_no_param) {
        const f32 v = input.values[machine.phase_param];
        state.phase[machine.sync_group] = v - std::floor(v);
      } else {
        // Led (stage 2): the phase the pose shows at the tick's end, as the clocks' lead does.
        const f32 cycles = leader_cycles(graph, m, input, state, dt);
        if (cycles > 0.0f)
          state.phase[machine.sync_group] = wrap01(state.phase[machine.sync_group] + dt * cycles);
      }
    }
  }
  return entered;
}

void graph_catch_up(const CompiledGraph& graph, const AnimationGraphInput& input,
                    AnimationGraphState& state, f32 gap) noexcept {
  if (state.graph != graph.id || !(gap > 0.0f)) return;
  for (u32 m = 0; m < graph.machines.size(); ++m) {
    if (state.state[m] == 0) continue;
    state.time[m] += gap;
    if (state.next[m] != 0) {
      state.next_time[m] += gap;
      state.elapsed[m] += gap;
      if (state.elapsed[m] >= state.duration[m]) {
        state.state[m] = state.next[m];
        state.time[m] = state.next_time[m];
        state.next[m] = 0;
        state.elapsed[m] = 0.0f;
        state.duration[m] = 0.0f;
        state.next_time[m] = 0.0f;
      }
    }
    const ControlMachine& machine = graph.machines[m];
    if (machine.sync_group == 0xFFu) continue;
    if (machine.phase_param != k_graph_no_param) {
      const f32 v = input.values[machine.phase_param];
      state.phase[machine.sync_group] = v - std::floor(v);
    } else {
      const f32 cycles = leader_cycles(graph, m, input, state, 0.0f);
      if (cycles > 0.0f)
        state.phase[machine.sync_group] = wrap01(state.phase[machine.sync_group] + gap * cycles);
    }
  }
}

u32 graph_control(const CompiledGraph& graph, std::span<AnimationGraphInput> inputs,
                  std::span<AnimationGraphState> states, f32 dt,
                  std::span<GraphStep> steps) noexcept {
  const u32 machines = graph.machines.size();
  const usize n = std::min(inputs.size(), states.size());
  u32 changed = 0;
  for (usize i = 0; i < n && (i + 1) * machines <= steps.size(); ++i) {
    if (graph_control(graph, inputs[i], states[i], dt, steps.data() + i * machines) != 0u)
      ++changed;
  }
  return changed;
}

// ---- the pose program's evaluation (stage 2) ---------------------------------------------------

namespace {

// One batch as the kernels see it: its instances, and registers 1.. in the evaluator's scratch.
struct Batch {
  const GraphInstance* instances = nullptr;
  u32 count = 0;
  u32 joints = 0;
  Vec3* translation = nullptr;
  Quat* rotation = nullptr;
  Vec3* scale = nullptr;

  anim::PoseView reg(u32 r, u32 i) const noexcept {
    if (r == 0) return instances[i].out;
    const usize at = (usize{r - 1} * count + i) * joints;
    return {std::span<Vec3>(translation + at, joints), std::span<Quat>(rotation + at, joints),
            std::span<Vec3>(scale + at, joints)};
  }
};

using Kernel = void (*)(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32* value,
                        const Library& library, const anim::Skeleton& skeleton);

// The rest pose, then the clip at value × k: the state's clock × its rate, or the group's phase ×
// the clip's duration. A clip writes only the joints its tracks name.
void kernel_sample(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32* value,
                   const Library& library, const anim::Skeleton& skeleton) {
  const bool looping = (op.b & k_pose_op_looping) != 0;
  if (op.data == Library::k_not_found) {
    for (u32 k = 0; k < n; ++k)
      anim::rest_pose(skeleton, b.reg(op.dst, list[k]));
    return;
  }
  const anim::Clip& clip = library.clip_data(op.data);
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const anim::PoseView out = b.reg(op.dst, i);
    anim::rest_pose(skeleton, out);
    clip.sample(value[i] * op.k, out, looping);
  }
}

// The first contributor: dst = w * a. At w = 1 the copy is exact, which is what makes a blend at a
// threshold, or a state with no transition in flight, the one clip to the bit.
void kernel_set(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32* value,
                const Library&, const anim::Skeleton&) {
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const f32 w = value[i];
    const anim::PoseView src = b.reg(op.a, i);
    const anim::PoseView dst = b.reg(op.dst, i);
    for (u32 j = 0; j < b.joints; ++j) {
      const Quat q = src.rotation[j];
      dst.translation[j] =
          Vec3{src.translation[j].x * w, src.translation[j].y * w, src.translation[j].z * w};
      dst.rotation[j] = Quat{q.x * w, q.y * w, q.z * w, q.w * w};
      dst.scale[j] = Vec3{src.scale[j].x * w, src.scale[j].y * w, src.scale[j].z * w};
    }
  }
}

// A later contributor: dst += w * a, the rotation's weight multiplied by the sign of its dot with
// the sum so far (q and -q are one rotation; adding the far one would cancel the near one). The
// sign is a select, not a branch.
void kernel_add(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32* value,
                const Library&, const anim::Skeleton&) {
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const f32 w = value[i];
    const anim::PoseView src = b.reg(op.a, i);
    const anim::PoseView dst = b.reg(op.dst, i);
    for (u32 j = 0; j < b.joints; ++j) {
      const Quat q = src.rotation[j];
      const Quat s = dst.rotation[j];
      const f32 d = s.x * q.x + s.y * q.y + s.z * q.z + s.w * q.w;
      const f32 sw = d < 0.0f ? -w : w;
      dst.rotation[j] = Quat{s.x + q.x * sw, s.y + q.y * sw, s.z + q.z * sw, s.w + q.w * sw};
      const Vec3 t = dst.translation[j];
      const Vec3 u = src.translation[j];
      dst.translation[j] = Vec3{t.x + u.x * w, t.y + u.y * w, t.z + u.z * w};
      const Vec3 c = dst.scale[j];
      const Vec3 e = src.scale[j];
      dst.scale[j] = Vec3{c.x + e.x * w, c.y + e.y * w, c.z + e.z * w};
    }
  }
}

// The sum of two or more contributors, normalized once: translation and scale over the weights'
// sum, each rotation to unit length (ozz-animation's blending job). Only `sqrt` and `/`, which IEEE
// rounds correctly on every compiler.
void kernel_normalize(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32* value,
                      const Library&, const anim::Skeleton&) {
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const f32 sum = value[i];
    const anim::PoseView dst = b.reg(op.dst, i);
    for (u32 j = 0; j < b.joints; ++j) {
      const Vec3 t = dst.translation[j];
      dst.translation[j] = Vec3{t.x / sum, t.y / sum, t.z / sum};
      const Vec3 s = dst.scale[j];
      dst.scale[j] = Vec3{s.x / sum, s.y / sum, s.z / sum};
      const Quat q = dst.rotation[j];
      const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
      dst.rotation[j] = len > 0.0f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
    }
  }
}

// Indexed by `PoseOpKind` for an op's first list (code 1); an Accumulate's second list (code 2)
// is `kernel_add`.
constexpr Kernel k_kernels[k_pose_op_kinds] = {kernel_sample, kernel_set, kernel_normalize};

}  // namespace

void PoseEvaluator::evaluate(const CompiledGraph& graph, const Library& library,
                             const anim::Skeleton& skeleton,
                             std::span<const GraphInstance> instances, u32 batch) {
  if (graph.program.empty() || instances.empty()) return;
  const usize step = batch == 0 ? usize{1} : (batch > 0xFFFFu ? usize{0xFFFFu} : usize{batch});
  for (usize first = 0; first < instances.size(); first += step) {
    const usize n = instances.size() - first < step ? instances.size() - first : step;
    run_batch(graph, library, skeleton, instances.subspan(first, n));
  }
}

void PoseEvaluator::run_batch(const CompiledGraph& graph, const Library& library,
                              const anim::Skeleton& skeleton,
                              std::span<const GraphInstance> instances) {
  const u32 count = static_cast<u32>(instances.size());
  const u32 joints = skeleton.joint_count();
  const u32 ops = graph.program.size();
  const usize cells = usize{ops} * count;
  if (code_.size() < cells) {
    code_.resize(static_cast<u32>(cells));
    value_.resize(static_cast<u32>(cells));
  }
  if (list_.size() < count) list_.resize(count);
  const usize slots = usize{graph.registers > 1 ? graph.registers - 1 : 0} * count * joints;
  if (rotation_.size() < slots) {
    translation_.resize(static_cast<u32>(slots));
    rotation_.resize(static_cast<u32>(slots));
    scale_.resize(static_cast<u32>(slots));
  }
  std::fill(code_.data(), code_.data() + cells, u8{0});
  u8* code = code_.data();
  f32* value = value_.data();

  // Each instance's ops: which apply, as what, with what weight or time. Its two states are
  // visited in index order, the program's, so the first contributor to a sum is the program's
  // first and the sum's order is the compiled one.
  const ControlMachine& machine = graph.machines[0];
  f32 w[k_graph_max_blend_inputs];
  for (u32 i = 0; i < count; ++i) {
    const GraphInstance& gi = instances[i];
    const AnimationGraphState& st = *gi.state;
    const bool live = st.graph == graph.id && st.state[0] != 0;
    u32 at[2] = {live ? u32{st.state[0]} : u32{machine.entry} + 1u, live ? u32{st.next[0]} : 0u};
    f32 clock[2] = {(live ? st.time[0] : 0.0f) + gi.lead,
                    (live ? st.next_time[0] : 0.0f) + gi.lead};
    f32 weight[2] = {1.0f, 0.0f};
    if (live) state_weights(st, 0, gi.lead, gi.interpolate, weight[0], weight[1]);
    if (at[1] != 0 && at[1] < at[0]) {
      std::swap(at[0], at[1]);
      std::swap(clock[0], clock[1]);
      std::swap(weight[0], weight[1]);
    }
    const f32 phase = machine.sync_group != 0xFFu ? st.phase[machine.sync_group] : 0.0f;
    u32 n0 = 0;
    f32 sum0 = 0.0f;
    for (u32 k = 0; k < 2; ++k) {
      if (at[k] == 0 || !(weight[k] > 0.0f)) continue;
      const PoseState& ps = graph.pose_states[machine.first_state + at[k] - 1];
      const f32 when = (ps.flags & k_pose_state_synced) != 0 ? phase : clock[k];
      const usize op = ps.first_op;
      clip_weights(graph, ps, *gi.input, w);
      usize to_out = op + 1;
      if ((ps.flags & k_pose_state_blend) == 0) {
        code[op * count + i] = 1;
        value[op * count + i] = when;
      } else {
        u32 nb = 0;
        f32 sumb = 0.0f;
        for (u32 c = 0; c < ps.clip_count; ++c) {
          if (!(w[c] > 0.0f)) continue;
          const usize s = op + 2u * c;
          code[s * count + i] = 1;
          value[s * count + i] = when;
          code[(s + 1) * count + i] = nb == 0 ? u8{1} : u8{2};
          value[(s + 1) * count + i] = w[c];
          ++nb;
          sumb += w[c];
        }
        const usize norm = op + 2u * ps.clip_count;
        code[norm * count + i] = nb >= 2 ? u8{1} : u8{0};
        value[norm * count + i] = sumb;
        to_out = norm + 1;
      }
      code[to_out * count + i] = n0 == 0 ? u8{1} : u8{2};
      value[to_out * count + i] = weight[k];
      ++n0;
      sum0 += weight[k];
    }
    const usize last = ops - 1u;
    code[last * count + i] = n0 >= 2 ? u8{1} : u8{0};
    value[last * count + i] = sum0;
  }

  // Op by op: the compacted list of the instances it applies to, and its kernel over that list.
  Batch b;
  b.instances = instances.data();
  b.count = count;
  b.joints = joints;
  b.translation = translation_.data();
  b.rotation = rotation_.data();
  b.scale = scale_.data();
  u16* list = list_.data();
  for (u32 o = 0; o < ops; ++o) {
    const GraphOp& op = graph.program[o];
    const u8* codes = code + usize{o} * count;
    const f32* values = value + usize{o} * count;
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
      list[n] = static_cast<u16>(i);
      n += codes[i] == 1 ? 1u : 0u;
    }
    if (n != 0) k_kernels[op.kind](op, b, list, n, values, library, skeleton);
    if (op.kind != static_cast<u8>(PoseOpKind::Accumulate)) continue;
    n = 0;
    for (u32 i = 0; i < count; ++i) {
      list[n] = static_cast<u16>(i);
      n += codes[i] == 2 ? 1u : 0u;
    }
    if (n != 0) kernel_add(op, b, list, n, values, library, skeleton);
  }
}

usize PoseEvaluator::bytes() const noexcept {
  return sizeof(PoseEvaluator) + translation_.capacity() * sizeof(Vec3) +
         rotation_.capacity() * sizeof(Quat) + scale_.capacity() * sizeof(Vec3) + code_.capacity() +
         value_.capacity() * sizeof(f32) + list_.capacity() * sizeof(u16);
}

// ---- the library -------------------------------------------------------------------------------

u32 GraphLibrary::add(CompiledGraph&& graph) {
  for (u32 i = 0; i < graphs_.size(); ++i) {
    if (graphs_[i]->id == graph.id) {
      *graphs_[i] = std::move(graph);
      return i;
    }
  }
  graphs_.push_back(std::make_unique<CompiledGraph>(std::move(graph)));
  return graphs_.size() - 1;
}

const CompiledGraph* GraphLibrary::find(const Id128& id) const noexcept {
  for (const std::unique_ptr<CompiledGraph>& g : graphs_) {
    if (g->id == id) return g.get();
  }
  return nullptr;
}

usize GraphLibrary::bytes() const noexcept {
  usize n = graphs_.capacity() * sizeof(std::unique_ptr<CompiledGraph>);
  for (const std::unique_ptr<CompiledGraph>& g : graphs_)
    n += g->bytes();
  return n;
}

}  // namespace engine::animation
