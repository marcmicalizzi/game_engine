#pragma once

// **The animated world** (docs/subsystems/animation.md, "The app-side glue"; docs/subsystems/
// apps.md, "--animate", "Animation in a session").
//
// The whole of `--animate` and of a scene file's per-instance `animation` blocks, and it is in the
// app rather than in `systems/renderer` on purpose: the renderer must not depend on the animation
// capability, on `domain/ecs`, or on flecs, so what crosses the boundary is one span of
// `anim::JointMatrix` and one `renderer::InstanceJoints` per instance and nothing else. A game
// writes exactly this — a world, a library, one entity per character, a fixed-step tick on the
// simulation's clock, and a per-frame read of the runs — which is why it is spelled out here
// instead of folded into a frame loop, and why it is a file of its own: the windowed loop, the
// offscreen one and the reference renderer call the same functions, so there is one answer to
// "which instances animate" and one to "how many steps has the world taken".
//
// **Which meshes are clip sources** (`load_clips`): a mesh an instance's `animation` block names
// must be a skinned glTF, and anything else — a `.clusters` container, a mesh a generator made
// (a `.flora.json` palm, a `.rock.json` rock, the water's pool, the terrain), a glTF with no skin —
// is refused with a sentence naming the instance and the mesh. `--animate` (or a `--mesh` run)
// adds every other mesh as a *candidate*: a skinned glTF among them animates, and anything else is
// passed over quietly, so a mixed scene animates its characters and draws its palms, its shoreline
// and its water rigid. A generated mesh is never even opened as a clip source.
//
// **The clock** (`AnimationTicks`, `advance_animation`): the world steps at 60 Hz, a whole step
// when the clock driving it says one is due — never one a rendered frame. In a session
// (`--interactive`, `--replay-input`, `--inject-input`, a `--game`) the clock is the session's
// tick: step k is due at the tick `k * tick_hz / 60`, run from the session's tick observer with the
// tiers decided from that tick's camera, so a live window, an injected window and an offscreen
// replay of the same recording step the same world at the same ticks and end on the same poses,
// whatever their frame rates. Without a session the clock is the frame index, one step a frame,
// as `--frames N` always was. Frame-rate independence is the tick's: a frame draws whatever the
// ticks before it left.
//
// Without ENGINE_VIEW_ANIMATION (a minimal build, `ENGINE_WITH_ANIMATION=OFF`,
// `ENGINE_WITH_ECS=OFF`) none of this compiles and `--animate` is refused with a sentence.

#if ENGINE_VIEW_ANIMATION

#include "fly_camera.h"
#if ENGINE_VIEW_GAME
#include "game_props.h"
#endif

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <core/math/world.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/tiers.h>
#include <foundation/input/input.h>
#include <systems/animation/animation.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>

#include <memory>
#include <schemas/scene.h>
#include <string>
#include <string_view>

namespace engine::view {

// The rate the animated world steps at: the frame index's sixty a second, which is what the
// deformation phase and the light orbit already run at, so `--frames N` advances the world exactly
// N steps and a session's tick at `tick_hz` maps onto it by `tick * 60 / tick_hz`.
inline constexpr u32 k_animation_hz = 60;

// What the command line asks of the animated world (main.cpp's `Options`, the five flags).
struct AnimationOptions {
  bool animate = false;  // --animate: every skinned instance plays
  std::string clip;      // --animate <clip>: the clip an instance with no block of its own plays
  f32 speed = 1.0f;      // --anim-speed
  bool lod = true;       // --anim-lod
  f32 lod_scale = 1.0f;  // --anim-lod-scale
};

struct AnimatedScene {
  ecs::SimWorld sim;  // 60 Hz (`k_animation_hz`)
  animation::Library library;
  animation::AnimationSystem system;
  Vector<Id128> entities;                 // one per scene instance; null where it is rigid
  Vector<renderer::InstanceJoints> runs;  // refilled every step; what the renderer reads
  Vector<u32> mesh_skeleton;              // the skeleton each scene mesh's file contributed
  // Each scene mesh's run of the scene's morph channels and the channels' names (the merged mesh's
  // `morph_names`), taken at `attach_instances`: what a game's actor is told it can shape (ABI 9).
  Vector<u32> mesh_first_channel;
  Vector<u32> mesh_channel_count;
  Vector<std::string> morph_names;
  // The library clip each instance was attached with; `Library::k_not_found` for a skin that has
  // none, which stands at its bind pose (P61).
  Vector<u32> instance_clip;
  Vector<u32> instance_skeleton;  // the library skeleton each instance was attached with
  u32 joints = 0;                 // the widest skeleton attached
  u32 instances = 0;              // entities attached
  std::string clip_name;          // what the summary reports
  // **The whole read's index of each scene instance, and back** (renderer.md, "The whole read's
  // index"): everything above is indexed by the loaded instance, and everything a scene file or a
  // game names — an animation block, an actor's handle, the summary's `instance` — by the whole
  // read's, which a streamed world's load does not keep (it hands some instances to the tail).
  // Taken from `renderer::read_instance` and `SceneData::loaded_of_read` at `attach_instances`;
  // for a scene read whole both are the identity and `loaded_of_read` is empty.
  Vector<u32> read_instance;   // per scene instance
  Vector<u32> loaded_of_read;  // per instance of the whole read; `renderer::k_not_loaded`: the tail
  u32 read_count = 0;          // the whole read's instances

