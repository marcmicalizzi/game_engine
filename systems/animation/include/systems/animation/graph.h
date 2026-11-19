#pragma once

// Animation graphs as data, stages 1 and 2: roadmap S35, the design
// (docs/experiments/animation-graphs.md, "Stages, in build order") and what is built (animation.md,
// "Graphs").
//
// A graph is document records (systems/animation/schemas/animation_graph.schema): a `Graph`, its
// `GraphParam`s, its `GraphMachine`s, each machine's `GraphState`s and `GraphTransition`s, and each
// state's pose as one `GraphNode`. This header is the three things stage 1 builds over them:
//
//   1. **The input as plain data** (`GraphDesc`): the records with their ids and parents, filled
//      from a document (`read_graph`) or built in code (`locomotion_graph`, the walker's).
//   2. **The compile** (`compile_graph`): a control table per machine — the states, each with a
//      contiguous run of its outgoing transitions sorted by (priority, id), `any` transitions in a
//      run of their own evaluated first, every condition a `ControlCondition` — and per state what
//      a host needs to play it (the clip by name and by library index, its rate, whether it is
//      phase-locked). A state's index is its position in its machine's states sorted by `Id128`, so
//      one graph gives the same indices on every machine and in every process.
//   3. **The control pass** (`graph_control`): per instance, the transitions that hold from the
//   state
//      the machine is in (or heading to), the clocks, the sync groups' phases — written into the
//      instance's `AnimationGraphState` — and, for every machine whose state was entered this tick,
//      a `GraphStep` saying which state and with what fade. A host plays the step into the entity's
//      `AnimationPlayer` (`AnimationSystem::apply_graph_step`, the same collapse-and-fade code
//      `play()` is), or through its own actors as engine-view's walker does, so `sample_poses` runs
//      unchanged and every pose hash of a stage-1 graph is the pose its calls would have given.
//
// Stage 2 (animation.md, "Graphs: the pose program") adds the fourth:
//
//   4. **The pose program** (`GraphOp`, `PoseEvaluator`): a state's pose is a `Clip` or a `Blend1D`
//      of clips by one parameter over thresholds, its clips phase-locked to a sync group (the
//      machine's `phase_param`, or the leader's phase — the input of highest weight sets the
//      cycle, Unreal's rule) or on the state's own clock; a cross-fade is the two states' poses
//      summed by weight. Every blend is the **normalized weighted sum** (ozz-animation's blending
//      job): quaternions aligned to the running sum by a sign and added with their weights,
//      translations and scales added with theirs, the sum normalized once — never a slerp chain.
//      The program is compiled once per graph, ops in a fixed order, and evaluated op-major over a
//      batch of instances, each op's kernel over the compacted list of the instances it applies to.
//      `AnimationSystem` runs it as the scheduled `animation.graph_sample` for graph-driven
//      entities that carry no `AnimationPlayer`, after the scheduled `animation.graph_control`.
//
// Stage 3 (animation.md, "Graphs: layers and masks") adds the fifth:
//
//   5. **Layers**: a graph's second machine is a layer over its first when it has an `Override` or
//      `Additive` child node, with a weight (times a float parameter's value when it names one)
//      and a `GraphMask` compiled per skeleton. The program evaluates the base machine into
//      register 0, the layer machine into its own register, and ends with one layer op over the
//      mask's joints only: an override blends each toward the layer by the weight × the joint's
//      mask weight (the base keeps the rest of the weight, so the two sum to 1), an additive
//      composes the layer's difference from its reference pose (ozz-animation's form: translation
//      added, rotation on the right from identity, scale multiplied). A joint outside the mask is
//      never written; a layer at weight 0 is not evaluated at all.
//
// What the compile refuses by name rather than ignores, with the stage that takes it: the
// `Inertialize` blend (stage 4), the `Machine` kind, `exit_phase`, the `KeepPhase`/`MatchPhase`
// offsets, `skeleton_roles`, `root_motion`, an `Override` or `Additive` node anywhere but as a
// machine's layer node, a weight other than 1 on a node that is not a layer node, a blend of
// blends, and two layers or a layer of a layer (no stage of the design builds them yet; a graph
// has two machines). A graph of two machines neither of which is a layer, or both, compiles its
// control tables but no pose program (`CompiledGraph::program_why` says so), and so does a layered
// graph compiled without a skeleton.
//
// **Stated limits** (ADR-0017): 8 parameters, 2 machines, 2 sync groups (the components' widths),
// 16 inputs a `Blend1D`, 65,534 states a machine and 65,535 transitions, conditions and pose ops a
// graph (the tables' u16 indices). A graph past any of them is refused at compile with the count.
//
// **Determinism**: `hashed`, as the capability is. The control pass is per-instance arithmetic over
// the instance's own two components — compares, adds of `dt`, a floor for the phase — with no
// shared accumulator, so the batch form at any split and the single form give the same bytes. The
// pose program is per-instance arithmetic too, in the compiled op order: `+ - * / sqrt` and the
// sign of a dot product, no C library call (the clip sampler's `slerp` inside a key interval is
// `anim`'s), so it does not depend on the batch, the worker count or the order instances are
// visited in.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>

