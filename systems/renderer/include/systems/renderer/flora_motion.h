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
//          farther). `s` is the load the wind's strength over its record's mean gives
//          (`flora_storm_load`: the strength itself to a mean day, its streamlining law past it,
//          held at k_flora_max_wind), and the angle is held by `flora_bend_cap`; `g(t)` the gust,
//          two incommensurate sines of the clock in [0, 1] with phases from the instance's seed;
//          `phase` the part's own, from its pivot in the mesh's frame and the seed — never a world
//          position (ADR-0053). Compliance is the species' per class of part (trunk, limb, twig,
//          leaf; `flora_stiffness`), times `k_flora_load_bearing` for a part the structural table
//          flags load-bearing and not full-sway (a load-bearing part flagged
//          `k_structural_full_sway` keeps its class's compliance: A45).
//   push   away from the walker: a part that is not load-bearing (and not the trunk) whose rest
//          axis comes within the capsule's radius plus the part's own reach (its wood's radius,
//          or `k_flora_leaf_reach` of its length for a part with no wood or one the table flags
//          foliage, whichever is more) of the capsule's axis
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
#include <domain/geometry/flora_species.h>
#include <domain/gfx/cluster_cull.h>
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
// **The storm** (flora.md, "Motion in a storm"; docs/experiments/wind.md, "The trees in a storm";
// roadmap W154). The strength the sway follows its law to and holds past: 8.4 over the 5 m/s mean
// day is 42 m/s, the wind past which fewer than half the trees of a storm stand (Virot et al. 2016,
// Phys. Rev. E 93). The engine builds no failure, so past it a tree holds the pose its law gives
// there. It was 2 (10 m/s) until W154, which a gust over a gale could not pass.
inline constexpr f32 k_flora_max_wind = 8.4f;
inline constexpr f32 k_flora_flutter_hz = 0.9f;  // a 1 m part's flutter; 1/sqrt(length)
// The load a part bends under past the mean day: s for s <= 1 (the model's own lean, the constants
// set for a mean day) and s^b past it, b = 2 + E, E the crown's Vogel exponent by kind
// (`flora_streamline_exponent`): drag on a reconfiguring crown grows as U^(2+E), not U^2, and a
// part's quasi-static bend is its moment over its stiffness, so in proportion to the drag.
// Continuous at s = 1, so a step of the wind through the mean day moves no part by a jump.
//
// `hold` is where the strength is held (`renderer.flora.storm_strength`, `FloraMesh::hold`): the
// sway follows its law to there, and the bound a swaying instance's spheres are padded by is taken
// there, so the two always agree whatever the tunable says.
f32 flora_storm_load(f32 strength, f32 exponent, f32 hold = k_flora_max_wind) noexcept;
// `renderer.flora.storm_strength`, read at the load: the strength (over the 5 m/s mean day) the
// sway is held at and the padding is built for, in [0, k_flora_max_wind] (default: all of it,
// 42 m/s). 2 is main's 10 m/s before W154: its padding, and its hold.
f32 flora_storm_hold_tunable() noexcept;
// The drag's exponent b = 2 + E for a species: a broadleaf's crown (and the shrubs, ferns, herbs,
// vines and palms taken with it) 1.1, a conifer's 1.32, a field's blade 4/3 (the page has the
// sources and the rule). 1 (the model's linear lean) for a species the catalogue lacks.
f32 flora_streamline_exponent(std::string_view species) noexcept;
// **A part bends no further than the wind can carry it** (the elastica's limit): a load that would
// turn it by `raw` radians turns it by `raw` up to half of `most`, and past that by a curve that
// approaches `most` (a tanh, continuous with its first derivative at the knee), never past `raw`.
// `most` is the angle between the part's rest direction and where the wind blows to, the turn that
// leaves it streamed along the wind, and never under `k_flora_bend_floor`.
f32 flora_bend_cap(f32 raw, f32 most) noexcept;
inline constexpr f32 k_flora_bend_floor = 0.785398163f;  // pi / 4: a part near the wind flutters
// A field's blades are posed by the GPU from one load a frame, so their cap is on the load: a
// blade of the most compliant field species (the sward's 0.2) turns at most 1.2 rad (laid over,
// short of the ground) at the gust's and flutter's peak, which is a load of 1.2 / (2 x 0.2) = 3.
inline constexpr f32 k_flora_field_most_load = 3.0f;
// The load the frame's field block carries (`FloraFieldFrame::wind.z`) for a strength.
f32 flora_field_load(f32 strength, f32 hold = k_flora_max_wind) noexcept;
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
// `renderer.flora.padding_records` (W154-2): the swaying instances' records a step of the padding
// ladder rewrites in one frame, at least 1.
u32 flora_padding_records_tunable() noexcept;
// `renderer.flora.sward_near_m` and `sward_fade_m` (renderer.md, "The hand-off by distance";
// G136) as the band an instance with a hand-off role carries (`gfx::pack_handoff_band`):
// [near - fade / 2, near + fade / 2) metres, the low end clamped at zero.
u32 sward_handoff_band_tunable() noexcept;
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
// its strength over the record's mean (0 is calm, 1 a mean day). `max_load` is the load the
// frame's padding holds (`flora_padding_ladder`'s hold): the sway's load is never past it, so a
// tree never leaves the spheres its record carries this frame.
struct FloraWind {
  Vec3 to{1.0f, 0.0f, 0.0f};
  f32 strength = 0.0f;
  f32 max_load = 3.0e38f;
};