  // ---- the clock ----------------------------------------------------------------------------
  u64 steps = 0;               // world steps taken
  const char* clock = "none";  // "ticks" (a session's), "frames" (the frame index), "none"
  u32 clock_hz = 0;            // the clock's rate: the session's tick_hz, or 60
  u64 clock_ticks = 0;         // the last value of that clock the world was brought up to

  // ---- the camera-driven LOD (anim_lod.h) -----------------------------------------------------
  //
  // The SoA `sim::TierAssignment` works on, over the **animated** instances only: a rigid one has
  // no pose to coarsen and would only cost a score. `row_instance` maps a row back to its scene
  // instance, which is the one place the tier code's index space and the entity ids meet — the
  // same seam `sim::apply_tier_changes` has, written here because an app cannot make a
  // `sim::EntityHandle` without flecs and `set_tier(Id128, u8)` is the surface that exists for it.
  bool lod = true;
  f32 lod_scale = 1.0f;
  sim::TierParams tier_params;
  sim::ObserverSet observers;
  sim::TierAssignment tiers;
  Vector<u32> row_instance;    // scene instance of each row below
  Vector<WorldPos> positions;  // world centre of the instance's padded bounding sphere, f64
  Vector<f32> radii;           // its radius, padding and instance scale included
  Vector<f32> importance;      // what the camera says the row is worth, refilled every step
  Vector<u8> tier_state;       // in/out of assign_tiers; the tier each row is at
  Vector<u8> instance_tier;    // the same, indexed by scene instance, for `step_animation`
  Vector<sim::TierChange> changes;
  u32 histogram[animation::k_tier_count] = {};
  u32 promotions = 0;
  u32 demotions = 0;
  u32 deferred = 0;
  f64 tick_ns = 0.0;  // summed over the run, for the summary
  f64 lod_ns = 0.0;

