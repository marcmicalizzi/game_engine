#pragma once

// A soft body from a tissue region (docs/subsystems/tissue.md, "A soft body from a runtime region";
// docs/subsystems/physics.md, "A soft body from a tissue region"): a `Runtime` region of four-node
// cells — a derived cage (cage.h) or one imported as authored — built as `domain/physics`'s soft
// body through its existing wrapping (`physics::SoftBodyDesc`, ADR-0026's backend surface), and
// stepped at the fixed step under the load a state names until it settles.
//
// **What the definition becomes**, and nothing else:
//
//   cells        one volume constraint each, compliance 36 V0 / (K - 5 mu / 3), and one distance
//                constraint per edge, compliance l0^2 / (2 w) with w = (5/4) sum(mu V0) over the
//                cells sharing it: the XPBD form of the declared energy `bulk-edge-v0`
//                (src/energy.h), whose constants make the declared (K, mu) exact in the isotropic
//                part for any cell shape; V0 and l0 at the rest
//   materials    each cell's phases mixed by fraction, as region.materials mixes them
//   mass         density times volume, a quarter of each cell to its corners, at the construction
//                (where the densities are declared)
//   attachments  as the constraints they declare: `Fixed` held (`physics::AttachmentKind::Rigid`,
//                a stiff spring's limit: the module's `Spring` sags a held node by g dt^2 / 2 a
//                step under gravity, and no conversion from `stiffness_pa` to a follow rate
//                exists), where the node rests, carried by its frame's motion from the rest state
//                to the load state;
//                `SlidingBilateral` and `Unilateral` as contact with their target, which is the
//                physics module's own vertex-against-triangle contact and therefore one-sided
//   frames       static triangle meshes where the load state's FrameState puts them (the
//   construction
//                where it has none): the kinematic targets and the contact
//   the load     the state's gravity, in the body's coordinates, less the medium's buoyancy at the
//                body's mean density
//
// The rest shape is the region's `Rest` state (or one named), where the particles start. What it
// does not read yet: membranes, cables, a cable's slack and recruitment, a spring's `stiffness_pa`
// (no conversion to a follow rate exists), a bone target, another region's sheet. Content-build and
// test code: it builds a `physics::World` of its own, single-threaded, deterministic.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/physics/deformable.h>
#include <domain/physics/soft_body.h>
#include <domain/tissue/tissue_file.h>

#include <string>

namespace engine::tissue {

struct SoftBodyOptions {
  // The region; empty takes the file's one Runtime region of four-node cells.
  std::string region;
  // The rest shape; empty takes the region's Rest state, and the construction when it has none.
  std::string rest_state;
  // The state whose load the body is stepped under, and whose nodes the settled body is measured
  // against. Required by `settle_soft_body`.
  std::string load_state;
  // The fixed step and ADR-0029's defaults: 8 iterations (XPBD sub-steps) at 2 collision sub-steps.
  u32 step_hz = 60;
  u32 iterations = physics::k_cage_iterations_default;
  u32 sub_steps = physics::k_cage_sub_steps_default;
  // Plan 07 §7.10's defaults: `material.damping` 4 per second, `limits.max_strain` 0.5.
  f32 linear_damping = 4.0f;
  f32 max_strain = 0.5f;
  // A sliding attachment is free tangentially: the contact carries no friction.
  f32 friction = 0.0f;
  // Settled: the `settle_quantile` of the nodes' speeds below `settle_speed_m_s` for
  // `settle_steps` steps in a row (0.1 mm/s, the 99th percentile, half a second). A quantile and
  // not the fastest node, because a node sliding on a triangulated support can chatter between two
  // faces' planes for ever at a fraction of a millimetre while the body has stopped: those nodes
  // are reported (`SettleResult::still_moving`), not waited for. Given up after `max_steps` (50 s
  // at 60 Hz).
  f32 settle_speed_m_s = 1.0e-4f;
  f32 settle_quantile = 0.99f;
  u32 settle_steps = 30;
  u32 max_steps = 3000;
};

// A target the body is held to or contacts: a frame or a surface of the definition.
struct SoftBodyTarget {
  std::string name;
  bool frame = true;      // a Frame, or a Surface
  Vector<Vec3> vertices;  // where the load state puts it
  Vector<u32> triangles;  // wound so that the region is in front (the backend's contact is
                          // one-sided): a frame outward, a surface toward the region
  bool contact = false;   // a sliding or unilateral attachment's target, or a frame
};

// The body as `physics::SoftBodyDesc` takes it, and what it was built from.
struct TissueSoftBody {
  Vector<Vec3> rest;  // the rest shape, where the particles start
  Vector<f32> inverse_masses;
  Vector<physics::SoftEdge> edges;
  Vector<physics::SoftVolumeConstraint> volumes;
  Vector<u32> faces;  // the cells' boundary, outward
  // Held nodes: the target (an index of `targets`, or -1 the world), the node's place in the
  // target's own coordinates, and the kind.
  struct Anchor {
    u32 vertex = 0;
    i32 target = -1;
    Vec3 local{};
    physics::AttachmentKind kind = physics::AttachmentKind::Rigid;
  };
  Vector<Anchor> anchors;
  Vector<SoftBodyTarget> targets;
  Vec3 gravity{};  // the load's, less the medium's buoyancy
  f64 mass_kg = 0.0;
  f64 volume_m3 = 0.0;
  bool hero = false;
  std::string region;
  std::string rest_state;
  std::string load_state;
  Vector<std::string> notes;  // what the definition carries that the body does not read
};

// Builds the body. False, with a sentence, for a region that is not four-node cells, a state that
// does not exist, a material the law refuses (K < 5 mu / 3), or an attachment to a target the body
// cannot follow (a bone, another region's sheet).
bool build_soft_body(const TissueFile& file, const SoftBodyOptions& options, TissueSoftBody& out,
                     std::string* error = nullptr);

// A settle: the body stepped at the fixed step under the load state's load from its rest until it
// settles, and the settled nodes against the load state's.
struct SettleResult {
  bool settled = false;
  u32 steps = 0;
  f64 simulated_s = 0.0;
  f64 wall_ms = 0.0;                   // the steps alone, single-threaded
  f64 final_speed_m_s = 0.0;           // the fastest node over the last step
  u32 fastest_node = 0;                // which node that was
  f64 final_quantile_speed_m_s = 0.0;  // the settle quantile's speed over the last step
  u32 still_moving = 0;                // nodes faster than the settle speed over the last step
  Vector<f32> speed_trace_m_s;  // the settle quantile's speed at the end of each simulated second
  Vector<Vec3> nodes;           // where it settled
  Vector<f64> distance_m;       // per node, from the load state's position
  u32 inverted = 0;             // cells not positive at the end
  u32 strain_clamp_sweeps_max = 0;  // the most sweeps the strain clamp needed in a step
};
bool settle_soft_body(const TissueFile& file, const SoftBodyOptions& options, SettleResult& out,
                      std::string* error = nullptr);

// The settle as one JSON object: the steps and time, the distance distribution (p50, p95, max) in
// millimetres and where the largest is (the node, the node sets holding it, whether it is on the
// boundary), and the load state's own displacement from the rest beside it.
JsonValue settle_json(const TissueFile& file, const SoftBodyOptions& options,
                      const SettleResult& result);

}  // namespace engine::tissue