// ---- the padding follows the wind (W154-2; renderer.md, "What a storm costs") -----------------
//
// **A calm session pays for no storm.** A swaying instance's spheres are padded by a bound on its
// sway at the load its record says (`flora_rung_padding`), and the load the sway may reach is held
// at the record's: a **ladder** of loads, the lowest main's own (2: the 10 m/s the sway was held at
// before W154, with main's padding to the bit), each rung 1.5 times the one below, the top the
// strongest the scene's meshes can take. The rung is chosen each frame from the wind's ceiling over
// the next seconds (`flora_rung_choose`): up at once when the ceiling passes the rung, down only
// after it has stood below the rung beneath for `k_flora_rung_down_s`, so a gusting wind does not
// rewrite bounds every frame. A step rewrites the records of the scene's swaying instances,
// `renderer.flora.padding_records` a frame (GpuScene::flora_padding_frame), and the hold follows
// the records: up only once every record carries the new rung, down at once.
inline constexpr f32 k_flora_calm_load = 2.0f;  // main's hold, and the ladder's lowest rung
inline constexpr f32 k_flora_rung_ratio = 1.5f;
inline constexpr u32 k_flora_rungs_max = 8;
// Rung 0 serves a ceiling up to this times its load: a mean day's three-second gust over a forest
// (the gust factor 1.89, 9.5 m/s) is a conifer's load of 2.3, and that peak is held at main's 2
// rather than the mean day paying a rung (the page has the trade).
inline constexpr f32 k_flora_calm_slack = 1.2f;
inline constexpr f64 k_flora_rung_down_s = 3.0;
// The load of rung `rung` on a ladder whose top is `top` (the last rung is `top` itself).
f32 flora_rung_load(u32 rung, f32 top) noexcept;
u32 flora_rung_count(f32 top) noexcept;
// The padding of a mesh whose calm padding (rung 0, `flora_padding_at_load` at
// `k_flora_calm_load`) is `calm`, at a rung's `load`: the bound is linear in the load (every
// part's most turn is its compliance times it), a field's load is held at
// `k_flora_field_most_load`, and a hair over the product keeps a float's rounding inside it.
// `load` at or under the calm load answers `calm` itself, to the bit.
f32 flora_rung_padding(f32 calm, bool field, f32 load) noexcept;
// The ladder's state: the rung every record carries (`hold`, what the sway is held at), the rung
// the records are being written to (`target`), and since when the ceiling stood under the rung
// below the target (negative: it has not).
struct FloraLadder {
  u32 hold = 0;
  u32 target = 0;
  f64 below_since_s = -1.0;
};
// The rung a ceiling `demand` (a load) asks for at `time_s`: up at once, down after
// `k_flora_rung_down_s` under the rung below. Returns the new target; `hold` is the caller's.
u32 flora_rung_choose(FloraLadder& ladder, f32 demand, f64 time_s, f32 top) noexcept;

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
  f32 streamline = 1.0f;        // the species' drag exponent past the mean day (`flora_storm_load`)
  f32 hold = k_flora_max_wind;  // the strength it is held at: the tunable's at `flora_mesh`
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
// `hold` is the scene's fields' (`FloraFieldRest::hold`), the strength their padding was built for.
// `max_load` is the frame's padding hold (`FloraWind::max_load`).
void flora_field_frame(Vec2 to, f32 strength, f64 time_s, const WorldEye& eye,
                       const FrameFlora& frame, gfx::FloraFieldFrame& out,
                       f32 hold = k_flora_max_wind, f32 max_load = 3.0e38f) noexcept;

