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
  Vector<u32> instance_clip;              // the library clip each instance was attached with
  u32 joints = 0;                         // the widest skeleton attached
  u32 instances = 0;                      // entities attached
  std::string clip_name;                  // what the summary reports

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

  AnimatedScene() : system(library) {}
};

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
// a frame rather than one a tick, because the call waits for the device.
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
  bool bind(AnimatedScene& scene, const renderer::SceneDesc& desc,
            const Vector<DeclaredActor>& declared, u32 tick_hz, std::string& error);
  void hook(AnimationTicks& ticks) noexcept;
  // The state file's `actors`, before the first tick. False with `error` for an entry that does not
  // read; a name that is not bound is skipped and said.
  bool restore(const JsonValue& actors, std::string& error);

  // ---- ActorHost ----
  hotload::Result place(u32 instance, const f64* position, f32 yaw) override;
  hotload::Result play(u32 instance, std::string_view clip, f32 fade_s, bool looping) override;
  hotload::Result stop(u32 instance, f32 fade_s) override;
  hotload::Result speed(u32 instance, f32 speed) override;
  hotload::Result state(u32 instance, hotload::ActorState& out) override;
  JsonValue to_json() const override;
  JsonValue summary_json() const override;
  JsonValue calls_json() const override;

  // Between frames: every actor moved since the last call, in one `move_instances`. True when
  // nothing moved.
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
