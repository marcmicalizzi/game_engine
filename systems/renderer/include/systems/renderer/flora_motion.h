#pragma once

// **Trees move** (docs/subsystems/renderer.md, "The flora stage"; docs/subsystems/flora.md,
// "Motion"; roadmap S66): the CPU half of the deformed-vertex pool's flora stage. Once per frame
// per swaying instance it turns the frame's wind and the walker's capsule into one
// `gfx::FloraPartPose` per part of the tree, parent-first, which `deform.slang` applies per vertex.
//
// **The model.** A part bends about its pivot by a rotation whose vector is the sum of two:
//
//   wind   towards where the wind blows, about the axis that turns the part's rest direction
//          towards it: `compliance * s * (0.6 + 0.8 g(t))` of steady lean, plus a flutter of
//          `compliance * s * (0.25 + 0.35 g(t)) * sin(2 pi f t + phase)` whose frequency falls as
//          one over the square root of the part's length (`k_flora_flutter_hz` for a metre: a
//          longer part sways slower, and since a bend's tip travel is the angle times the length,
//          farther). `s` is the wind's strength over its record's mean, held to [0,
//          k_flora_max_wind]; `g(t)` the gust, two incommensurate sines of the clock in [0, 1] with
//          phases from the instance's seed; `phase` the part's own, from its pivot in the mesh's
//          frame and the seed — never a world position (ADR-0053). Compliance is the species' per
//          class of part (trunk, limb, twig, leaf; `flora_stiffness`), times `k_flora_load_bearing`
//          for a part the structural table flags load-bearing.
//   push   away from the walker: a part that is not load-bearing (and not the trunk) whose rest
//          axis comes within the capsule's radius plus the part's own reach of the capsule's axis
//          is turned, about its pivot, by the angle that carries its nearest point clear, at most
//          `k_flora_push_max`, eased in and out over `k_flora_push_ease_ticks` frame ticks
//          (smoothstep of a ramp, held per part between frames: `FloraPartState`).
//
// The vertex's bend weight scales the part's angle, so the root stays and the tip travels, and a
// child's frame is its parent's frame times the parent's own bend at the weight where the child
// leaves it (`FloraMesh::attach`), so a frond rides its trunk and a twig its limb. Zero wind and no
// push leaves every part at rest, and a part at rest is passed through bit for bit by the stage.
//
// **Determinism.** A pose is a function of the parts, the clock (the frame index at 60 Hz), the
// instance's seed, the wind the frame names and the walker's capsule, and of the push state the
// frames before left — which is a function of the same, frame by frame. A replay of a recorded walk
// draws every pose it drew.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/gfx/flora_motion.h>

#include <span>
#include <string_view>

