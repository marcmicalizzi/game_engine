#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document.h>
#include <systems/animation/graph.h>
#include <systems/animation/library.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>

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

// ---- the compile ---------------------------------------------------------------------------------

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

}  // namespace

// ---- read_graph ----------------------------------------------------------------------------------

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

// ---- the walker's graph ----------------------------------------------------------------------------

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
    if (reads_fast) t.record.conditions.push_back(GraphCondition{"fast", GraphCompare::Equal, fast});
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
  return n;
}

bool compile_graph(const GraphDesc& desc, const Library* library, CompiledGraph& out,
                   std::string& why) {
  out = CompiledGraph{};
  out.id = desc.id;
  out.name = desc.graph.name;
  if (desc.graph.skeleton_roles) return refuse(why, desc, "skeleton_roles is stage 2's");
  if (desc.graph.root_motion) return refuse(why, desc, "root_motion is a later stage's");

  // Parameters, in id order: a parameter's index is its slot in the input component.
  const Vector<const GraphItem<GraphParam>*> params = sorted(desc.params, desc.id);
  if (params.size() > k_graph_max_params) {
    return refuse(why, desc, std::to_string(params.size()) + " parameters, past the limit of " +
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
    return refuse(why, desc, std::to_string(machines.size()) + " machines, past the limit of " +
                                 std::to_string(k_graph_max_machines));
  }
  // Nodes under anything but a state are stage 2's (a blend tree, a layer).
  for (const GraphItem<GraphNode>& n : desc.nodes) {
    bool under_state = false;
    for (const GraphItem<GraphState>& s : desc.states)
      under_state = under_state || s.id == n.parent;
    if (!under_state) return refuse(why, desc, "a node outside a state's pose is stage 2's");
  }
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
      return refuse(why, desc, "machine '" + mname + "': " + std::to_string(states.size()) +
                                   " states, past the limit of " +
                                   std::to_string(k_graph_max_states));
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
        return refuse(why, desc, "machine '" + mname + "': phase_param '" +
                                     machine.record.phase_param + "' is not a float parameter");
      }
    }

    // The states and what plays each.
    for (const GraphItem<GraphState>* s : states) {
      const Vector<const GraphItem<GraphNode>*> pose = sorted(desc.nodes, s->id);
      const std::string where = "state '" + mname + "." + s->record.name + "'";
      if (pose.size() != 1) {
        return refuse(why, desc, where + " has " + std::to_string(pose.size()) +
                                     " pose nodes; a state's pose is one node");
      }
      const GraphNode& node = pose[0]->record;
      if (node.kind != GraphNodeKind::Clip)
        return refuse(why, desc, where + ": a node other than a Clip is a later stage's");
      if (!node.inputs.empty() || !node.thresholds.empty() || !node.param.empty())
        return refuse(why, desc, where + ": a Clip node takes no inputs, thresholds or parameter");
      if (node.sync_group > k_graph_max_sync_groups) {
        return refuse(why, desc, where + ": sync group " + std::to_string(node.sync_group) +
                                     ", past the limit of " +
                                     std::to_string(k_graph_max_sync_groups));
      }
      if (!std::isfinite(node.rate) || !std::isfinite(node.weight) || node.weight != 1.0f)
        return refuse(why, desc, where + ": a weight other than 1 is stage 2's");
      StateInfo& info = out.state_info.emplace_back();
      info.id = s->id;
      info.name = s->record.name;
      info.clip = node.clip;
      info.rate = node.rate;
      info.looping = node.looping;
      if (!node.clip.empty()) {
        info.clip_id = Library::clip_id(node.clip);
        if (library != nullptr) {
          info.clip_index = library->clip_index(info.clip_id);
          if (info.clip_index == Library::k_not_found)
            return refuse(why, desc, where + " plays clip '" + node.clip + "', not in the library");
        }
      }
      if (node.sync_group != 0) {
        const u8 group = static_cast<u8>(node.sync_group - 1);
        if (cm.phase_param == k_graph_no_param) {
          return refuse(why, desc, where + " is phase-locked, and machine '" + mname +
                                       "' has no phase_param (a leader's phase is stage 2's)");
        }
        if (cm.sync_group != 0xFFu && cm.sync_group != group)
          return refuse(why, desc, "machine '" + mname + "' drives two sync groups");
        cm.sync_group = group;
        info.synced = true;
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
      if (r.exit_phase.has_value()) return refuse(why, desc, where + ": exit_phase is stage 2's");
      if (r.blend == GraphBlend::Inertialize)
        return refuse(why, desc, where + ": Inertialize is stage 4's");
      if (r.offset != GraphOffset::Zero)
        return refuse(why, desc, where + ": an offset other than Zero is stage 2's");
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
        return refuse(why, desc, "machine '" + mname + "': transition " + hex(t->id) +
                                     " leaves a state not of this machine");
      }
    }
  }
  if (out.transitions.size() > k_graph_max_records)
    return refuse(why, desc, "more transitions than the limit of 65,535");
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

u32 graph_control(const CompiledGraph& graph, AnimationGraphInput& input, AnimationGraphState& state,
                  f32 dt, GraphStep* steps) noexcept {
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
      const u32 current = static_cast<u32>((state.next[m] != 0 ? state.next[m] : state.state[m]) - 1);
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
      const f32 v = input.values[machine.phase_param];
      state.phase[machine.sync_group] = v - std::floor(v);
    }
  }
  return entered;
}

u32 graph_control(const CompiledGraph& graph, std::span<AnimationGraphInput> inputs,
                  std::span<AnimationGraphState> states, f32 dt, std::span<GraphStep> steps) noexcept {
  const u32 machines = graph.machines.size();
  const usize n = std::min(inputs.size(), states.size());
  u32 changed = 0;
  for (usize i = 0; i < n && (i + 1) * machines <= steps.size(); ++i) {
    if (graph_control(graph, inputs[i], states[i], dt, steps.data() + i * machines) != 0u) ++changed;
  }
  return changed;
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