  // ---- the joint collapse (player.h; apps.md, "The player's body") -----------------------------
  //
  // **A joint collapse mask per animated slot**: the joints whose skinning matrix the renderer is
  // handed as the head joint's position with a zero linear part, so every vertex skinned to them
  // lands on that one point, inside the neck — the first-person player's head. It is applied
  // **after** the capability's `build_skinning_matrices`, to a copy of the pool's matrices
  // (`collapsed`, refreshed at the end of every step), and never to the pool itself: the pose and
  // the matrices the summary's `pose_hash` is taken over stay the animation's, so the hashes are
  // the same with the head collapsed or not, and a third-person toggle leaves them alone. The copy
  // is the whole span (one contiguous span is the renderer's contract): 48 bytes a joint of the
  // pool a step, sized with the pool and grown only when the pool grows. Empty, or `collapse_on`
  // false, and the renderer reads the pool's own span as before. Why here and not in
  // `systems/animation`: the span the renderer reads is built here (`set_frame_joints`), and the
  // capability's matrices are reachable read-only, which is all this needs.
  //
  // **The head-look layer** (P58, apps.md "The head-look layer") rides the same copy: before the
  // collapse, each `LookJoint` — outermost first, the torso, the neck, the head — turns its joint
  // and every joint below it about the joint's posed position, `skin' = T(p) R T(-p) skin`, by
  // its share of the look (`down` about the body's +x, then `turn` about its +y; the rig faces
  // +z). The pose and the pool stay the animation's, so a replay draws the same body and the
  // hashes hold. `head_posed` keeps the head's matrix after the look and before the collapse: the
  // first-person eye is carried by it (player.h `PlayerBody::face_camera`).
  struct LookJoint {
    u32 joint = 0;       // the skeleton's index
    Vec3 bind{};         // the joint in the mesh's bind space, as `head_bind` below
    f32 down = 0.0f;     // radians about the body's +x: positive turns +z (ahead) toward -y (down)
    f32 turn = 0.0f;     // radians about its +y
    Vector<u32> joints;  // the joint and every joint below it
  };
  struct JointCollapse {
    u32 instance = 0;    // the loaded instance whose run it collapses
    u32 head = 0;        // the joint the others collapse to, the skeleton's index
    Vec3 head_bind{};    // the head joint in the mesh's bind space: inverse(inverse_bind[head])'s
                         // translation, which its skinning matrix takes to where it stands
    Vector<u32> joints;  // the head and every joint below it, the skeleton's indices
    bool collapse_head = true;     // false: the joints are drawn whole (the look alone)
    Vector<LookJoint> look;        // outermost first; empty: no look layer
    anim::JointMatrix head_posed;  // the head's skinning matrix after the look, before the collapse
    bool head_posed_valid = false;  // false: a run of nothing (the rest pose is drawn)
  };
  Vector<JointCollapse> collapse;
  bool collapse_on = true;  // the copy is handed to the renderer (a collapse or a look is on)
  Vector<anim::JointMatrix> collapsed;  // the pool's span with the collapse, as of the last step
  bool collapse_ready = false;          // `collapsed` matches the pool's span
  u64 collapse_steps = 0;               // steps whose matrices were collapsed, for the summary

