#pragma once

// Animation graphs as data, stage 1: roadmap S35, the design
// (docs/experiments/animation-graphs.md, "Stage 1") and what is built (animation.md, "Graphs").
//
// A graph is document records (systems/animation/schemas/animation_graph.schema): a `Graph`, its
// `GraphParam`s, its `GraphMachine`s, each machine's `GraphState`s and `GraphTransition`s, and each
// state's pose as one `GraphNode`. This header is the three things stage 1 builds over them:
//
//   1. **The input as plain data** (`GraphDesc`): the records with their ids and parents, filled
//      from a document (`read_graph`) or built in code (`locomotion_graph`, the walker's).
//   2. **The compile** (`compile_graph`): a control table per machine — the states, each with a
//      contiguous run of its outgoing transitions sorted by (priority, id), `any` transitions in a
//      run of their own evaluated first, every condition a `ControlCondition` — and per state what a
//      host needs to play it (the clip by name and by library index, its rate, whether it is
//      phase-locked). A state's index is its position in its machine's states sorted by `Id128`, so
//      one graph gives the same indices on every machine and in every process.
//   3. **The control pass** (`graph_control`): per instance, the transitions that hold from the state
//      the machine is in (or heading to), the clocks, the sync groups' phases — written into the
//      instance's `AnimationGraphState` — and, for every machine whose state was entered this tick,
//      a `GraphStep` saying which state and with what fade. A host plays the step into the entity's
//      `AnimationPlayer` (`AnimationSystem::apply_graph_step`, the same collapse-and-fade code
//      `play()` is), or through its own actors as engine-view's walker does, so `sample_poses` runs
//      unchanged and every pose hash of a stage-1 graph is the pose its calls would have given.
//
// What stage 1 does not compile, and refuses by name rather than ignores: node kinds other than
// `Clip`, a state with more than one pose node, `exit_phase`, the `Inertialize` blend, the
// `KeepPhase`/`MatchPhase` offsets, `skeleton_roles` and `root_motion` (stages 2 to 4).
//
// **Stated limits** (ADR-0017): 8 parameters, 2 machines, 2 sync groups (the components' widths),
// 65,534 states a machine and 65,535 transitions and conditions a graph (the tables' u16 indices).
// A graph past any of them is refused at compile with the count.
//
// **Determinism**: `hashed`, as the capability is. The control pass is per-instance arithmetic over
// the instance's own two components — compares, adds of `dt`, a floor for the phase — with no shared
// accumulator, so the batch form at any split and the single form give the same bytes.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>

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
inline constexpr u32 k_graph_max_states = 0xFFFEu;      // a machine's; `state` is an index plus one
inline constexpr u32 k_graph_max_records = 0xFFFFu;     // transitions, conditions: u16 indices
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
  Vector<GraphItem<GraphNode>> nodes;              // parent: a state (its pose) or another node
};

// Reads graph `graph` and every record under it from `document`: its parameters, machines, states,
// transitions and nodes, by the `$type` each record carries. False, with `why` naming the record,
// when the root is not a live `engine.animation.Graph` or a record's properties do not read as its
// type. Records of other types under the graph are skipped (a comment, an editor's annotation).
bool read_graph(const doc::Document& document, const Id128& graph, GraphDesc& out, std::string& why);

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
  u8 blend = 0;             // GraphBlend
  u8 any = 0;               // 1: from any state (never into the one the machine is in or heading to)
  u8 _pad = 0;
  f32 duration_s = 0.0f;
  u32 clears = 0;           // the trigger bits its conditions read, cleared when it fires
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
  u16 entry = 0;        // a state index in this machine
  u16 any_first = 0;    // the `any` run, into `transitions`
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
  std::string clip;  // as the library names it; empty: the rest pose
  Id128 clip_id;     // `Library::clip_id(clip)`; null when `clip` is empty
  u32 clip_index = 0xFFFF'FFFFu;  // in the library the graph was compiled against, if one was given
  f32 rate = 1.0f;
  bool looping = true;
  bool synced = false;  // phase-locked: its time is the sync group's phase, its rate 0
};

class CompiledGraph {
 public:
  Id128 id;
  std::string name;
  Vector<ParamInfo> params;
  Vector<ControlMachine> machines;
  Vector<ControlState> states;           // every machine's, machine by machine
  Vector<StateInfo> state_info;          // parallel to `states`
  Vector<ControlTransition> transitions;
  Vector<ControlCondition> conditions;

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
// `StateInfo::clip_index` stays unset. False, with `why`, for everything stage 1 refuses (above).
bool compile_graph(const GraphDesc& desc, const Library* library, CompiledGraph& out,
                   std::string& why);

// ---- the control pass --------------------------------------------------------------------------

// What a machine did this tick, when it entered a state. 12 bytes.
struct GraphStep {
  u16 entered = 0;   // the state entered, as its index in the machine plus one; 0: nothing entered
  u8 blend = 0;      // GraphBlend: Cut on the first tick and for a zero duration
  u8 first = 0;      // 1: the machine's first tick (it entered with a cut)
  f32 fade_s = 0.0f; // 0 for a cut
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
inline GraphEnter graph_enter(const CompiledGraph& graph, u32 machine, const GraphStep& step) noexcept {
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
// state whose `graph` is another graph's (a reload, a swap) starts again at the entries.
u32 graph_control(const CompiledGraph& graph, AnimationGraphInput& input, AnimationGraphState& state,
                  f32 dt, GraphStep* steps) noexcept;

// The same over `inputs.size()` instances of one graph, in order: `steps` is instance-major,
// `graph.machines.size()` a instance. Returns how many instances entered a state on any machine.
u32 graph_control(const CompiledGraph& graph, std::span<AnimationGraphInput> inputs,
                  std::span<AnimationGraphState> states, f32 dt, std::span<GraphStep> steps) noexcept;

// ---- the library of compiled graphs ------------------------------------------------------------

// The compiled graphs a world plays, by the `Graph` record's id. Empty — no heap, nothing resident —
// for a world with no graph.
class GraphLibrary {
 public:
  // Adds `graph`, replacing one with the same id in place (its index, and every pointer
  // `find` returned for it, stay valid). Returns its index.
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
