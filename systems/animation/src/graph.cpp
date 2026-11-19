#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/anim/standard_skeleton.h>
#include <domain/doc/document.h>
#include <systems/animation/graph.h>
#include <systems/animation/library.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <type_traits>
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
    } else if (is_type<GraphMask>(object)) {
      ok = read_record(object, out.masks.emplace_back(), why);
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

// A list's items grouped by parent, each group in id order, found by binary search: what `sorted`
// gives for one parent, for every parent at once. Built once in n log n, so the compile visits a
// record a bounded number of times however many states and nodes a graph has (the review of batch
// 69, finding 6: a `sorted` a state and a scan a node made the compile quadratic in both).
template <class T>
class ByParent {
 public:
  explicit ByParent(const Vector<GraphItem<T>>& items) {
    items_.reserve(items.size());
    for (const GraphItem<T>& item : items)
      items_.push_back(&item);
    std::stable_sort(items_.begin(), items_.end(),
                     [](const GraphItem<T>* a, const GraphItem<T>* b) {
                       return a->parent != b->parent ? a->parent < b->parent : a->id < b->id;
                     });
  }
  std::span<const GraphItem<T>* const> children(const Id128& parent) const noexcept {
    const auto lo =
        std::lower_bound(items_.begin(), items_.end(), parent,
                         [](const GraphItem<T>* item, const Id128& p) { return item->parent < p; });
    const auto hi =
        std::upper_bound(lo, items_.end(), parent,
                         [](const Id128& p, const GraphItem<T>* item) { return p < item->parent; });
    return {lo, static_cast<usize>(hi - lo)};
  }

 private:
  Vector<const GraphItem<T>*> items_;
};

// A list's items by id, found by binary search (several items may share an id: a malformed graph).
template <class T>
class ById {
 public:
  explicit ById(const Vector<GraphItem<T>>& items) {
    items_.reserve(items.size());
    for (const GraphItem<T>& item : items)
      items_.push_back(&item);
    std::stable_sort(items_.begin(), items_.end(),
                     [](const GraphItem<T>* a, const GraphItem<T>* b) { return a->id < b->id; });
  }
  std::span<const GraphItem<T>* const> find(const Id128& id) const noexcept {
    const auto lo =
        std::lower_bound(items_.begin(), items_.end(), id,
                         [](const GraphItem<T>* item, const Id128& i) { return item->id < i; });
    const auto hi =
        std::upper_bound(lo, items_.end(), id,
                         [](const Id128& i, const GraphItem<T>* item) { return i < item->id; });
    return {lo, static_cast<usize>(hi - lo)};
  }

 private:
  Vector<const GraphItem<T>*> items_;
};

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
  // An inertialization in flight (a negative duration, stage 4): the target alone, from its first
  // tick; the source is evaluated once, for the offsets, by the host (`inertial_capture`).
  if (st.duration[m] < 0.0f) {
    w_cur = 0.0f;
    w_next = 1.0f;
    return;
  }
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

// What the compile found about layers (stage 3), for the program: which machine is the base and
// which the layer, or why there is no program.
struct LayerPlan {
  bool layered = false;
  u32 base = 0;
  u32 layer = 0;
  std::string no_program;
};

// The pose program (graph.h, "the pose program"): per state of the base machine in index order its
// ops into register 0, then that register's normalize; with a layer, the layer machine's the same
// into the layer register, its normalize, and the layer op. Register 0 is the instance's slot, 1 a
// clip's sample, 2 a Blend1D's sum, and the layer register the one after.
void compile_program(CompiledGraph& g, const LayerPlan& plan) {
  g.program.clear();
  g.registers = 0;
  g.program_why.clear();
  if (!plan.no_program.empty()) {
    g.program_why = plan.no_program;
    return;
  }
  const u32 order[2] = {plan.base, plan.layer};
  const u32 machine_count = plan.layered ? 2u : 1u;
  // A state cross-faded into itself would be two copies of one state on two clocks, which the
  // program's one op run a state cannot play.
  for (u32 mi = 0; mi < machine_count; ++mi) {
    const ControlMachine& machine = g.machines[order[mi]];
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
  }
  usize ops = machine_count + (plan.layered ? 1u : 0u);
  bool any_blend = false;
  for (u32 mi = 0; mi < machine_count; ++mi) {
    const ControlMachine& machine = g.machines[order[mi]];
    for (u32 s = 0; s < machine.state_count; ++s) {
      const PoseState& ps = g.pose_states[machine.first_state + s];
      const bool blend = (ps.flags & k_pose_state_blend) != 0;
      any_blend = any_blend || blend;
      ops += blend ? 2u * ps.clip_count + 2u : 2u;
    }
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
  const u8 layer_register = plan.layered ? (any_blend ? u8{3} : u8{2}) : u8{0};
  for (u32 mi = 0; mi < machine_count; ++mi) {
    const u32 m = order[mi];
    const ControlMachine& machine = g.machines[m];
    const u8 out = mi == 0 ? u8{0} : layer_register;
    for (u32 s = 0; s < machine.state_count; ++s) {
      PoseState& ps = g.pose_states[machine.first_state + s];
      ps.first_op = static_cast<u16>(g.program.size());
      if ((ps.flags & k_pose_state_blend) == 0) {
        sample(s, ps, 0);
        accumulate(s, out, 1, 0, k_pose_op_state);
      } else {
        for (u32 c = 0; c < ps.clip_count; ++c) {
          sample(s, ps, c);
          accumulate(s, 2, 1, c, 0);
        }
        normalize(s, 2);
        accumulate(s, out, 2, 0, k_pose_op_state);
      }
    }
    g.out_op[m] = static_cast<u16>(g.program.size());
    g.out_register[m] = out;
    normalize(0xFFFFu, out);
  }
  g.base_machine = static_cast<u8>(plan.base);
  g.layer_register = layer_register;
  if (plan.layered) {
    GraphLayer& layer = g.layers[0];
    layer.op = static_cast<u16>(g.program.size());
    GraphOp& op = g.program.emplace_back();
    op.kind = layer.kind;
    op.dst = 0;
    op.a = layer_register;
    op.node = 0xFFFFu;
    op.data = 0;
  }
  g.registers = plan.layered ? u32{layer_register} + 1u : (any_blend ? 3u : 2u);
}

// The mask of a layer node, compiled for `skeleton` into a weight per joint (graph.h, "Layers";
// GraphMaskJoint in the schema). An entry names a standard-skeleton role, or failing that a joint;
// with `subtree` its descendants take its weight too, tapered over `depth` generations (Unreal's
// Blend Depth: the joint `k` generations below the named one at min(1, (k + 1) / depth) of the
// weight). A joint two entries reach takes the larger weight.
bool compile_mask(const GraphDesc& desc, const std::string& name, const anim::Skeleton& skeleton,
                  Vector<f32>& weights, std::string& why) {
  const u32 joints = skeleton.joint_count();
  weights.assign(joints, name.empty() ? 1.0f : 0.0f);
  if (name.empty()) return true;
  const GraphItem<GraphMask>* mask = nullptr;
  for (const GraphItem<GraphMask>& m : desc.masks) {
    if (m.parent != desc.id || m.record.name != name) continue;
    if (mask != nullptr) return refuse(why, desc, "two masks named '" + name + "'");
    mask = &m;
  }
  if (mask == nullptr)
    return refuse(why, desc, "a layer node names mask '" + name + "', not a mask of the graph");
  const anim::JointMapping roles = anim::map_joints(skeleton);
  Vector<i32> depth(joints, -1);  // generations below the entry's joint; -1: outside its subtree
  for (const GraphMaskJoint& entry : mask->record.joints) {
    const std::string where = "mask '" + name + "': joint '" + entry.joint + "'";
    if (!std::isfinite(entry.weight) || entry.weight < 0.0f || entry.weight > 1.0f)
      return refuse(why, desc, where + ": a weight outside [0, 1]");
    i32 at = anim::k_no_joint;
    const anim::StandardJoint role = anim::standard_joint_from_name(entry.joint);
    if (role != anim::StandardJoint::Count) at = roles[role];
    if (at == anim::k_no_joint) at = skeleton.find(entry.joint);
    if (at == anim::k_no_joint) {
      return refuse(why, desc,
                    where + " is neither a role this skeleton maps nor one of its joints");
    }
    for (u32 j = 0; j < joints; ++j)
      depth[j] = -1;
    depth[static_cast<u32>(at)] = 0;
    // A parent comes before its joint (anim::Skeleton), so one forward pass finds the subtree.
    if (entry.subtree) {
      for (u32 j = static_cast<u32>(at) + 1; j < joints; ++j) {
        const i32 p = skeleton.parents[j];
        if (p != anim::k_no_joint && depth[static_cast<u32>(p)] >= 0)
          depth[j] = depth[static_cast<u32>(p)] + 1;
      }
    }
    for (u32 j = 0; j < joints; ++j) {
      if (depth[j] < 0) continue;
      f32 taper = 1.0f;
      if (entry.depth != 0 && depth[j] + 1 < static_cast<i32>(entry.depth))
        taper = static_cast<f32>(depth[j] + 1) / static_cast<f32>(entry.depth);
      const f32 w = entry.weight * taper;
      if (w > weights[j]) weights[j] = w;
    }
  }
  return true;
}

// Which machine is a layer, its node's weight, parameter, mask and reference pose (stage 3). A
// refusal is returned false with `why`; a graph that compiles but has no program says why in
// `plan.no_program`.
bool plan_layers(const GraphDesc& desc, const Vector<const GraphItem<GraphMachine>*>& machines,
                 const Library* library, const anim::Skeleton* skeleton, CompiledGraph& out,
                 LayerPlan& plan, std::string& why) {
  const GraphItem<GraphNode>* node[k_graph_max_machines] = {nullptr, nullptr};
  u32 layers = 0;
  for (u32 m = 0; m < machines.size(); ++m) {
    for (const GraphItem<GraphNode>& n : desc.nodes) {
      if (n.parent != machines[m]->id) continue;
      if (node[m] != nullptr) {
        return refuse(why, desc,
                      "machine '" + machines[m]->record.name +
                          "' has two layer nodes (two layers of one machine are in no stage of "
                          "the design yet)");
      }
      node[m] = &n;
      ++layers;
    }
  }
  if (machines.size() == 1) {
    if (layers != 0) {
      return refuse(why, desc,
                    "machine '" + machines[0]->record.name +
                        "' is a layer over no base (a graph of one machine)");
    }
    return true;
  }
  if (layers == 0) {
    plan.no_program =
        "a graph of 2 machines and no layer node: composing their poses is a layer's, an "
        "Override or Additive child node of one machine";
    return true;
  }
  if (layers == 2) {
    return refuse(why, desc,
                  "both machines are layers (a layer of a layer is in no stage of the design "
                  "yet)");
  }
  plan.layered = true;
  plan.layer = node[0] != nullptr ? 0u : 1u;
  plan.base = 1u - plan.layer;
  const GraphNode& r = node[plan.layer]->record;
  const std::string where = "the layer node of machine '" + machines[plan.layer]->record.name + "'";
  if (!std::isfinite(r.weight) || r.weight < 0.0f || r.weight > 1.0f)
    return refuse(why, desc, where + ": a weight outside [0, 1]");
  if (!r.inputs.empty() || !r.thresholds.empty() || !r.clip.empty() || r.sync_group != 0)
    return refuse(why, desc, where + " takes a weight, a parameter, a mask and a reference only");
  GraphLayer layer;
  layer.kind = static_cast<u8>(r.kind == GraphNodeKind::Override ? PoseOpKind::Override
                                                                 : PoseOpKind::Additive);
  layer.machine = static_cast<u8>(plan.layer);
  layer.weight = r.weight;
  if (!r.param.empty()) {
    const u32 p = out.param_index(r.param);
    if (p == k_absent || out.params[p].kind != GraphParamKind::Float)
      return refuse(why, desc,
                    where + ": its weight reads a float parameter, not '" + r.param + "'");
    layer.param = static_cast<u16>(p);
  }
  if (skeleton == nullptr) {
    plan.no_program =
        "a layer's mask and reference pose are compiled per skeleton, and this graph was "
        "compiled with none";
    out.layers.push_back(layer);
    return true;
  }
  const u32 joints = skeleton->joint_count();
  if (joints == 0 || joints > 0xFFFFu)
    return refuse(why, desc, where + ": a skeleton of " + std::to_string(joints) + " joints");
  Vector<f32> weights;
  if (!compile_mask(desc, r.mask, *skeleton, weights, why)) return false;
  layer.first_joint = out.layer_joints.size();
  for (u32 pass = 0; pass < 2; ++pass) {
    for (u32 j = 0; j < joints; ++j) {
      const bool full = weights[j] == 1.0f;
      if (pass == 0 ? !full : (full || !(weights[j] > 0.0f))) continue;
      out.layer_joints.push_back(static_cast<u16>(j));
      out.layer_weights.push_back(weights[j]);
      if (pass == 0)
        ++layer.full;
      else
        ++layer.partial;
    }
  }
  if (layer.full + layer.partial == 0)
    return refuse(why, desc, where + ": mask '" + r.mask + "' touches no joint");
  if (r.kind == GraphNodeKind::Additive) {
    // The reference: the skeleton's rest pose, then the named clip's first key over it.
    anim::Pose reference;
    anim::rest_pose(*skeleton, reference);
    if (!r.reference.empty() && library != nullptr) {
      const u32 clip = library->clip_index(Library::clip_id(r.reference));
      if (clip == Library::k_not_found) {
        return refuse(why, desc,
                      where + ": reference clip '" + r.reference + "' is not in the library");
      }
      library->clip_data(clip).sample(0.0f, reference, false);
    }
    for (u32 j = 0; j < joints; ++j) {
      const Vec3 s = reference.scale[j];
      if (!(s.x != 0.0f && s.y != 0.0f && s.z != 0.0f) || !std::isfinite(s.x) ||
          !std::isfinite(s.y) || !std::isfinite(s.z)) {
        return refuse(why, desc,
                      where + ": the reference pose has a zero or non-finite scale at joint " +
                          std::to_string(j) + " (an additive scale is a quotient)");
      }
    }
    layer.reference = out.reference_rotation.size();
    for (u32 j = 0; j < joints; ++j) {
      out.reference_translation.push_back(reference.translation[j]);
      out.reference_rotation.push_back(reference.rotation[j]);
      out.reference_scale.push_back(reference.scale[j]);
    }
  }
  out.layers.push_back(layer);
  out.skeleton_joints = joints;
  return true;
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
  n += layers.capacity() * sizeof(GraphLayer);
  n += layer_joints.capacity() * sizeof(u16) + layer_weights.capacity() * sizeof(f32);
  n += reference_translation.capacity() * sizeof(Vec3) +
       reference_rotation.capacity() * sizeof(Quat) + reference_scale.capacity() * sizeof(Vec3);
  return n;
}

// The compiled tables' revision: every table an instance's indices point into, and the program
// they drive, hashed as bytes (each record's padding is a named field set to zero, so the bytes
// are the values). Never 0, which a state written before `AnimationGraphState` version 2 holds.
static u32 graph_revision(const CompiledGraph& g) noexcept {
  u64 h = hash_bytes(&g.id, sizeof(g.id));
  const auto add = [&h](const auto& table) {
    using T = std::remove_cvref_t<decltype(table[0])>;
    h = hash_bytes(table.data(), usize{table.size()} * sizeof(T), h);
    h = hash_combine(h, table.size());
  };
  add(g.machines);
  add(g.states);
  add(g.transitions);
  add(g.conditions);
  add(g.pose_states);
  add(g.pose_clips);
  add(g.program);
  add(g.layers);
  add(g.layer_joints);
  add(g.layer_weights);
  h = hash_combine(h, g.registers);
  const u32 r = static_cast<u32>(h ^ (h >> 32));
  return r != 0 ? r : 1u;
}

bool compile_graph(const GraphDesc& desc, const Library* library, CompiledGraph& out,
                   std::string& why, const anim::Skeleton* skeleton) {
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
  // A node is a state's pose, an input of a Blend1D that is one, or a machine's layer node (stage
  // 3); nothing else is in any stage of the design.
  // Every lookup below is a binary search over a table built once, so the compile is n log n in
  // the records (it was quadratic in the nodes, and in states times nodes; finding 6 above).
  const ById<GraphState> states_by_id(desc.states);
  const ById<GraphNode> nodes_by_id(desc.nodes);
  const ByParent<GraphNode> nodes_by_parent(desc.nodes);
  const ByParent<GraphState> states_by_parent(desc.states);
  const ByParent<GraphTransition> transitions_by_parent(desc.transitions);
  const auto is_state = [&](const Id128& id) { return !states_by_id.find(id).empty(); };
  const auto is_machine = [&](const Id128& id) {
    for (const GraphItem<GraphMachine>* m : machines) {
      if (m->id == id) return true;
    }
    return false;
  };
  for (const GraphItem<GraphNode>& n : desc.nodes) {
    bool placed = is_state(n.parent) || is_machine(n.parent);
    for (const GraphItem<GraphNode>* p : nodes_by_id.find(n.parent)) {
      placed = placed || (p->record.kind == GraphNodeKind::Blend1D && is_state(p->parent));
    }
    if (!placed) {
      return refuse(why, desc,
                    "a node outside a state's pose, a Blend1D's inputs or a machine's layer node "
                    "is in no stage of the design yet");
    }
    const bool layer = is_machine(n.parent);
    if (layer && n.record.kind != GraphNodeKind::Override &&
        n.record.kind != GraphNodeKind::Additive) {
      return refuse(why, desc, "a machine's child node is its layer node: an Override or Additive");
    }
    if (!layer &&
        (n.record.kind == GraphNodeKind::Override || n.record.kind == GraphNodeKind::Additive)) {
      return refuse(why, desc,
                    "an Override or Additive node is a machine's layer node (a layer inside a "
                    "state's pose is in no stage of the design yet)");
    }
    if (!layer && (!n.record.mask.empty() || !n.record.reference.empty()))
      return refuse(why, desc, "a mask or a reference is a layer node's");
    if (layer && n.record.kind == GraphNodeKind::Override && !n.record.reference.empty())
      return refuse(why, desc, "a reference pose is an Additive layer's, not an Override's");
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
    const std::span<const GraphItem<GraphState>* const> states =
        states_by_parent.children(machine.id);
    if (states.empty()) return refuse(why, desc, "machine '" + mname + "' has no state");
    if (states.size() > k_graph_max_states) {
      return refuse(why, desc,
                    "machine '" + mname + "': " + std::to_string(states.size()) +
                        " states, past the limit of " + std::to_string(k_graph_max_states));
    }
    ControlMachine& cm = out.machines.emplace_back();
    cm.first_state = out.states.size();
    cm.state_count = static_cast<u16>(states.size());
    // A state's index in the machine: the first of its id in the id-ordered states.
    const auto local = [&](const Id128& id) {
      const auto at = std::lower_bound(
          states.begin(), states.end(), id,
          [](const GraphItem<GraphState>* s, const Id128& i) { return s->id < i; });
      return at != states.end() && (*at)->id == id ? static_cast<u32>(at - states.begin())
                                                   : k_absent;
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
      const std::span<const GraphItem<GraphNode>* const> pose = nodes_by_parent.children(s->id);
      const std::string where = "state '" + mname + "." + s->record.name + "'";
      if (pose.size() != 1) {
        return refuse(why, desc,
                      where + " has " + std::to_string(pose.size()) +
                          " pose nodes; a state's pose is one node");
      }
      const GraphNode& node = pose[0]->record;
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
      if (!std::isfinite(node.rate) || !std::isfinite(node.weight) || node.weight != 1.0f) {
        return refuse(why, desc,
                      where +
                          ": a weight other than 1 is a layer node's (a weight inside a state's "
                          "pose is in no stage of the design yet)");
      }
      StateInfo& info = out.state_info.emplace_back();
      info.id = s->id;
      info.name = s->record.name;
      info.rate = node.rate;
      info.looping = node.looping;
      PoseState& ps = out.pose_states.emplace_back();
      ps.first_clip = static_cast<u16>(out.pose_clips.size());
      ps.flags = node.looping ? k_pose_state_looping : u8{0};
      const std::span<const GraphItem<GraphNode>* const> children =
          nodes_by_parent.children(pose[0]->id);
      if (node.kind == GraphNodeKind::Clip) {
        if (!node.inputs.empty() || !node.thresholds.empty() || !node.param.empty() ||
            !children.empty()) {
          return refuse(why, desc,
                        where + ": a Clip node takes no inputs, thresholds or parameter");
        }
        // A Clip state's one clip counts against the same limit a Blend1D's do: `first_clip` is a
        // u16 (the review of batch 69, finding 6: two machines of Clip states could pass it).
        if (out.pose_clips.size() + 1u > k_graph_max_records)
          return refuse(why, desc, "more pose clips than the limit of 65,535");
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
              !in.param.empty() || !nodes_by_parent.children(input->id).empty()) {
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
    const std::span<const GraphItem<GraphTransition>* const> by_id =
        transitions_by_parent.children(machine.id);
    Vector<const GraphItem<GraphTransition>*> transitions;
    transitions.reserve(static_cast<u32>(by_id.size()));
    for (const GraphItem<GraphTransition>* t : by_id)
      transitions.push_back(t);
    std::stable_sort(transitions.begin(), transitions.end(),
                     [](const GraphItem<GraphTransition>* a, const GraphItem<GraphTransition>* b) {
                       return a->record.priority < b->record.priority;
                     });
    // The same order grouped by the state each leaves (stable: (priority, id) within a group), so
    // a state's run is found by a binary search rather than a pass over the machine's transitions.
    Vector<const GraphItem<GraphTransition>*> by_from = transitions;
    std::stable_sort(by_from.begin(), by_from.end(),
                     [](const GraphItem<GraphTransition>* a, const GraphItem<GraphTransition>* b) {
                       return a->record.from < b->record.from;
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
      const Id128& from = states[si]->id;
      for (auto t = std::lower_bound(by_from.begin(), by_from.end(), from,
                                     [](const GraphItem<GraphTransition>* a, const Id128& f) {
                                       return a->record.from < f;
                                     });
           t != by_from.end() && (*t)->record.from == from; ++t) {
        if (!emit(**t)) return false;
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
  LayerPlan plan;
  if (!plan_layers(desc, machines, library, skeleton, out, plan, why)) return false;
  // Stage 4 inertializes the base machine's pose (the host's `inertial_capture` over the program's
  // output); a layer machine's transitions blend inside the program, which has no offsets.
  if (plan.layered) {
    const ControlMachine& layer = out.machines[plan.layer];
    const u32 first = layer.any_first;
    u32 last = first + layer.any_count;
    for (u32 s = 0; s < layer.state_count; ++s) {
      const ControlState& cs = out.states[layer.first_state + s];
      last = std::max(last, u32{cs.first_transition} + cs.transition_count);
    }
    for (u32 t = first; t < last; ++t) {
      if (out.transitions[t].blend == static_cast<u8>(GraphBlend::Inertialize))
        return refuse(why, desc, "an Inertialize transition on a layer machine is in no stage yet");
    }
  }
  compile_program(out, plan);
  out.revision = graph_revision(out);
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
  state.revision = graph.revision;
}

u32 graph_control(const CompiledGraph& graph, AnimationGraphInput& input,
                  AnimationGraphState& state, f32 dt, GraphStep* steps) noexcept {
  // Before any index of `state` is read: a state written for another graph, or for this graph
  // before it was replaced in place, starts again at the entries (its indices may name states the
  // new compile does not have).
  if (state.graph != graph.id || state.revision != graph.revision) {
    state = AnimationGraphState{};
    state.graph = graph.id;
    state.revision = graph.revision;
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
        if (state.elapsed[m] >= std::fabs(state.duration[m])) {
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
          // An inertialization is held like a cross-fade with its duration negated: the evaluator
          // then weighs the target alone (`state_weights`) and the host adds the offsets.
          state.next[m] = static_cast<u16>(tr.to + 1);
          state.duration[m] =
              tr.blend == static_cast<u8>(GraphBlend::Inertialize) ? -tr.duration_s : tr.duration_s;
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
  if (state.graph != graph.id || state.revision != graph.revision || !(gap > 0.0f)) return;
  for (u32 m = 0; m < graph.machines.size(); ++m) {
    if (state.state[m] == 0) continue;
    state.time[m] += gap;
    if (state.next[m] != 0) {
      state.next_time[m] += gap;
      state.elapsed[m] += gap;
      if (state.elapsed[m] >= std::fabs(state.duration[m])) {
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
  const CompiledGraph* graph = nullptr;
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

// ---- the layer ops (stage 3) ----

// One joint of an override at `t` (the layer's weight × the joint's mask weight, in (0, 1)): the
// normalized weighted sum of the base at 1 − t and the layer at t — ozz-animation's blend of two
// layers whose weights sum to 1, with the per-joint weight its `joint_weights` are — so a weight
// below 1 leaves the rest to the base and never to the rest pose.
inline void override_joint(const anim::PoseView& dst, const anim::PoseView& src, u32 j, f32 t) {
  const f32 u = 1.0f - t;
  const f32 sum = u + t;
  const Quat b = dst.rotation[j];
  const Quat l = src.rotation[j];
  const f32 d = b.x * l.x + b.y * l.y + b.z * l.z + b.w * l.w;
  const f32 lt = d < 0.0f ? -t : t;
  const Quat q{b.x * u + l.x * lt, b.y * u + l.y * lt, b.z * u + l.z * lt, b.w * u + l.w * lt};
  const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  dst.rotation[j] = len > 0.0f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : b;
  const Vec3 bt = dst.translation[j];
  const Vec3 tt = src.translation[j];
  dst.translation[j] =
      Vec3{(bt.x * u + tt.x * t) / sum, (bt.y * u + tt.y * t) / sum, (bt.z * u + tt.z * t) / sum};
  const Vec3 bs = dst.scale[j];
  const Vec3 ts = src.scale[j];
  dst.scale[j] =
      Vec3{(bs.x * u + ts.x * t) / sum, (bs.y * u + ts.y * t) / sum, (bs.z * u + ts.z * t) / sum};
}

// An override at weight 1 (code 1): the mask's full joints are the layer's, copied; its partial
// joints blended at their mask weights. No other joint is written.
void kernel_override_full(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32*,
                          const Library&, const anim::Skeleton&) {
  const GraphLayer& layer = b.graph->layers[op.data];
  const u16* joints = b.graph->layer_joints.data() + layer.first_joint;
  const f32* weights = b.graph->layer_weights.data() + layer.first_joint;
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const anim::PoseView src = b.reg(op.a, i);
    const anim::PoseView dst = b.reg(op.dst, i);
    for (u32 m = 0; m < layer.full; ++m) {
      const u32 j = joints[m];
      dst.translation[j] = src.translation[j];
      dst.rotation[j] = src.rotation[j];
      dst.scale[j] = src.scale[j];
    }
    for (u32 m = layer.full; m < u32{layer.full} + layer.partial; ++m)
      override_joint(dst, src, joints[m], weights[m]);
  }
}

// An override at a weight in (0, 1) (code 2): every joint of the mask at the weight × its own.
void kernel_override_partial(const GraphOp& op, const Batch& b, const u16* list, u32 n,
                             const f32* value, const Library&, const anim::Skeleton&) {
  const GraphLayer& layer = b.graph->layers[op.data];
  const u16* joints = b.graph->layer_joints.data() + layer.first_joint;
  const f32* weights = b.graph->layer_weights.data() + layer.first_joint;
  const u32 count = u32{layer.full} + layer.partial;
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const f32 w = value[i];
    const anim::PoseView src = b.reg(op.a, i);
    const anim::PoseView dst = b.reg(op.dst, i);
    for (u32 m = 0; m < count; ++m)
      override_joint(dst, src, joints[m], w * weights[m]);
  }
}

// An additive layer at any weight in (0, 1] (codes 1 and 2), over the mask's joints: the layer's
// difference from its reference pose — translation `l − r`, rotation `conj(r) · l`, scale `l / r`
// (ozz-animation's additive builder) — applied to the base by ozz's blending job's additive pass at
// `t` (the weight × the joint's mask weight): translation `+ d · t`; rotation **on the right**,
// `base · normalize(lerp(identity, ±d, t))`, the difference's sign taken so the lerp is the short
// way; scale `× (d · t + (1 − t))`. A joint whose layer pose is its reference pose exactly is left
// as the base — that is what an additive of nothing is, and it keeps the bits (a −0 in the base
// would otherwise come back +0).
void kernel_additive(const GraphOp& op, const Batch& b, const u16* list, u32 n, const f32* value,
                     const Library&, const anim::Skeleton&) {
  const CompiledGraph& g = *b.graph;
  const GraphLayer& layer = g.layers[op.data];
  const u16* joints = g.layer_joints.data() + layer.first_joint;
  const f32* weights = g.layer_weights.data() + layer.first_joint;
  const Vec3* rt = g.reference_translation.data() + layer.reference;
  const Quat* rr = g.reference_rotation.data() + layer.reference;
  const Vec3* rs = g.reference_scale.data() + layer.reference;
  const u32 count = u32{layer.full} + layer.partial;
  for (u32 k = 0; k < n; ++k) {
    const u32 i = list[k];
    const f32 w = value[i];
    const anim::PoseView src = b.reg(op.a, i);
    const anim::PoseView dst = b.reg(op.dst, i);
    for (u32 m = 0; m < count; ++m) {
      const u32 j = joints[m];
      const f32 t = w * weights[m];
      const Vec3 lt = src.translation[j];
      const Quat lr = src.rotation[j];
      const Vec3 ls = src.scale[j];
      const bool still = lt.x == rt[j].x && lt.y == rt[j].y && lt.z == rt[j].z && lr.x == rr[j].x &&
                         lr.y == rr[j].y && lr.z == rr[j].z && lr.w == rr[j].w && ls.x == rs[j].x &&
                         ls.y == rs[j].y && ls.z == rs[j].z;
      const Quat d = conjugate(rr[j]) * lr;
      const f32 sign = d.w < 0.0f ? -1.0f : 1.0f;
      const Quat q{d.x * sign * t, d.y * sign * t, d.z * sign * t, (d.w * sign - 1.0f) * t + 1.0f};
      const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
      const Quat e = len > 0.0f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
      const Vec3 bt = dst.translation[j];
      const Quat br = dst.rotation[j];
      const Vec3 bs = dst.scale[j];
      const f32 u = 1.0f - t;
      const Vec3 ot{bt.x + (lt.x - rt[j].x) * t, bt.y + (lt.y - rt[j].y) * t,
                    bt.z + (lt.z - rt[j].z) * t};
      const Quat orr = br * e;
      const Vec3 os{bs.x * ((ls.x / rs[j].x) * t + u), bs.y * ((ls.y / rs[j].y) * t + u),
                    bs.z * ((ls.z / rs[j].z) * t + u)};
      dst.translation[j] = still ? bt : ot;
      dst.rotation[j] = still ? br : orr;
      dst.scale[j] = still ? bs : os;
    }
  }
}

// Indexed by `PoseOpKind`: the kernel over an op's first list (code 1), and over its second list
// (code 2) where the kind has one — an Accumulate's later contributors, a layer's weights below 1.
constexpr Kernel k_kernels[k_pose_op_kinds] = {kernel_sample, kernel_set, kernel_normalize,
                                               kernel_override_full, kernel_additive};
constexpr Kernel k_second_kernels[k_pose_op_kinds] = {nullptr, kernel_add, nullptr,
                                                      kernel_override_partial, kernel_additive};

}  // namespace

void PoseEvaluator::evaluate(const CompiledGraph& graph, const Library& library,
                             const anim::Skeleton& skeleton,
                             std::span<const GraphInstance> instances, u32 batch) {
  if (graph.program.empty() || instances.empty()) return;
  // A layered graph's masks and reference pose are one skeleton's (compile_graph).
  if (graph.skeleton_joints != 0 && graph.skeleton_joints != skeleton.joint_count()) return;
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
  f32 w[k_graph_max_blend_inputs];
  // One machine's ops for instance `i`: its states' samples and sums into its output register,
  // and that register's normalize.
  const auto mark = [&](u32 m, u32 i) {
    const ControlMachine& machine = graph.machines[m];
    const GraphInstance& gi = instances[i];
    const AnimationGraphState& st = *gi.state;
    // A state of another graph, or of this one before a replacement, is read as not yet entered:
    // its indices are never used (graph_control resets it on its next tick).
    const bool live = st.graph == graph.id && st.revision == graph.revision && st.state[m] != 0;
    u32 at[2] = {live ? u32{st.state[m]} : u32{machine.entry} + 1u, live ? u32{st.next[m]} : 0u};
    f32 clock[2] = {(live ? st.time[m] : 0.0f) + gi.lead,
                    (live ? st.next_time[m] : 0.0f) + gi.lead};
    f32 weight[2] = {1.0f, 0.0f};
    if (live) state_weights(st, m, gi.lead, gi.interpolate, weight[0], weight[1]);
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
    const usize last = graph.out_op[m];
    code[last * count + i] = n0 >= 2 ? u8{1} : u8{0};
    value[last * count + i] = sum0;
  };
  const GraphLayer* layer = graph.layers.empty() ? nullptr : &graph.layers[0];
  for (u32 i = 0; i < count; ++i) {
    mark(graph.base_machine, i);
    if (layer == nullptr) continue;
    // The layer's weight: the node's, times its parameter's value when it names one, in [0, 1]. At
    // 0 (or NaN) nothing of the layer is evaluated: no sample, no op.
    f32 lw = layer->weight;
    if (layer->param != k_graph_no_param) lw = lw * instances[i].input->values[layer->param];
    lw = clamp01(lw);
    if (!(lw > 0.0f)) continue;
    mark(layer->machine, i);
    code[usize{layer->op} * count + i] = lw >= 1.0f ? u8{1} : u8{2};
    value[usize{layer->op} * count + i] = lw;
  }

  // Op by op: the compacted list of the instances it applies to, and its kernel over that list.
  Batch b;
  b.graph = &graph;
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
    const Kernel second = k_second_kernels[op.kind];
    if (second == nullptr) continue;
    n = 0;
    for (u32 i = 0; i < count; ++i) {
      list[n] = static_cast<u16>(i);
      n += codes[i] == 2 ? 1u : 0u;
    }
    if (n != 0) second(op, b, list, n, values, library, skeleton);
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
