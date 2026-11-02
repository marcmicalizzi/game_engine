#pragma once

// **The player's body** (docs/subsystems/apps.md, "The player's body"; roadmap P52, stage A).
//
// `engine-view --player <rig.glb>` stands a skinned character at the walker's feet: a load-time
// actor named `player`, appended to the scene's whole read before the renderer loads it (so the
// animation world binds it as it binds any named instance and the deformed-vertex pool, the cull
// and the ray tracing chain lay it out with the scene), placed every tick by the session through
// the actors' own path (`GameActors::place`, the once-a-frame flush), and posed by the rig's walk
// clip while the walker moves over the ground. **First person collapses the head**: the skinning
// matrices of the rig's head joint and every joint below it are replaced, in the matrices the
// renderer reads, by the head joint's position with no linear part (animation.h `JointCollapse`),
// so the camera inside the head sees the shoulders, the chest, the arms and the legs when it looks
// down and nothing of the head when it looks ahead.
//
// Why a load-time actor and not a spawn: a deformed instance cannot be added to the renderer's
// dynamic tail (apps.md "Actors", `actor_spawn`), and the body is there for the whole session.
// Why the actors' path: a module that finds `player` moves and poses the same entity the session
// does, the state file carries it as it carries any actor, and the frame sees one write of the
// instance table a frame whoever moved it.

#if ENGINE_VIEW_ANIMATION && ENGINE_VIEW_GAME

#include "animation.h"
#include "fly_camera.h"
#include "game_props.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>
#include <domain/anim/standard_skeleton.h>
#include <systems/renderer/scene.h>

#include <string>

namespace engine::view {

inline constexpr const char* k_player_name = "player";
// **The player's actor handle** — what `actor_find("player")` answers — a fixed number past any
// scene's instances rather than its index in the whole read. The body is appended after the whole
// read, which is read once there is a device, and a module finds its actors in `on_load`, before
// one: a handle that waited for the read would be a different number by the time the module used
// it. `DeclaredActor::read` carries the index the read gave it.
inline constexpr u32 k_player_actor = 0x7fff'ffffu;

// **The rig, measured before any device** (`measure_player_rig`): what `--player` names, the
// numbers the summary's `interactive.player` block reports, and what the session needs to stand it.
struct PlayerRig {
  std::string mesh;  // the path as given
  u32 joints = 0;
  // The **skinned** bind pose's box, in the mesh's own units: the rest skinning matrices applied to
  // the vertices, because a skinned glTF's vertices stay in bind space (assets.md) and a rig whose
  // inverse binds leave out an ancestor — RiggedFigure's Z_UP — lies down in its raw positions and
  // stands up through its joints.
  Vec3 bind_lo{};
  Vec3 bind_hi{};
  f32 bind_height = 0.0f;  // bind_hi.y - bind_lo.y
  f32 scale = 1.0f;        // the uniform scale to `target_height_m`
  f32 target_height_m = 0.0f;
  // The head the collapse is built from: the standard skeleton's `Head` (anim::map_joints), else
  // the one child of its `Neck` (a rig whose neck chain ends in the head, as RiggedFigure's
  // `neck_joint_2` does), else none. `head_from` says which.
  i32 head = -1;
  std::string head_name;
  const char* head_from = "none";
  Vec3 head_bind{};  // the head joint's bind position as drawn, mesh units
  // The camera's eye less the head joint after scaling, in the body's own frame (x to its right,
  // y up, z ahead of it), metres: how far the eye sits above and ahead of the neck's top. The owner
  // reads it when the eye seems too high or low.
  Vec3 eye_from_head{};
  f32 eye_height_m = 0.0f;
  // The rig's lowest bind point below its origin, after scaling: the origin is stood this far
  // above the walker's feet so the soles are on the ground (0 for a rig whose origin is its feet).
  f32 origin_above_feet_m = 0.0f;
  // **The rig faces +z** (glTF's convention: "the front of a glTF asset faces +Z"), and the
  // engine's heading 0 looks along -z, so the body is turned a half turn past the heading.
  f32 facing_yaw = k_pi;
  std::string walk_clip;  // the library's name of the clip played walking, empty: none
  std::string idle_clip;  // and standing; empty: the rest pose
};

// The rig's head joint (above) and the collapse mask built from it: the head and every joint below
// it in the hierarchy, in joint order. Empty for a rig with no head.
i32 player_head_joint(const anim::Skeleton& skeleton, const anim::JointMapping& mapping,
                      const char*& from) noexcept;
void player_collapse_mask(const anim::Skeleton& skeleton, i32 head, Vector<u32>& out);

// **Before any device**: loads `path`, refuses a mesh that is not a skinned glTF with a sentence
// naming it (a `.clusters` container carries the skin's bindings but not its skeleton or clips;
// apps.md "Animation in a session"), and measures it — the skinned bind pose's box, the scale to
// `target_height_m`, the head and where the eye sits from it, the walk and idle clips.
bool measure_player_rig(const std::string& path, f32 target_height_m, f32 eye_height_m,
                        PlayerRig& out, std::string& error);

// **The session's half**: the body appended to the scene, stood on the walker every tick, and the
// head's collapse on its animated slot.
class PlayerBody {
 public:
  explicit PlayerBody(PlayerRig rig) : rig_(std::move(rig)) {}
  PlayerBody(const PlayerBody&) = delete;
  PlayerBody& operator=(const PlayerBody&) = delete;