#include <memory>
#include <schemas/animation_graph.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::doc {
class Document;
}

namespace engine::animation {

class Library;

inline constexpr u32 k_graph_max_params = 8;
inline constexpr u32 k_graph_max_machines = 2;
inline constexpr u32 k_graph_max_sync_groups = 2;
inline constexpr u32 k_graph_max_states = 0xFFFEu;   // a machine's; `state` is an index plus one
inline constexpr u32 k_graph_max_records = 0xFFFFu;  // transitions, conditions: u16 indices
inline constexpr u32 k_graph_max_blend_inputs = 16;  // a Blend1D's clips
inline constexpr u16 k_graph_no_param = 0xFFFFu;

// ---- the compile's input -----------------------------------------------------------------------

// One record of a graph: its id, its parent's id, and its properties as the schema type.
template <class T>
struct GraphItem {
  Id128 id;
  Id128 parent;
  T record;
};

// A graph as its records. The order within each list is not meaning: the compile sorts by id.
struct GraphDesc {
  Id128 id;
  Graph graph;
  Vector<GraphItem<GraphParam>> params;
  Vector<GraphItem<GraphMachine>> machines;
  Vector<GraphItem<GraphState>> states;            // parent: a machine
  Vector<GraphItem<GraphTransition>> transitions;  // parent: a machine
  Vector<GraphItem<GraphNode>> nodes;  // parent: a state (its pose), another node, or a machine
                                       // (its layer node, stage 3)
  Vector<GraphItem<GraphMask>> masks;  // parent: the graph (stage 3)
};

// Reads graph `graph` and every record under it from `document`: its parameters, machines, states,
// transitions and nodes, by the `$type` each record carries. False, with `why` naming the record,
// when the root is not a live `engine.animation.Graph` or a record's properties do not read as its
// type. Records of other types under the graph are skipped (a comment, an editor's annotation).
bool read_graph(const doc::Document& document, const Id128& graph, GraphDesc& out,
                std::string& why);

// The walker's graph (engine-view's `PlayerBody`, apps.md "The procedural gait"): parameters
// `moving` (bool), `fast` (bool) and `phase` (float, [0, 1)); one machine, `gait`, entered at
// `stand`; states `stand` (the idle clip at its own rate 1, or the rest pose when `idle` is empty),
// `walk` and `run` (phase-locked to sync group 1, whose phase is `phase`); and three `any`
// transitions with a cross-fade of `fade_s` — into `walk` when moving and not fast, into `run` when
// moving and fast, into `stand` when not moving. The ids are a hash of the clip names and the fade,
// so the same body builds the same graph and two bodies with different clips two graphs.
struct LocomotionClips {
  std::string_view idle;
  std::string_view walk;
  std::string_view run;
  f32 fade_s = 0.2f;
};
inline constexpr u32 k_locomotion_moving = 0;
inline constexpr u32 k_locomotion_fast = 1;
inline constexpr u32 k_locomotion_phase = 2;
inline constexpr u32 k_locomotion_params = 3;
GraphDesc locomotion_graph(const LocomotionClips& clips);

// ---- the compiled graph ------------------------------------------------------------------------

// One condition: the parameter's value (or its trigger bit) against `value`. 8 bytes.
struct ControlCondition {
  u16 param = 0;
  u8 op = 0;       // GraphCompare
  u8 trigger = 0;  // 1: the condition holds while the parameter's trigger bit is set
  f32 value = 0.0f;
};

// One transition of a machine's table. 16 bytes.
struct ControlTransition {
  u16 to = 0;               // the destination, as its index in the machine
  u16 first_condition = 0;  // into `CompiledGraph::conditions`
  u8 condition_count = 0;
  u8 blend = 0;  // GraphBlend
  u8 any = 0;    // 1: from any state (never into the one the machine is in or heading to)
  u8 _pad = 0;
  f32 duration_s = 0.0f;
  u32 clears = 0;  // the trigger bits its conditions read, cleared when it fires
};

// One state of a machine's table: its run of outgoing transitions. 4 bytes.
struct ControlState {
  u16 first_transition = 0;
  u16 transition_count = 0;
};

// One machine. 16 bytes.
struct ControlMachine {
  u32 first_state = 0;  // into `CompiledGraph::states` and `state_info`
  u16 state_count = 0;
  u16 entry = 0;      // a state index in this machine
  u16 any_first = 0;  // the `any` run, into `transitions`
  u16 any_count = 0;
  u16 phase_param = k_graph_no_param;  // the parameter that is the sync group's phase
  u8 sync_group = 0xFFu;               // the group it drives, 0 or 1; 0xFF: none
  u8 _pad = 0;
};

// One parameter: what a write is clamped to. Cold (read when an input is written).
struct ParamInfo {
  std::string name;
  GraphParamKind kind = GraphParamKind::Float;
  f32 default_value = 0.0f;
  f32 lo = 0.0f;
  f32 hi = 0.0f;
  bool clamped = false;
};

// What a host needs to play a state. Cold (read when a state is entered).
struct StateInfo {
  Id128 id;
  std::string name;
  std::string clip;               // as the library names it; empty: the rest pose
  Id128 clip_id;                  // `Library::clip_id(clip)`; null when `clip` is empty
  u32 clip_index = 0xFFFF'FFFFu;  // in the library the graph was compiled against, if one was given
  f32 rate = 1.0f;
  bool looping = true;
  bool synced = false;  // phase-locked: its time is the sync group's phase, its rate 0
  bool blend = false;   // a Blend1D (stage 2): only the pose program plays it, never a player
};

// ---- the pose program (stage 2) --------------------------------------------------------------

// A state's pose, as the control pass's sync groups and the pose program read it. 8 bytes.
inline constexpr u8 k_pose_state_blend = 1u;    // a Blend1D of `clip_count` clips by `param`
inline constexpr u8 k_pose_state_synced = 2u;   // its clips are phase-locked to the machine's group
inline constexpr u8 k_pose_state_looping = 4u;  // its clips loop
struct PoseState {
  u16 first_clip = 0;            // into `CompiledGraph::pose_clips`
  u8 clip_count = 0;             // 1 for a Clip state
  u8 flags = 0;                  // k_pose_state_*
  u16 param = k_graph_no_param;  // a Blend1D's parameter
  u16 first_op = 0;              // its first op in `CompiledGraph::program` (machine 0's states)
};

// One clip a state's pose samples. 16 bytes.
struct PoseClip {
  u32 clip_index = 0xFFFF'FFFFu;  // in the library the graph was compiled against; unset: rest pose
  f32 rate = 1.0f;                // its own clock's rate, or the cycle's rate when it leads a group
  f32 duration = 0.0f;            // the clip's, read from the library at compile; 0 without one
  f32 threshold = 0.0f;           // a Blend1D input's
};

// What an op computes; the kernel table is indexed by it (plan 11 §11.4: one dispatch an op a
// batch, never a call a node an instance).
enum class PoseOpKind : u8 {
  Sample = 0,      // the rest pose, then the clip at the instance's time, into `dst`
  Accumulate = 1,  // `dst = w * a` for the first contributor, `dst += sign * w * a` after it
  Normalize = 2,   // `dst`'s translation and scale over the weights' sum, its rotations to unit
  Override = 3,    // stage 3: `dst`'s masked joints toward the layer `a` by the weight × the mask
  Additive = 4,    // stage 3: `dst`'s masked joints composed with (`a` − reference) × the weight
};
inline constexpr u32 k_pose_op_kinds = 5;

// One op of a graph's pose program. 16 bytes. Registers: 0 is the instance's own pool slot (the
// program's output and the base machine's pose), 1 a clip's sample, 2 a Blend1D's sum, and with a
// layer (stage 3) the next one the layer machine's pose (`CompiledGraph::layer_register`: 2 when
// no state of the graph is a Blend1D, 3 when one is, so a layered graph has three or four).
inline constexpr u8 k_pose_op_synced = 1u;   // Sample: its time is the group's phase × `k`
inline constexpr u8 k_pose_op_looping = 2u;  // Sample: the clip loops
inline constexpr u8 k_pose_op_state = 4u;    // Accumulate into register 0: weighted by its state
struct GraphOp {
  u8 kind = 0;    // PoseOpKind
  u8 dst = 0;     // the register written
  u8 a = 0;       // Accumulate: the register read
  u8 b = 0;       // k_pose_op_* flags
  u16 node = 0;   // the state (its index in its machine) the op is part of; 0xFFFF: a machine's
                  // output normalize or a layer op
  u16 param = 0;  // Sample and a Blend1D's Accumulate: the clip, as an index into its state's run
  u32 data = 0;   // Sample: the clip's index in the library (0xFFFFFFFF: the rest pose); a layer
                  // op: its index in `CompiledGraph::layers`
  f32 k = 0.0f;   // Sample: the rate on the state's clock, or the clip's duration when phase-locked
};

// A layer (stage 3): the graph's second machine applied over its first by an `Override` or an
// `Additive` node. Its mask is compiled per skeleton into `CompiledGraph::layer_joints` and
// `layer_weights`: `full` joints at weight 1 first, then `partial` joints at their weights in
// (0, 1); a joint in neither is never touched. 24 bytes.
struct GraphLayer {
  u32 first_joint = 0;           // into `layer_joints` and `layer_weights`
  u16 full = 0;                  // joints of the mask at weight 1
  u16 partial = 0;               // joints of the mask at a weight in (0, 1), after them
  u32 reference = 0xFFFF'FFFFu;  // Additive: the first joint of its reference pose, into
                                 // `reference_*`; unset for an Override
  u16 param = k_graph_no_param;  // the float parameter the weight is multiplied by
  u8 kind = 0;                   // PoseOpKind::Override or ::Additive
  u8 machine = 0;                // the layer machine's index
  f32 weight = 1.0f;             // the node's weight, in [0, 1]
  u16 op = 0;                    // the layer op's index in the program
  u16 _pad = 0;
};

class CompiledGraph {
 public:
  Id128 id;
  std::string name;
  Vector<ParamInfo> params;
  Vector<ControlMachine> machines;
  Vector<ControlState> states;   // every machine's, machine by machine
  Vector<StateInfo> state_info;  // parallel to `states`
  Vector<ControlTransition> transitions;
  Vector<ControlCondition> conditions;
  // Stage 2: every state's pose (parallel to `states`), the clips they sample, and the program for
  // machine 0's pose. `program` is empty, and `program_why` says why, for a graph whose pose the
  // program cannot compute yet (two machines: stage 3's layers compose them).
  Vector<PoseState> pose_states;
  Vector<PoseClip> pose_clips;
  Vector<GraphOp> program;
  u32 registers = 0;  // what the program writes: 0..registers-1
  // A hash of the compiled tables (never 0), written into every instance's `AnimationGraphState`:
  // a graph replaced under the same id (`GraphLibrary::add`) has another, so an instance of the
  // old one starts again at the entries before any of its state indices is read. A hash rather
  // than a counter, so a save loaded by a process that compiles the same graph keeps its states.
  u32 revision = 0;
  std::string program_why;
  // Per machine, in machine order: the program's normalize of the machine's output register (the
  // base's is register 0, a layer machine's `layer_register`); unset for a graph with no program.
  u16 out_op[k_graph_max_machines] = {0xFFFFu, 0xFFFFu};
  u8 out_register[k_graph_max_machines] = {0, 0};
  u8 base_machine = 0;    // the machine that is not a layer
  u8 layer_register = 0;  // 0: no layer
  // Stage 3: the layers (one at most: a graph has two machines), their masks' joints and weights
  // and the additive layers' reference poses, all compiled for one skeleton of `skeleton_joints`
  // joints (0: a graph with no layer, which any skeleton plays).
  Vector<GraphLayer> layers;
  Vector<u16> layer_joints;
  Vector<f32> layer_weights;
  Vector<Vec3> reference_translation;
  Vector<Quat> reference_rotation;
  Vector<Vec3> reference_scale;
  u32 skeleton_joints = 0;
  // The skeleton a host compiled a layered graph for (`AnimationSystem::add_graph`).
  Id128 skeleton;