  AnimatedScene() : system(library) {}
};

// The collapse of one animated instance's `joints` onto its skeleton's `head` (above), applied at
// once to the matrices the last step left and after every step from now on. `on` false keeps the
// mask and hands the renderer the pool's matrices (third person).
void set_joint_collapse(AnimatedScene& scene, u32 instance, const anim::Skeleton& skeleton,
                        u32 head, std::span<const u32> joints);
void set_joint_collapse_on(AnimatedScene& scene, bool on);
// Refreshes `collapsed` from the pool's matrices; `step_animation` calls it after every step.
void apply_joint_collapse(AnimatedScene& scene);
// The head-look layer on collapse entry `entry` (the order `set_joint_collapse` added them):
// `collapse_head` false draws the head whole; `pivots`, outermost first, are the joints the look
// turns (each with the joints below it). Applied at once, as the collapse is.
void set_joint_look(AnimatedScene& scene, u32 entry, const anim::Skeleton& skeleton,
                    bool collapse_head, std::span<const u32> pivots);
// Whether collapse entry `entry` collapses its head and the joints below it: the rule
// `set_joint_look` set, changed between frames (third person draws the head whole whatever the rule
// was, P53). The run is written again at once when the rule changed; the same rule is a no-op.
void set_joint_collapse_head(AnimatedScene& scene, u32 entry, bool collapse_head);
// Each pivot's share of the look, radians, `down` and `turn` as `LookJoint`'s, the same count as
// `pivots`; refreshes that entry's run of `collapsed` from the pool alone (the run's joints, not
// the whole span), so a frame may set the look it draws with between two steps.
void set_joint_look_angles(AnimatedScene& scene, u32 entry, std::span<const f32> down,
                           std::span<const f32> turn);

// A stable, reproducible id per scene instance (ADR-0028 seam 3), from the instance index rather
// than a counter, so a capture is deterministic across runs.
Id128 instance_id(u32 instance);

// The clip `request` names among the clips of `skeleton`: its index among that skeleton's clips,
// its full library name, or its name without the library's prefix; an empty request takes the
// skeleton's first clip. `k_not_found` when nothing matches.
u32 find_clip(const animation::Library& library, u32 skeleton, std::string_view request);

// `--morph-animate`'s clips when nothing is skinned: the weight curves of every glTF the scene
// names that a generator did not make, in a library of their own. A file with neither a skin nor a
// weight curve is passed over.
void load_morph_clips(animation::Library& library, const renderer::SceneDesc& desc);
// The clip `--morph-animate` plays among the clips that have weight tracks, named as `find_clip`.
u32 find_morph_clip(const animation::Library& library, std::string_view request);

// Part one: does this run animate, and which instances play a clip? Before the scene loads,
// because `SceneInstance::joints` is what makes an instance skinned and the GPU scene's deform
// table is laid out from it. `out` stays null when nothing animates. False, with a sentence, for a
// block on a mesh that cannot play one, or for `--animate` on a scene with no skinned glTF.
bool prepare_animation(std::unique_ptr<AnimatedScene>& out, renderer::SceneDesc& desc,
                       const AnimationOptions& options, std::string& error);

// Part two: one entity per skinned instance, its clip and its phase, the padding its bounds need,
// and the tier rows. After the scene has loaded (the renderer's joint buffer is sized by the pose
// pool, which only knows its size once the population is attached).
bool attach_instances(AnimatedScene& scene, const renderer::SceneDesc& desc,
                      renderer::SceneData& data, const AnimationOptions& options,
                      std::string& error);

// What the camera says each character is worth, before a step: the observer set from the views,
// the per-view importance, `sim::TierAssignment` over the rows, the changes handed to the
// capability.
void update_animation_lod(AnimatedScene& scene, const renderer::ViewSet& views,
                          const renderer::Camera& camera);

// One fixed step of the world, then where each instance's matrices are. A coarsened instance
// (LOD2, LOD3) hands a zero-length run — the rest pose — deliberately (animation.cpp says why).
void step_animation(AnimatedScene& scene);

// Brings the world up to `clock_ticks` of a clock of `clock_hz` (`clock` names it for the
// summary): every step due by then, each after a tier decision from `camera` through `renderer`'s
// views. Steps already taken are never taken again and a clock that went backwards steps nothing,
// so a frame drawn twice from the same clock draws the same pose. Returns the steps it ran.
u32 advance_animation(AnimatedScene& scene, renderer::SceneRenderer& renderer,
                      const renderer::Camera& camera, const char* clock, u32 clock_hz,
                      u64 clock_ticks);

// **The session's clock**: a tick observer (`FlySession::set_tick_observer`) that chains the one
// already set — the game's, which runs first, so a game that will one day change a clip at a tick
// changes it before that tick's step — and then brings the world up to the session's tick, the
// tiers decided from that tick's camera. `attach` installs it; the object must outlive the session.
//
// **Two hooks round the step** (`before`, `after`, with `hooks_user`): a game's actors
// (`GameActors`) advance their fades after the game's commands and before the world steps, and
// sample their paths once it has. Null, nothing runs.
struct AnimationTicks {
  AnimatedScene* scene = nullptr;
  renderer::SceneRenderer* views_of = nullptr;  // whose views a tier is decided from
  u32 tick_hz = 0;
  FlySession::TickObserver next = nullptr;
  void* next_user = nullptr;
  using StepHook = void (*)(void* user, u64 tick);
  StepHook before = nullptr;
  StepHook after = nullptr;
  void* hooks_user = nullptr;

  void attach(FlySession& session, AnimatedScene& animated, renderer::SceneRenderer& view_renderer,
              u32 session_tick_hz) noexcept;
  static void observe(void* user, const FlySession& session, const input::InputState& input);
};

#if ENGINE_VIEW_GAME
// **The actors a game moves** (module_abi.h ABI 5; docs/subsystems/apps.md, "Actors: the
// characters a game moves"; roadmap S88): the scene's named animated instances as a `--game`
// module drives them, through the runner (`GameRunner::set_actors`), by scene instance.
//
// **A command is applied at the tick the module made it.** The module runs in the game's tick
// observer, which `AnimationTicks` calls first, so a play, a stop or a rate goes into the animated
// world before that tick's step — the same tick in a live window, an injected one and an offscreen
// replay — and a fade is advanced a tick at a time in `before_step`. A move changes the actor's
// placement and its tier row at the tick too; the renderer's instance table takes every actor that
// moved since the last frame in **one** `SceneRenderer::move_instances` before the next frame
// (`flush`), as a prop reaches the picture on the frame after the tick that changed it — one call
// a frame rather than one a tick, since only where an actor stands when the frame is drawn can be
// seen. The call waits for nothing (since 2026-10-22 not in a scene read whole either): the moves
// ride the next frame's record upload (renderer.md, "Moving an instance").
//
// **What is state and what is replayed.** `to_json` is every actor's placement and its player
// (`AnimationPlayer` as its schema writes it) plus a fade in flight, the state file's `actors`, and
// `restore` puts them back before the first tick: a cold restore stands every actor where the
// last run left it, in the clip and at the playhead it had. The module's commands are not
// recorded — a replay re-runs the module from what the recording restored, and its calls recur at
// the same ticks.
//
// **A fade to the rest pose is the player's weight** (`AnimationPlayer::weight`, which the sampler
// already blends toward the bind pose): written here a tick at a time through
// `ecs::WorldCommands::set_bytes`, the component's own bytes, since the capability's surface has a
// cross-fade between clips and no stop. A cut is the clip cleared; a play from the rest pose with a
// fade is a cut to the clip at weight 0 that rises.
class GameActors final : public ActorHost {
 public:
  GameActors();
  ~GameActors() override;
  GameActors(const GameActors&) = delete;
  GameActors& operator=(const GameActors&) = delete;

