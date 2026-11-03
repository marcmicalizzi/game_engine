#pragma once

// **The player's body** (docs/subsystems/apps.md, "The player's body"; roadmap P52, stage A).
//
// `engine-view --player <rig.glb>` stands a skinned character at the walker's feet: a load-time
// actor named `player`, appended to the scene's whole read before the renderer loads it (so the
// animation world binds it as it binds any named instance and the deformed-vertex pool, the cull
// and the ray tracing chain lay it out with the scene), placed every tick by the session through
// the actors' own path (`GameActors::place`, the once-a-frame flush), and posed by the rig's walk
// clip while the walker moves over the ground. **First person sees from the face** (P58,
// `view.player.eye=face`, the default): the camera stands just past the face's furthest forward
// point, raised to the eye line, carried by the head's posed skinning matrix, and the look turns
// the neck and the head (the head-look layer, animation.h `LookJoint`), so the head is drawn whole
// — in the shadow and the reflections — and looking down looks forward and down. The rule before
// it, `eye=walker`, keeps the walker's eye inside the skull and **collapses the head**: the
// skinning matrices of the rig's head joint and every joint below it are replaced, in the matrices
// the renderer reads, by the head joint's position with no linear part (animation.h
// `JointCollapse`).
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
  // The library's name of the clip played walking (`view.player.walk_clip`: by name, any case; a
  // name the rig lacks is its first clip), empty: none (`none`, or a rig with no clip).
  std::string walk_clip;
  // And standing (`view.player.idle_clip`); empty: the rest pose (`none`, or a rig without that
  // name).
  std::string idle_clip;

  // ---- the first-person eye (P58; apps.md "The eye at the face") ------------------------------
  //
  // `view.player.eye`: `face` puts the camera at the face's furthest forward point, `eye_step_m`
  // past it and raised to the eye line, carried every frame by the head's posed, look-turned
  // skinning matrix; `walker` is the rule before it, the walker's eye over the feet. A rig with no
  // head, or no vertex weighted to it, stands the walker's eye whatever the tunable says.
  bool eye_face = false;            // the rule in effect
  const char* eye_from = "walker";  // `eyes` (eye joints), `head` (the head box's eye line),
                                    // `face` (the furthest point's own height), `walker`
  u32 head_vertices = 0;            // the vertices weighted to the head's subtree above the neck
  Vec3 head_lo{};                   // their box at rest in the body's frame, metres from the soles
  Vec3 head_hi{};
  f32 eye_step_m = 0.0f;        // `view.player.eye_step_m`
  f32 eye_line = 0.0f;          // `view.player.eye_line`: from the head box's top, of its height
  Vec3 eye_bind{};              // the eye in the mesh's bind space, mesh units: what the
                                // head's skinning matrix takes to where it stands
  Vec3 eye_point{};             // the eye at rest in the body's frame, metres from the soles
                                // (x its right, y up, z ahead)
  anim::JointMatrix head_rest;  // the head's rest skinning matrix (a run of nothing draws it)
  // **The head-look layer's pivots**, outermost first: the torso (the neck's parent), the neck (the
  // standard skeleton's `Neck` when it is the head's ancestor), the head; -1 where the rig has
  // none.
  i32 torso = -1;
  i32 neck = -1;
  std::string torso_name;
  std::string neck_name;
  // The three pivots at rest in the body's frame, metres from the soles (zero where absent).
  Vec3 torso_point{};
  Vec3 neck_point{};
  Vec3 head_point{};
  bool head_look = false;  // `view.player.head_look` and the face rule
  // The cervical ranges the look is held to (radians): flexion (down), extension (up), axial
  // rotation each way; past them the torso turns (`view.player.neck_*_deg`).
  f32 flexion = 0.0f;
  f32 extension = 0.0f;
  f32 rotation = 0.0f;
  // `view.player.collapse_head` resolved: `auto` collapses under the walker's eye and not under
  // the face's; `true` and `false` force it.
  bool collapse_head = true;
};

// **The look's share per pivot** (apps.md, "The head-look layer"): the look's pitch (up positive,
// the session's sign) and its yaw relative to the body split into the torso's, the neck's and the
// head's turns — the cervical share held to the rig's ranges and split between neck and head by
// the craniocervical junction's part of each motion, the rest to the torso. `down[i]`, `turn[i]`
// in the order torso, neck, head; a pivot the rig lacks gets nothing (a missing neck gives its
// share to the head, a missing torso drops what is past the range).
struct LookShares {
  f32 down[3] = {0.0f, 0.0f, 0.0f};
  f32 turn[3] = {0.0f, 0.0f, 0.0f};
};
LookShares player_look_shares(const PlayerRig& rig, f32 pitch, f32 yaw_relative) noexcept;
// The craniocervical junction's part of the cervical motion: occiput–C1 is 24% of the cervical
// spine's flexion–extension and C1–C2 53% of its axial rotation (Farber et al. 2025, 1,009 motion
// segments; apps.md "The head-look layer").
inline constexpr f32 k_head_flexion_share = 0.24f;
inline constexpr f32 k_head_rotation_share = 0.53f;

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

  // **The frame's camera under `view.player.eye=face`** (P58): `drawn` is the session's state the
  // frame draws (a tick's, or a live frame's interpolation of two), `eye_height` the walker's.
  // Sets the head-look layer from its pitch (`set_joint_look_angles`), then puts the camera at the
  // eye point through the head's posed, look-turned skinning matrix, the body placed at the
  // walker's feet turned to `drawn`'s heading and scaled as the actor is — so looking down moves
  // the eye forward and down along the face. The orientation stays `drawn`'s. False (and nothing
  // set) under the walker's rule, before `bind`, or with no head: the caller keeps its camera.
  bool face_camera(const FlyState& drawn, f32 eye_height, f32 fov_y, f32 znear,
                   renderer::Camera& out);
  // A marker capture's camera was the last `face_camera`: kept for the summary (at most 16).
  void note_marker(u64 tick);

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
  u32 entry_ = ~0u;        // the body's entry in `AnimatedScene::collapse`
  bool look_ = false;      // the head-look layer is set on it
  // The last face camera (`face_camera`): the look's shares, the eye in the body's frame and in
  // the world; and the markers' (`note_marker`).
  struct FaceEye {
    u64 tick = 0;
    f32 pitch = 0.0f;
    LookShares shares;
    Vec3 eye{};  // the body's frame, metres from the soles
    WorldPos position{};
  };
  FaceEye last_;
  u64 face_frames_ = 0;
  Vector<FaceEye> markers_;
  u64 places_ = 0;         // ticks the session stood the body
  u64 flying_ticks_ = 0;   // ticks the body was left where the walker last stood
  u64 walking_ticks_ = 0;  // ticks it played the walk clip
  u64 gait_changes_ = 0;
  u64 refused_ = 0;  // placements the actors refused
};

}  // namespace engine::view

#endif  // ENGINE_VIEW_ANIMATION && ENGINE_VIEW_GAME