namespace engine::renderer {

// Radians of steady lean a part takes at the record's mean wind (s = 1), by class: the trunk (part
// 0), a limb (a child of the trunk), a twig (deeper), and a leaf (a part the structural table gives
// no wood: a spray of cards, a willow's curtain). A palm's fronds are its limbs.
struct FloraStiffness {
  f32 trunk = 0.01f;
  f32 limb = 0.05f;
  f32 twig = 0.09f;
  f32 leaf = 0.14f;
};
// Shared geometry species catalogue (flora.md, "Motion"). An unknown species returns zero
// compliance here and is rejected by flora_mesh; it never silently receives another habit.
FloraStiffness flora_stiffness(std::string_view species) noexcept;

inline constexpr f32 k_flora_load_bearing = 0.25f;  // a load-bearing part's share of its class's
inline constexpr f32 k_flora_max_wind = 2.0f;       // the strength the sway saturates at
inline constexpr f32 k_flora_flutter_hz = 0.9f;     // a 1 m part's flutter; 1/sqrt(length)
inline constexpr f32 k_flora_push_max = 0.9f;       // radians, about 52 degrees
inline constexpr u32 k_flora_push_ease_ticks = 12;  // 0.2 s at the frame index's 60 Hz
inline constexpr f32 k_flora_push_margin = 0.05f;   // metres past the capsule a pushed part clears
inline constexpr f32 k_flora_leaf_reach = 0.12f;    // a part's reach past its axis, of its length

// The `renderer.flora.*` tunables, read once a frame or a load: whether trees sway at all, and the
// wind they sway in where the scene's ground says none — where it blows to in the ground plane
// (x, z) and its strength over a record's mean.
bool flora_motion_tunable() noexcept;
// `renderer.flora.tail` and `renderer.flora.sway_m` (renderer.md, "Streamed trees sway"): whether a
// streamed world's trees sway with their tiles, and the farthest ring floor, metres, whose distance
// form still does.
bool flora_tail_tunable() noexcept;
f32 flora_sway_m_tunable() noexcept;
// `renderer.flora.rest_pose_cascades_m` (renderer.md, "The caster policy"; R95): metres from the
// eye past which a swaying instance casts its rest pose into the cascades; 0, the default, is off.
f32 flora_rest_pose_cascades_m_tunable() noexcept;
struct FloraWindDefault {
  Vec2 to{1.0f, 0.0f};
  f32 strength = 1.0f;
};
FloraWindDefault flora_wind_default() noexcept;

// **What a frame says sways the trees** (`FrameDesc::flora`). The wind: unset, it is the scene
// ground's record at the sky's hour (`FrameDesc::sky_time_s`: the weather follows the day) where
// the ground says one, and the `renderer.flora.wind_*` tunables' where it does not; set, it is
// `wind_to` (where it blows to, x and z) at `wind_strength` over a record's mean — and a strength
// of 0 with no walker draws every tree at rest, to the bit. The walker: its capsule, upright from
// its feet, which bends aside the fronds, leaves and twigs it walks into; a host that walks
// (engine-view's `view::Walker`) sets it every frame it walks.
struct FrameFlora {
  bool wind_set = false;
  Vec2 wind_to{1.0f, 0.0f};
  f32 wind_strength = 0.0f;
  bool walker = false;
  WorldPos walker_feet{};
  f32 walker_radius = 0.3f;
  f32 walker_height = 1.8f;
};

// The wind at an instance, in the instance's mesh frame: where it blows **to** (a unit vector) and
// its strength over the record's mean (0 is calm, 1 a mean day).
struct FloraWind {
  Vec3 to{1.0f, 0.0f, 0.0f};
  f32 strength = 0.0f;
};

// The walker's capsule in the instance's mesh frame: its feet, its radius and its height, upright
// along the mesh's +y.
struct FloraPush {
  bool active = false;
  Vec3 feet{};
  f32 radius = 0.3f;
  f32 height = 1.8f;
};

// One part's push between frames: how far its ease has come (0..1), and the rotation vector it is
// easing towards or away from.
struct FloraPartState {
  f32 ease = 0.0f;
  Vec3 push{};
};

// What a mesh's motion contract becomes at load: per part its compliance, the parent's bend weight
// at its pivot, its reach past its axis, and whether a walker may push it.
//
// `field` is the catalogue's motion kind (geometry::FloraMotionKind; renderer.md, "The field
// kind"): such a mesh is posed by the pool pass alone, from the frame's `gfx::FloraFieldFrame`, and
// takes no run of the pose table, no push state and no tail record — `field_compliance` is the
// species' leaf compliance, the one number its blades bend by.
struct FloraMesh {
  Vector<geometry::ClusterFileMotionPart> parts;
  Vector<f32> compliance;
  Vector<f32> attach;
  Vector<f32> reach;
  Vector<u8> pushable;
  Vector<u32> phase_id;  // per part, the id its phase is taken from: its own, or its source's
  bool field = false;
  f32 field_compliance = 0.0f;
  u32 part_count() const noexcept { return parts.size(); }
};

// ---- the field kind's travelling gust (renderer.md, "The field kind"; grass-sward.md, "Engine
// hand-back: field sway")
// --------------------------------------------------------------------------
//
// Two scales of coherent gust, each two plane waves 20 degrees either side of the wind, travelling
// downwind. The mixing-layer analogy for canopies (Raupach, Finnigan & Brunet 1996; Finnigan 2000)
// puts the streamwise spacing of the canopy-top eddies at about 8 shear lengths, Ls = U(h)/U'(h),
// roughly h/3 for a dense canopy (h/alpha with alpha about 2.8), and their convection at about 1.8
// U(h): the honami of a knee-high (0.5 m) sward is a 1.4 m wave at 1.8 m/s with U(h) taken as 1 m/s
// on a mean day, and the sweeps a 20 m forest edge sheds over a clearing a 57 m one at 4.5 m/s
// (U(h) 2.5 m/s). The flutter's 1.26 Hz is the honami's own frequency, 1.8 / 1.4, between Py et
// al.'s alfalfa (1.05 Hz) and wheat (2.50 Hz). The speeds are fixed, not scaled by the strength:
// a speed that followed a varying wind would make the phase jump by the change times the clock.
inline constexpr f32 k_flora_field_honami_m = 1.4f;
inline constexpr f32 k_flora_field_honami_mps = 1.8f;
inline constexpr f32 k_flora_field_sweep_m = 57.0f;
inline constexpr f32 k_flora_field_sweep_mps = 4.5f;
inline constexpr f32 k_flora_field_spread_rad = 0.349066f;  // 20 degrees off the wind
inline constexpr f32 k_flora_field_flutter_hz = 1.26f;
inline constexpr f32 k_flora_field_weights[4] = {0.2f, 0.2f, 0.3f, 0.3f};  // honami, then sweeps

// The frame's field block: the wind (`to` in the ground plane, unit; `strength` over a record's
// mean), the gust waves' phases at the eye reduced in double (the GPU adds the root's offset from
// the eye), the flutter's phase, and the walker's capsule from the eye when the frame names one.
// A function of its arguments alone: the field kind has no CPU state.
void flora_field_frame(Vec2 to, f32 strength, f64 time_s, const WorldEye& eye,
                       const FrameFlora& frame, gfx::FloraFieldFrame& out) noexcept;

// Builds `out` from a mesh's sections 35 and 37 and its species. False for a table that breaks the
// contract (a parent at or after its child, a part count past 65,536). `ids`, when it is as long as
// `parts`, is each part's id in the mesh a distance form was cut from
// (SourceMesh::motion_part_ids): the sway's phase is taken from it, so a pruned form sways as its
// source does.
bool flora_mesh(std::span<const geometry::ClusterFileMotionPart> parts,
                std::span<const geometry::ClusterFileStructuralPart> structure,
                std::string_view species, FloraMesh& out, std::span<const u32> ids = {});

// **Whether a part stays a collider** (scene_collision.md, "What gets no body"): the trunk, a
// load-bearing limb and a part flagged `k_structural_collider` (a shrub's stout stem) do; with no
// structural table, the trunk alone. The understory's part 0 is a root with no geometry, so a
// fern, a tuft and a herb have no body at all. Fronds, leaves, curtains and
// twigs are walked through, and the push bends them aside.
bool flora_part_collides(std::span<const geometry::ClusterFileStructuralPart> structure,
                         u32 part) noexcept;

// The gust at `time_s` for an instance's seed, in [0, 1].
f32 flora_gust(f64 time_s, u32 seed) noexcept;

// One frame's poses for one instance: `out` and `state` parallel to `mesh.parts`. `ticks` is the
// frame ticks since this instance's last poses (0 on its first), which the push's ease advances by.
void flora_part_poses(const FloraMesh& mesh, const FloraWind& wind, f64 time_s, u32 seed,
                      const FloraPush& push, u32 ticks, std::span<FloraPartState> state,
                      std::span<gfx::FloraPartPose> out) noexcept;

// A bound on how far any vertex of the mesh leaves its rest position under any wind up to
// `k_flora_max_wind`, mesh units: what the instance's spheres are padded by (renderer.md, "The
// flora stage", "Bounds"). `vertices` and `motion` are the mesh's own, parallel.
f32 flora_bounds_padding(const FloraMesh& mesh, std::span<const Vec3> vertices,
                         std::span<const geometry::ClusterFileMotionVertex> motion) noexcept;

}  // namespace engine::renderer