  // After `attach_instances`: one actor per declared name whose instance animates. A declared actor
  // whose instance did not animate (a rigid mesh under `--animate`) is left out and said once.
  // `tick_hz` paces the path's samples. Then the hooks onto `ticks`. Every scene moves its actors,
  // a dynamic one (props, a world) included (`SceneRenderer::move_instances`).
  //
  // **An actor's handle is the whole read's index** (`DeclaredActor::instance`), and `desc` is the
  // whole read; the animated world and the renderer are indexed by the loaded instance, which the
  // bind finds once through `loaded_actor` (game_props.h). A declared actor whose instance a
  // streamed world handed to its tail is refused, false with a sentence naming the actor and its
  // instance: `keep_actors` (game_props.h) is what keeps one in the prefix, and a host that did not
  // call it has a game that would hear `NotFound` for an actor its scene names.
  bool bind(AnimatedScene& scene, const renderer::SceneDesc& desc,
            const Vector<DeclaredActor>& declared, u32 tick_hz, std::string& error);
  void hook(AnimationTicks& ticks) noexcept;
  // The state file's `actors`, before the first tick. False with `error` for an entry that does not
  // read; a name that is not bound is skipped and said.
  bool restore(const JsonValue& actors, std::string& error);

  // ---- ActorHost ----
  hotload::Result place(u32 instance, const f64* position, f32 yaw) override;
  hotload::Result place_aligned(u32 instance, const f64* position, f32 yaw, const f32* up,
                                f32 max_tilt_rad) override;
  hotload::Result play(u32 instance, std::string_view clip, f32 fade_s, bool looping) override;
  hotload::Result stop(u32 instance, f32 fade_s) override;
  hotload::Result speed(u32 instance, f32 speed) override;
  hotload::Result state(u32 instance, hotload::ActorState& out) override;
  // ABI 9 (apps.md, "Actors"): the shape is applied at the tick — `state` reports it at once — and
  // reaches the renderer at `flush`, the weights through `set_instance_morph_weights` and the scale
  // in the move's transform. **The shape is the game's state, not the host's**: `to_json` does not
  // write it and a cold restore stands the actor at the scene's shape until the module says it
  // again.
  hotload::Result morph(u32 instance, const f32* weights, u32 count) override;
  hotload::Result scale(u32 instance, f32 scale) override;
  hotload::Result morph_info(u32 instance, hotload::MorphInfo& io) override;
  JsonValue to_json() const override;
  JsonValue summary_json() const override;
  JsonValue calls_json() const override;

  // Between frames: every actor moved since the last call, in one `move_instances`, and every actor
  // whose weights changed, one `set_instance_morph_weights` each. True when nothing moved.
  bool flush(renderer::SceneRenderer& renderer, std::string* error);
  u32 count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
#endif

// The frame's half of the contract: the span of every animated instance's matrices and the run
// per instance, as the world stands.
void set_frame_joints(const AnimatedScene& scene, renderer::FrameDesc& frame) noexcept;

// The summary's animation block (`engine.scene.FlythroughAnimation`): what the camera decided,
// what the tick cost per frame of `frames`, the clock, and **per animated instance the hashes of
// its clip, its playhead and its drawn pose** at the end of the run, which a game's test pins.
scene::FlythroughAnimation animation_summary(const AnimatedScene& scene, u64 frames);
// The same as JSON, for the windowed summary line's `anim`.
JsonValue anim_summary(const AnimatedScene& scene, u64 frames);

}  // namespace engine::view

#endif  // ENGINE_VIEW_ANIMATION