  u32 param_index(std::string_view param_name) const noexcept;  // 0xFFFFFFFF when absent
  // A state of machine `machine` by name, as its index in the machine; 0xFFFFFFFF when absent.
  u32 state_index(u32 machine, std::string_view state_name) const noexcept;
  const StateInfo& state(u32 machine, u32 index) const noexcept {
    return state_info[machines[machine].first_state + index];
  }
  // What the compiled graph holds, in bytes: the object and every table and string it owns.
  usize bytes() const noexcept;
};

// Compiles `desc`. With `library`, every clip a state names must be in it (refused by the graph's,
// the state's and the clip's names otherwise); without one, the names are taken as given and
// `StateInfo::clip_index` stays unset (the program then samples the rest pose, and a led group,
// knowing no duration, does not advance). False, with `why`, for everything refused (above). The
// pose program is compiled with the control tables when the graph has one machine.
//
// Stage 3: a graph of two machines is a base and a layer — the machine with an `Override` or
// `Additive` child node is the layer, the other the base — and its masks and an additive layer's
// reference pose are compiled for `skeleton` (a graph's masks name joints by role, which only a
// skeleton resolves). A layered graph compiled without a skeleton (or without a library) compiles
// its control tables and no program, and `program_why` says so.
bool compile_graph(const GraphDesc& desc, const Library* library, CompiledGraph& out,
                   std::string& why, const anim::Skeleton* skeleton = nullptr);

// ---- the control pass --------------------------------------------------------------------------

// What a machine did this tick, when it entered a state. 12 bytes.
struct GraphStep {
  u16 entered = 0;    // the state entered, as its index in the machine plus one; 0: nothing entered
  u8 blend = 0;       // GraphBlend: Cut on the first tick and for a zero duration
  u8 first = 0;       // 1: the machine's first tick (it entered with a cut)
  f32 fade_s = 0.0f;  // 0 for a cut
  u32 _pad = 0;
};

// A step as a host plays it: the state entered (its clip, rate, looping and whether it is
// phase-locked) and the fade. `state` points into the compiled graph and is valid while the graph
// is in its library and not replaced.
struct GraphEnter {
  const StateInfo* state = nullptr;
  f32 fade_s = 0.0f;
  u8 blend = 0;
  bool first = false;
};
inline GraphEnter graph_enter(const CompiledGraph& graph, u32 machine,
                              const GraphStep& step) noexcept {
  GraphEnter out;
  if (step.entered == 0) return out;
  out.state = &graph.state(machine, step.entered - 1u);
  out.fade_s = step.fade_s;
  out.blend = step.blend;
  out.first = step.first != 0;
  return out;
}

// Writes `values` (by parameter index, `count` of them, each clamped into its range; a bool to 0 or
// 1) and sets `triggers`' bits into `input`, for the graph `graph`. A host's one write per tick.
void set_graph_inputs(const CompiledGraph& graph, AnimationGraphInput& input,
                      std::span<const f32> values, u32 triggers) noexcept;

// The inputs and the state an instance starts with: the graph's id, the parameters' defaults, no
// machine entered (the first control tick enters with a cut).
void init_graph_instance(const CompiledGraph& graph, AnimationGraphInput& input,
                         AnimationGraphState& state) noexcept;

// One instance's control tick of `dt` seconds. `steps` holds one step a machine
// (`graph.machines.size()` of them); returns a bit a machine that entered a state this tick. A
// state whose `graph` is another graph's (a swap), or whose `revision` is not this compile's (a
// graph replaced in place: a reload), starts again at the entries before any index is read.
u32 graph_control(const CompiledGraph& graph, AnimationGraphInput& input,
                  AnimationGraphState& state, f32 dt, GraphStep* steps) noexcept;

// The same over `inputs.size()` instances of one graph, in order: `steps` is instance-major,
// `graph.machines.size()` a instance. Returns how many instances entered a state on any machine.
u32 graph_control(const CompiledGraph& graph, std::span<AnimationGraphInput> inputs,
                  std::span<AnimationGraphState> states, f32 dt,
                  std::span<GraphStep> steps) noexcept;

// A machine's sync group with no `phase_param` is led (stage 2): each control tick its phase
// advances by `dt * rate / duration` of the clip of highest weight among the group's clips in the
// state the machine is in and the one it is heading to (Unreal's sync groups: the leader sets the
// cycle; a tie goes to the first in the program's order). A group none of whose clips plays this
// tick keeps its phase.
//
// An instance promoted out of LOD3 (no control pass ran for `gap` seconds): the clocks, a
// transition in flight (finished if its duration passed) and a led group's phase advanced over the
// gap in closed form, as `apply_tier` advances a player; no transition is evaluated (the next
// control tick does that on the inputs then). The design's full LOD3 rule is stage 5's.
void graph_catch_up(const CompiledGraph& graph, const AnimationGraphInput& input,
                    AnimationGraphState& state, f32 gap) noexcept;

// ---- the pose program's evaluation (stage 2)
// -----------------------------------------------------

// One instance as the pose program reads it: its inputs and state after this tick's control pass,
// the tick's step (`lead`: the control pass's clocks are the tick's start, the pose shows its end,
// as a player advanced before it is sampled does), whether a cross-fade is interpolated (LOD0) or
// cut at its midpoint (LOD1 and LOD2, as `LodPlan::interpolate` says), and where its pose goes.
struct GraphInstance {
  const AnimationGraphInput* input = nullptr;
  const AnimationGraphState* state = nullptr;
  f32 lead = 0.0f;
  bool interpolate = true;
  anim::PoseView out;
};

// The batched, op-major evaluator. One per thread (it holds the batch's registers and lists, grown
// to the largest batch × registers × joints it has seen and never shrunk, so a steady tick
// allocates nothing). `evaluate` runs `graph`'s program over `instances`, all of `skeleton`,
// `batch` instances at a time: per batch, each instance's op weights and times from its state (no
// per-op state is kept between ticks), then op by op the compacted list of the instances it applies
// to and the op's kernel over that list. An instance's pose depends on its own input and state
// alone.
class PoseEvaluator {
 public:
  void evaluate(const CompiledGraph& graph, const Library& library, const anim::Skeleton& skeleton,
                std::span<const GraphInstance> instances, u32 batch);
  // The scratch it holds, in bytes.
  usize bytes() const noexcept;