// ---- the far field at rest (renderer.md, "The far field at rest"; gfx/flora_motion.h; R152) ----
//
// **Past `renderer.flora.field_rest_m` a field does not sway, and the frame builds nothing for
// it.** The sway of a blade fades to nothing over the `renderer.flora.field_rest_fade_m` before
// that distance from the eye, and an instance wholly past it takes no pool block and, in the ray
// tracing chain, references its mesh's resident structures instead of having its own built every
// frame. What the rule needs of the scene, found once at load: its field records (one a field
// mesh, made together), the largest |root| of any of their blades (so an instance's roots lie
// within that times its scale of its origin) and the longest blade (the walker's reach).
struct FloraFieldRest {
  u32 first = 0;
  u32 count = 0;
  f32 root_reach = 0.0f;
  f32 blade_max = 0.0f;
  f32 hold = k_flora_max_wind;  // the least of the field meshes' `FloraMesh::hold` (W154)
};
// The scene's part, from its field meshes' blades: `meshes` are the scene's flora meshes and
// `records` each one's field record, or ~0 for a mesh with none (`GpuScene::field_deform_`).
FloraFieldRest flora_field_rest_of(std::span<const FloraMesh> meshes,
                                   std::span<const u32> records) noexcept;
// The tunables: metres from the eye (0: off, the field sways at every distance), and the band.
f32 flora_field_rest_m_tunable() noexcept;
f32 flora_field_rest_fade_m_tunable() noexcept;
// Writes the rule into a frame's block, after `flora_field_frame`: the scene's records always, the
// distances when the rule is on and the scene has a field.
void flora_field_rest(const FloraFieldRest& scene, const FrameFlora& frame,
                      gfx::FloraFieldFrame& out) noexcept;

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
// structural table, a parts-kind tree's legacy trunk alone. Field/none kinds never collide, and
// a structural table must flag even part zero. The understory's part 0 has no geometry, so a
// fern, a tuft and a herb have no body at all. Fronds, leaves, curtains and
// twigs are walked through, and the push bends them aside.
bool flora_part_collides(
    std::span<const geometry::ClusterFileStructuralPart> structure, u32 part,
    geometry::FloraMotionKind kind = geometry::FloraMotionKind::parts) noexcept;

// The gust at `time_s` for an instance's seed, in [0, 1].
f32 flora_gust(f64 time_s, u32 seed) noexcept;

// One frame's poses for one instance: `out` and `state` parallel to `mesh.parts`. `ticks` is the
// frame ticks since this instance's last poses (0 on its first), which the push's ease advances by.
void flora_part_poses(const FloraMesh& mesh, const FloraWind& wind, f64 time_s, u32 seed,
                      const FloraPush& push, u32 ticks, std::span<FloraPartState> state,
                      std::span<gfx::FloraPartPose> out) noexcept;

// A bound on how far any vertex of the mesh leaves its rest position under any wind up to
// `strength` (by default `k_flora_max_wind`, every wind the sway takes), mesh units: what the
// instance's spheres are padded by (renderer.md, "The flora stage", "Bounds"). `vertices` and
// `motion` are the mesh's own, parallel. Each part's most turn is its compliance times the load
// at that strength at the gust's and flutter's peak, held by `flora_bend_cap` with the widest
// `most` (pi, a part pointing into the wind).
f32 flora_bounds_padding(const FloraMesh& mesh, std::span<const Vec3> vertices,
                         std::span<const geometry::ClusterFileMotionVertex> motion,
                         f32 strength = k_flora_max_wind) noexcept;
// **The far forest leans** (W154-2; renderer.md, "The far forest leans"): a mesh's mean lean under
// a unit load as one shear, `dx = lean * y` along where the wind blows, fit by least squares over
// its vertices above its origin: each part's mean turn (its compliance times the lean at the gust's
// mean, 1.0) carried to the vertex as the padding carries its most turn, small angles, along the
// wind, averaged over four winds in the mesh's frame. What a still form leans by when the wind
// passes the still rule's.
f32 flora_mean_lean(const FloraMesh& mesh, std::span<const Vec3> vertices,
                    std::span<const geometry::ClusterFileMotionVertex> motion) noexcept;
// A still form's record leaned by `lean` (x and z: metres a metre of height, where the wind blows
// to) from its record at rest: its 3x4 sheared about its origin, `x += lean.x y`, `z += lean.y y`,
// its scale bound widened by the shear's norm (at most 1 + |lean|) and its uniform-scale flag
// cleared, so the cull's spheres hold the leaned form; `lean` zero gives `rest` back, to the bit.
gfx::InstanceDesc still_lean_record(const gfx::InstanceDesc& rest, Vec2 lean) noexcept;
// The same bound at a **load** rather than a strength, with no cap: every part's most turn is its
// compliance times `load` (a field's held at `k_flora_field_most_load`) at the gust's and
// flutter's peak, so it is linear in the load and holds every pose whose load is at most `load`.
// At `k_flora_calm_load` it is main's padding before W154, operation for operation.
f32 flora_padding_at_load(const FloraMesh& mesh, std::span<const Vec3> vertices,
                          std::span<const geometry::ClusterFileMotionVertex> motion,
                          f32 load) noexcept;

}  // namespace engine::renderer