  const PlayerRig& rig() const noexcept { return rig_; }
  // The declared actor `player`, handle `k_player_actor`; its `read` is the whole read's index
  // once `append` has run.
  DeclaredActor declared() const;

  // Before `prepare_animation`: the rig as one more mesh and one more instance at the end of the
  // scene's whole read, named nowhere in the file, uniformly scaled, standing at `start` until the
  // walker first stands it, with an animation block (its first clip) so the animated world attaches
  // it. Its index in the whole read is kept. After the whole read, so a streamed world never hands
  // it to the ring (it is past the run `stream_authored` may stream).
  void append(renderer::SceneDesc& desc, const WorldPos& start);
  u32 read_index() const noexcept { return read_; }

  // The session's tick observer, chained **in front of** the one already set (the game's), so the
  // body stands where the walker is before the module's tick reads or moves it. Before
  // `AnimationTicks::attach`, which chains this in turn, so the world steps after both.
  void chain(FlySession& session) noexcept;
  // After the actors are bound (`GameActors::bind`): the joint collapse on the body's slot when the
  // rig has a head (`view.player.collapse_head`, default on), else one stderr line.
  bool bind(AnimatedScene& scene, GameActors& actors, std::string& error);

  // `interactive.player`: the rig's numbers, the collapse, the placements made and the body's
  // actor block as the actors report it.
  JsonValue summary_json(const GameActors* actors) const;

 private:
  static void observe(void* user, const FlySession& session, const input::InputState& input);
  void tick(const FlySession& session);

  PlayerRig rig_;
  u32 read_ = ~0u;
  AnimatedScene* scene_ = nullptr;
  GameActors* actors_ = nullptr;
  FlySession::TickObserver next_ = nullptr;
  void* next_user_ = nullptr;
  f64 walked_ = 0.0;  // the walker's distance at the last tick
  // What the body does: 0 not yet decided, 1 walking (the walk clip), 2 standing.
  u8 gait_ = 0;
  bool collapse_ = false;  // the mask is set on the slot
  Vector<u32> mask_;       // the joints collapsed
  u64 places_ = 0;         // ticks the session stood the body
  u64 flying_ticks_ = 0;   // ticks the body was left where the walker last stood
  u64 walking_ticks_ = 0;  // ticks it played the walk clip
  u64 gait_changes_ = 0;
  u64 refused_ = 0;  // placements the actors refused
};

}  // namespace engine::view

#endif  // ENGINE_VIEW_ANIMATION && ENGINE_VIEW_GAME