 private:
  void run_batch(const CompiledGraph& graph, const Library& library, const anim::Skeleton& skeleton,
                 std::span<const GraphInstance> instances);

  Vector<Vec3> translation_;  // registers 1.. of the batch, register-major, joint-minor
  Vector<Quat> rotation_;
  Vector<Vec3> scale_;
  Vector<u8> code_;    // per op per instance: 0 skip, 1 set or apply, 2 add
  Vector<f32> value_;  // per op per instance: the weight (the sum, for a normalize) or the time
  Vector<u16> list_;   // the compacted list of one op
};

// ---- the library of compiled graphs ------------------------------------------------------------

// The compiled graphs a world plays, by the `Graph` record's id. Empty — no heap, nothing resident
// — for a world with no graph.
class GraphLibrary {
 public:
  // Adds `graph`, replacing one with the same id in place (its index, and every pointer
  // `find` returned for it, stay valid; its instances carry the old `revision` and start again at
  // the entries on their next control tick). Returns its index.
  u32 add(CompiledGraph&& graph);
  const CompiledGraph* find(const Id128& id) const noexcept;
  u32 size() const noexcept { return graphs_.size(); }
  const CompiledGraph& at(u32 index) const noexcept { return *graphs_[index]; }
  usize bytes() const noexcept;

 private:
  // Each graph its own allocation, so a pointer `find` returned survives the next `add`.
  Vector<std::unique_ptr<CompiledGraph>> graphs_;
};

}  // namespace engine::animation
