// Experiment E19 (docs/plan/09-testing-profiling.md §9.6, docs/plan/10-roadmap-risks.md §10.5,
// docs/experiments/e19-lattice-cage.md): a lattice cage around a rigid two-bone core, pressed
// to 30% of its depth by a kinematic plate, held, and released. It decides whether the lattice
// is ADR-0026's v1 cage kind and what the default element, iteration, and sub-step budgets are.
//
// Three benchmarks, because the question has two halves that want different harnesses:
//
//   physics.e19.tick        the cost grid. Build the fixture, press it, hold it, and let the
//                           bench harness time steady-state ticks under load — 3 element counts
//                           x 3 worker counts x 3 iteration counts x 2 sub-step counts. The
//                           harness's own JSON lines carry the timing.
//   physics.e19.press       the physics. One full press-hold-release per timed iteration,
//                           printing strain, volume, recovery, and interpenetration as a JSON
//                           line of its own, swept over element, iteration, and sub-step counts
//                           (worker count does not enter: the world is deterministic in it).
//   physics.e19.sustained   E22's shape, which costs nothing extra here: the same cage under a
//                           rigid load of 1x and 5x its own mass for 20 s, for creep,
//                           oscillation, plane penetration, and volume drift.
//
// Re-running this elsewhere (the Titan X baseline machine, docs/ci/self-hosted-runners.md):
//
//   engine_physics_bench.exe --filter=physics.e19.tick --json=e19-tick.jsonl
//   engine_physics_bench.exe --filter=physics.e19.press --repeats=1 --warmup=0 --quiet
//   engine_physics_bench.exe --filter=physics.e19.sustained --repeats=1 --warmup=0 --quiet
//
// The last two print one `{...}` line per configuration, so their stdout is a JSON-lines file.
// The `#` lines describe the fixture. Debug builds only smoke-run benchmarks, so the debug
// fixture is deliberately tiny; every number in the write-up comes from msvc-release.
//
// Since ADR-0029 the grids carry a `mode` dimension: `fixed` has both decisions in — the volume
// compliance derived from the cell size and the authored `limits.max_strain` enforced — `unfixed`
// is the fixture as the experiment first ran, and `clamp-only` and `conversion-only` turn one of
// the two on at a time so that a row that moved can say *which* change moved it. The controls run
// in the same session as the fixed grid, so the write-up compares rows taken minutes apart on a
// machine whose state is recorded, not rows taken months apart on a machine whose state was not.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/physics/physics.h>
#include <foundation/bench/bench.h>

#include <cmath>
#include <cstdio>

using namespace engine;
using namespace engine::physics;

namespace {

// --- the fixture's dimensions ----------------------------------------------------------------
//
// One geometry for every element count, so the sweep is about cage *resolution* and not about
// cage size: the cube is always 0.40 m on a side and the spacing follows from n.

constexpr f32 k_cage_side = 0.40f;
constexpr f32 k_ground_gap = 0.02f;
// Foam and flesh, not steel. A cage at 1e-6 edge compliance is effectively inextensible and
// pressing 30% out of it is a fight the solver does not win (domain/physics/tests).
constexpr f32 k_edge_compliance = 1.0e-4f;
// **Not the same number as the edge compliance, and not by a little.** XPBD divides the
// constraint's compliance by dt^2 and compares it against the sum of inverse masses times the
// squared constraint gradient. For an edge the gradient is 1, so at 0.19 kg a particle that sum
// is about 10 and a compliance of 1e-4 (0.36 after the dt^2 division) is a stiff edge. For a
// tetrahedron's volume the gradient is an *area*, so for a 6 cm cell the same sum is about
// 1e-4 — four orders of magnitude smaller — and a compliance of 1e-4 does not soften the
// constraint, it switches it off. Measured: at 1e-4 the cage loses a third of its volume under
// the press and never gets it back (0.67 of rest an hour later, 68 mm RMS from its rest pose);
// at 1e-8 it returns to 1.000 of rest within one step of the plate leaving.
//
// 1e-8 is the number the first run of this experiment found by hand, and it is what the
// `unfixed` control mode below still uses. The fixed mode asks
// `volume_compliance_for(preservation, cell)` for it instead (ADR-0029 decision 3), which is the
// same material expressed as a number an artist can author and a generator can rescale: at this
// fixture's 5.71 cm cell the plan's default preservation of 0.8 is 4.2e-9, and 1e-8 is a
// preservation of 0.63. The two modes are what separate "the conversion works" from "the
// experiment was re-run on a different day".
constexpr f32 k_volume_compliance_hand_tuned = 1.0e-8f;
// plan 07 §7.10's defaults, which the fixed mode authors against instead of hand-tuning.
constexpr f32 k_volume_preservation = 0.8f;
constexpr f32 k_max_strain = 0.5f;
// plan 07 §7.10's default: element mass is density x element volume.
constexpr f32 k_density = 1000.0f;
// The core's two bones: a shallow elbow in the XY plane, well inside the cage.
constexpr f32 k_bone_radius = 0.040f;
// "A stiff spring rather than a weld" (plan 05 §5.14). At a 60 Hz step this closes half the
// gap to the bone each step, so bone-adjacent tissue tracks but still lags and still carries
// momentum, which is what makes the interpenetration count a measurement rather than a
// tautology.
constexpr f32 k_attach_follow_rate = 30.0f;
// The shell of cage vertices just outside the bone surface is what hangs off the bone.
constexpr f32 k_attach_band = 0.045f;
// A thick plate, not a sheet: soft-body particles are points to the solver and one flung a few
// centimetres in a step can end up on the far side of a thin obstacle (domain/physics/tests).
constexpr f32 k_plate_half = 0.20f;
// Friction at the cage's two flat contacts. It is a fixture parameter and not a detail: a cage
// gripped top and bottom cannot bulge sideways, and a volume it cannot displace it has to lose.
constexpr f32 k_contact_friction = 0.6f;

// A CTest smoke run (`--smoke`, in every build) only checks that the fixture runs, so it gets a
// four-a-side cage and phases twelve times shorter, decided at run time from the harness's flag
// rather than from the build type: the release grid registers 54 variants, and at full length
// they cost a hosted CI runner minutes it is billed for.
constexpr u32 k_smoke_side = 4;
u32 step_scale() { return bench::smoke_mode() ? 12 : 1; }
u32 scaled(u32 steps) {
  const u32 s = steps / step_scale();
  return s < 2 ? 2 : s;
}

// Phase lengths at 60 Hz. The press is 0.5 s, the hold 1 s, the release 0.33 s, and the
// recovery window 3 s (E19 asks for the shape 1 s after release, which is inside it).
u32 settle_steps() { return scaled(150); }
u32 press_steps() { return scaled(30); }
u32 hold_steps() { return scaled(60); }
u32 lift_steps() { return scaled(20); }
u32 recover_steps() { return scaled(180); }
u32 one_second() { return scaled(60); }

// --- the configuration grid ---------------------------------------------------------------
//
// One i64 per benchmark variant is all `ENGINE_BENCH_ARGS` carries, so the five dimensions are
// packed into a decimal key that stays readable in the result name: 512080162 is 512 elements,
// 8 workers, 16 iterations, 2 sub-steps with ADR-0029's fixes in, and 1512080162 is the same
// configuration as the experiment first ran it.
//
// `mode` is the control. ADR-0029 changed two things about the fixture — where the volume
// compliance comes from and whether the authored strain limit is enforced — and a re-run on a
// different day cannot tell "the fixes did this" from "the machine was quieter this time" unless
// both are measured in the same session. So the unfixed configurations run beside the fixed ones
// and the write-up compares rows taken minutes apart rather than months apart.

// ADR-0029 changed two independent things, so the control has four settings rather than two: a
// row that moves needs to say *which* change moved it.
enum class Mode : u32 {
  Fixed = 0,          // both: the compliance conversion and the enforced strain limit
  Unfixed = 1,        // neither: the experiment exactly as it first ran
  ClampOnly = 2,      // the hand-tuned 1e-8 compliance, with the limit enforced
  ConversionOnly = 3  // the conversion, with nothing enforcing the limit
};

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::Fixed: return "fixed";
    case Mode::Unfixed: return "unfixed";
    case Mode::ClampOnly: return "clamp-only";
    case Mode::ConversionOnly: return "conversion-only";
  }
  return "?";
}

constexpr i64 e19_key(i64 elements, i64 workers, i64 iterations, i64 sub_steps, i64 mode = 0) {
  return mode * 1000000000 + elements * 1000000 + workers * 10000 + iterations * 10 + sub_steps;
}

struct Config {
  u32 elements = 0;
  u32 workers = 0;
  u32 iterations = 0;
  u32 sub_steps = 0;
  Mode mode = Mode::Fixed;

  bool converts() const noexcept { return mode == Mode::Fixed || mode == Mode::ConversionOnly; }
  bool clamps() const noexcept { return mode == Mode::Fixed || mode == Mode::ClampOnly; }
};

Config decode(i64 key) {
  Config c;
  c.mode = static_cast<Mode>(static_cast<u32>(key / 1000000000));
  c.elements = static_cast<u32>((key / 1000000) % 1000);
  c.workers = static_cast<u32>((key / 10000) % 100);
  c.iterations = static_cast<u32>((key / 10) % 1000);
  c.sub_steps = static_cast<u32>(key % 10);
  return c;
}

// 343, 512, and 729 particles are 7, 8, and 9 a side: the nominal 300 / 500 / 800 of the plan's
// E19 row, rounded to what a regular lattice can actually be.
[[maybe_unused]] u32 lattice_side(u32 elements) {
  u32 n = 2;
  while ((n + 1) * (n + 1) * (n + 1) <= elements)
    ++n;
  return n;
}

// --- small geometry helpers -----------------------------------------------------------------

f32 distance_to_segment(Vec3 p, Vec3 a, Vec3 b) noexcept {
  const Vec3 ab = b - a;
  const f32 len_sq = length_squared(ab);
  const f32 t = len_sq > 0.0f ? clamp(dot(p - a, ab) / len_sq, 0.0f, 1.0f) : 0.0f;
  return length(p - (a + ab * t));
}

Quat rotation_from_y(Vec3 dir) noexcept {
  const Vec3 up(0.0f, 1.0f, 0.0f);
  const f32 d = clamp(dot(up, dir), -1.0f, 1.0f);
  if (d > 0.9999f) return Quat::identity();
  if (d < -0.9999f) return quat_from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), k_pi);
  return quat_from_axis_angle(cross(up, dir), std::acos(d));
}

struct Extent {
  f32 min = 0.0f;
  f32 max = 0.0f;
  f32 size() const noexcept { return max - min; }
};

Extent vertical_extent(std::span<const Vec3> points) noexcept {
  Extent e{points[0].y, points[0].y};
  for (const Vec3& p : points) {
    if (p.y < e.min) e.min = p.y;
    if (p.y > e.max) e.max = p.y;
  }
  return e;
}

// Enclosed volume of a closed, outward-wound triangle surface, by the divergence theorem. The
// lattice builder's `faces` are exactly that surface.
f64 surface_volume(std::span<const Vec3> points, std::span<const u32> faces) noexcept {
  f64 six_v = 0.0;
  for (usize i = 0; i + 2 < faces.size(); i += 3) {
    const Vec3 a = points[faces[i]];
    const Vec3 b = points[faces[i + 1]];
    const Vec3 c = points[faces[i + 2]];
    six_v += static_cast<f64>(dot(a, cross(b, c)));
  }
  return six_v / 6.0;
}

// --- the scene ------------------------------------------------------------------------------

struct Bone {
  Vec3 a{};
  Vec3 b{};
  BodyId body{};
  Transform3 transform;
};

struct Scene {
  World world;
  LatticeVolume lattice;
  SoftBodyId cage{};
  BodyId plate{};
  Bone bones[2];
  Vector<Vec3> points;
  Vector<Vec3> settled;
  Vector<f32> rest_lengths;
  u32 vertex_count = 0;
  u32 rigid_attachments = 0;
  u32 spring_attachments = 0;
  u32 attached_inside_core = 0;
  f32 dt = 1.0f / 60.0f;
  u32 sub_steps = 1;
  f32 cage_origin_y = 0.0f;
  f32 total_mass = 0.0f;
  f32 rest_height = 0.0f;
  f64 rest_volume = 0.0;
  f32 plate_y = 0.0f;
  f32 spacing = 0.0f;
  f32 volume_compliance = 0.0f;
  f32 max_strain = 0.0f;
  // ADR-0029's budget, sampled every step of the phase the budget is written against: the cage
  // held under the press with its full contact set.
  f64 budget_ms_sum = 0.0;
  f64 budget_ms_peak = 0.0;
  u32 budget_samples = 0;
  f64 clamp_sweeps_sum = 0.0;
  u32 clamp_sweeps_peak = 0;
  u32 clamp_saturated_ticks = 0;

  void sample_budget() {
    const SoftBodyBudget budget = world.stats().soft_body_budget;
    const f64 ms = static_cast<f64>(budget.ambient_ms) + static_cast<f64>(budget.hero_ms);
    budget_ms_sum += ms;
    if (ms > budget_ms_peak) budget_ms_peak = ms;
    clamp_sweeps_sum += static_cast<f64>(budget.strain_clamp_sweeps);
    if (budget.strain_clamp_sweeps > clamp_sweeps_peak)
      clamp_sweeps_peak = budget.strain_clamp_sweeps;
    if (budget.strain_clamp_saturated) ++clamp_saturated_ticks;
    ++budget_samples;
  }
  f64 clamp_sweeps_mean() const noexcept {
    return budget_samples != 0 ? clamp_sweeps_sum / static_cast<f64>(budget_samples) : 0.0;
  }
  f64 budget_ms_mean() const noexcept {
    return budget_samples != 0 ? budget_ms_sum / static_cast<f64>(budget_samples) : 0.0;
  }

  void read() {
    ENGINE_VERIFY(world.read_soft_body_vertices(cage, std::span<Vec3>(points)) == vertex_count,
                  "e19: short vertex read");
  }

  // True when a cage particle is strictly inside a bone's capsule.
  bool inside_core(Vec3 p) const noexcept {
    for (const Bone& bone : bones)
      if (distance_to_segment(p, bone.a, bone.b) < k_bone_radius) return true;
    return false;
  }

  void step() {
    // The core is kinematically driven every step, as plan 05 §5.14's flow requires: animation
    // writes the skeleton and the solver reads it. It is held still here because E19 is about
    // compression around a rigid internal structure; bone-driven motion is E20's question.
    for (const Bone& bone : bones)
      ENGINE_VERIFY(world.move_kinematic(bone.body, bone.transform, dt), "e19: bone gone");
    ENGINE_VERIFY(world.step(dt, sub_steps) == Status::Ok, "e19: step failed");
  }

  // Driven, not teleported: a kinematic body that reaches its target in one step is what pushes
  // dynamic things instead of passing through them.
  void move_plate(f32 y) {
    plate_y = y;
    Transform3 target;
    target.position = Vec3(0.0f, plate_y, 0.0f);
    ENGINE_VERIFY(world.move_kinematic(plate, target, dt), "e19: plate gone");
  }

  // Teleported, which is only ever right while the plate is clear of everything: driving it
  // there instead would give it a metre per step of velocity and fire it through the cage.
  void place_plate(f32 y) {
    plate_y = y;
    Transform3 target;
    target.position = Vec3(0.0f, plate_y, 0.0f);
    ENGINE_VERIFY(world.set_body_transform(plate, target), "e19: plate gone");
  }

  void build(const Config& config, jobs::JobSystem* job_system);
  void settle();
};

void Scene::build(const Config& config, jobs::JobSystem* job_system) {
  const u32 n = bench::smoke_mode() ? k_smoke_side : lattice_side(config.elements);
  spacing = k_cage_side / static_cast<f32>(n - 1);
  // The two things ADR-0029 changed about this fixture. `Unfixed` is the experiment as it first
  // ran: a hand-tuned volume compliance and an authored strain limit that nothing read.
  volume_compliance = config.converts() ? volume_compliance_for(k_volume_preservation, spacing)
                                        : k_volume_compliance_hand_tuned;
  max_strain = config.clamps() ? k_max_strain : 0.0f;

  WorldOptions options;
  options.max_bodies = 64;
  options.max_body_pairs = 8192;
  options.max_contact_constraints = 8192;
  options.job_system = job_system;
  options.worker_count = config.workers;
  ENGINE_VERIFY(world.init(options) == Status::Ok, "e19: world init failed");
  dt = world.step_seconds();
  sub_steps = config.sub_steps;

  // Ground: a static box whose top face is at y = 0.
  {
    ShapeId shape;
    ENGINE_VERIFY(world.create_box(Vec3(4.0f, 0.5f, 4.0f), shape) == Status::Ok, "e19: ground");
    BodyDesc desc;
    desc.shape = shape;
    desc.transform.position = Vec3(0.0f, -0.5f, 0.0f);
    desc.motion = MotionType::Static;
    desc.layer = Layer::Static;
    desc.friction = k_contact_friction;
    BodyId body;
    ENGINE_VERIFY(world.create_body(desc, body) == Status::Ok, "e19: ground body");
  }

  cage_origin_y = 0.5f * k_cage_side + k_ground_gap;

  // The two-bone core, a shallow elbow in the XY plane through the cage's centre. Kinematic, so
  // it is infinitely heavy to the cage and to the plate alike.
  const Vec3 joint(0.0f, cage_origin_y + 0.02f, 0.0f);
  bones[0].a = Vec3(-0.15f, cage_origin_y - 0.02f, 0.0f);
  bones[0].b = joint;
  bones[1].a = joint;
  bones[1].b = Vec3(0.15f, cage_origin_y - 0.02f, 0.0f);
  for (Bone& bone : bones) {
    const Vec3 axis = bone.b - bone.a;
    ShapeId shape;
    ENGINE_VERIFY(world.create_capsule(0.5f * length(axis), k_bone_radius, shape) == Status::Ok,
                  "e19: bone shape");
    BodyDesc desc;
    desc.shape = shape;
    desc.transform.position = (bone.a + bone.b) * 0.5f;
    desc.transform.rotation = rotation_from_y(normalize(axis));
    desc.motion = MotionType::Kinematic;
    desc.layer = Layer::Kinematic;
    desc.friction = 0.6f;
    bone.transform = desc.transform;
    ENGINE_VERIFY(world.create_body(desc, bone.body) == Status::Ok, "e19: bone body");
  }

  // The cage.
  lattice = build_lattice_volume(n, spacing, k_edge_compliance, volume_compliance);
  vertex_count = static_cast<u32>(lattice.vertices.size());
  total_mass = k_density * k_cage_side * k_cage_side * k_cage_side;
  const f32 inverse_mass = static_cast<f32>(vertex_count) / total_mass;
  for (f32& m : lattice.inverse_masses)
    m = inverse_mass;

  // Attachments. A cage generated from a signed distance field around a skeleton has elements
  // that fall *inside* the bone and elements that sit just outside it, and ADR-0026 binds both
  // to the bone. The two kinds map onto exactly that split: inside the bone is Rigid, because
  // an element inside the bone must not be pushable out of it; the shell just outside is
  // Spring, because that is tissue, and tissue lags, carries momentum, and can be pressed
  // towards the bone. Only the second kind can produce an interpenetration, which is what
  // makes "no element passes through the core" a measurement and not a tautology.
  Vector<SoftAttachment> attachments;
  for (u32 i = 0; i < vertex_count; ++i) {
    const Vec3 world_point = lattice.vertices[i] + Vec3(0.0f, cage_origin_y, 0.0f);
    f32 best = 1.0e30f;
    u32 best_bone = 0;
    for (u32 b = 0; b < 2; ++b) {
      const f32 d = distance_to_segment(world_point, bones[b].a, bones[b].b);
      if (d < best) {
        best = d;
        best_bone = b;
      }
    }
    if (best > k_bone_radius + k_attach_band) continue;
    SoftAttachment attachment;
    attachment.vertex = i;
    attachment.body = bones[best_bone].body;
    // The anchor in the bone's local frame, which is where the particle rests.
    const Transform3& bone_transform = bones[best_bone].transform;
    const Vec3 offset = world_point - bone_transform.position;
    attachment.local_point = rotate(conjugate(bone_transform.rotation), offset);
    if (best < k_bone_radius) {
      attachment.kind = AttachmentKind::Rigid;
      ++rigid_attachments;
      ++attached_inside_core;
    } else {
      attachment.kind = AttachmentKind::Spring;
      attachment.follow_rate = k_attach_follow_rate;
      ++spring_attachments;
    }
    attachments.push_back(attachment);
  }

  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(lattice.vertices);
  desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
  desc.edges = std::span<const SoftEdge>(lattice.edges);
  desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
  desc.faces = std::span<const u32>(lattice.faces);
  desc.attachments = std::span<const SoftAttachment>(attachments);
  desc.transform.position = Vec3(0.0f, cage_origin_y, 0.0f);
  desc.iterations = config.iterations;
  desc.max_strain = max_strain;
  desc.friction = k_contact_friction;
  // Particles are points to the solver unless they are given a radius, and a point that ends up
  // inside a collider is pushed out through whichever face is nearest — which, for a particle
  // driven a few millimetres into a plate by a press, is as often the side as the bottom. A
  // radius of a third of the cell catches the contact before that happens; it is also the
  // honest reading of a cage element, which stands for a lump of material and not a point.
  desc.vertex_radius = 0.33f * spacing;
  desc.allow_sleeping = false;
  ENGINE_VERIFY(world.create_soft_body(desc, cage) == Status::Ok, "e19: cage");

  // The plate, parked clear above.
  {
    ShapeId shape;
    ENGINE_VERIFY(world.create_box(Vec3(0.5f, k_plate_half, 0.5f), shape) == Status::Ok,
                  "e19: plate shape");
    BodyDesc plate_desc;
    plate_desc.shape = shape;
    plate_desc.transform.position = Vec3(0.0f, 1.5f, 0.0f);
    plate_desc.motion = MotionType::Kinematic;
    plate_desc.layer = Layer::Kinematic;
    plate_desc.friction = k_contact_friction;
    ENGINE_VERIFY(world.create_body(plate_desc, plate) == Status::Ok, "e19: plate");
    plate_y = plate_desc.transform.position.y;
  }

  points.resize(vertex_count);
  settled.resize(vertex_count);
  rest_lengths.reserve(lattice.edges.size());
  for (const SoftEdge& edge : lattice.edges)
    rest_lengths.push_back(length(lattice.vertices[edge.a] - lattice.vertices[edge.b]));
  world.optimize_broad_phase();
}

void Scene::settle() {
  for (u32 i = 0; i < settle_steps(); ++i)
    step();
  read();
  settled.clear();
  settled.append(std::span<const Vec3>(points));
  rest_height = vertical_extent(std::span<const Vec3>(points)).size();
  rest_volume = surface_volume(std::span<const Vec3>(points), std::span<const u32>(lattice.faces));
}

// The largest edge stretch ratio in the current pose: E19's "element strain".
f32 max_stretch(const Scene& scene) noexcept {
  f32 worst = 0.0f;
  for (u32 i = 0; i < scene.lattice.edges.size(); ++i) {
    const SoftEdge& edge = scene.lattice.edges[i];
    const f32 rest = scene.rest_lengths[i];
    if (!(rest > 0.0f)) continue;
    const f32 ratio = length(scene.points[edge.a] - scene.points[edge.b]) / rest;
    if (ratio > worst) worst = ratio;
  }
  return worst;
}

u32 interpenetration_count(const Scene& scene) noexcept {
  u32 count = 0;
  for (u32 i = 0; i < scene.vertex_count; ++i) {
    // The particles that are inside the core by construction are the Rigid-attached ones; they
    // are the bone, not something that passed through it.
    if (scene.inside_core(scene.settled[i])) continue;
    if (scene.inside_core(scene.points[i])) ++count;
  }
  return count;
}

f32 rms_offset_mm(const Scene& scene) noexcept {
  f64 sum = 0.0;
  for (u32 i = 0; i < scene.vertex_count; ++i)
    sum += static_cast<f64>(length_squared(scene.points[i] - scene.settled[i]));
  return static_cast<f32>(1000.0 * std::sqrt(sum / static_cast<f64>(scene.vertex_count)));
}

// --- reporting ------------------------------------------------------------------------------

// A benchmark body runs once per timed iteration and again for the calibration and warmup
// passes, and the fixture is deterministic, so the JSON line would otherwise be printed nine
// times per configuration. One line per key is what a reader wants.
// `tag` keeps the three benchmarks apart: they share configuration keys, and without it the
// first one to run would silence the others.
bool report_once(i64 key, u32 tag) {
  const i64 id = key * 8 + static_cast<i64>(tag);
  static i64 seen[128];
  static u32 count = 0;
  for (u32 i = 0; i < count; ++i)
    if (seen[i] == id) return false;
  ENGINE_VERIFY(count < 128, "e19: more configurations than the report table holds");
  seen[count++] = id;
  return true;
}

void print_fixture(const char* what, const Scene& scene, const Config& config) {
  std::printf(
      "# e19 %s: build=%s mode=%s elements=%u edges=%u tets=%u faces=%u workers=%u iterations=%u "
      "sub_steps=%u solve_width=%u attach_rigid=%u attach_spring=%u mass=%.1fkg cell=%.4fm "
      "volume_compliance=%.3e max_strain=%.2f\n",
      what, ENGINE_DEBUG ? "debug" : "release", mode_name(config.mode), scene.vertex_count,
      static_cast<u32>(scene.lattice.edges.size()), static_cast<u32>(scene.lattice.volumes.size()),
      static_cast<u32>(scene.lattice.faces.size() / 3), config.workers, config.iterations,
      config.sub_steps, soft_body_solve_width(scene.vertex_count), scene.rigid_attachments,
      scene.spring_attachments, static_cast<f64>(scene.total_mass), static_cast<f64>(scene.spacing),
      static_cast<f64>(scene.volume_compliance), static_cast<f64>(scene.max_strain));
}

// --- the cost grid --------------------------------------------------------------------------

// Bring the fixture to the state whose tick cost the budget is written against: the cage held
// at 70% of its rest height with its full contact set, the core inside it, and the plate on
// top. A falling or resting cage touches less and measures less.
void press_and_hold(Scene& scene) {
  scene.settle();
  const Extent extent = vertical_extent(std::span<const Vec3>(scene.points));
  const f32 target = extent.min + 0.70f * scene.rest_height + k_plate_half;
  scene.place_plate(extent.max + k_plate_half + 0.005f);
  const f32 travel = (scene.plate_y - target) / static_cast<f32>(press_steps());
  for (u32 i = 0; i < press_steps(); ++i) {
    scene.move_plate(scene.plate_y - travel);
    scene.step();
  }
  for (u32 i = 0; i < hold_steps(); ++i) {
    scene.move_plate(target);
    scene.step();
  }
}

#if ENGINE_DEBUG
// The debug grid only has to prove the harness runs; the release grid is the experiment.
#define ENGINE_E19_TICK_ARGS e19_key(343, 1, 4, 1), e19_key(729, 4, 16, 2)
#else
// The 216-particle rows are ADR-0029's default T0 cage — at most one 256-vertex solve group — and
// the three `Unfixed` rows at the end are the same cages without the strain clamp, which is what
// makes the clamp's cost a subtraction rather than an estimate.
#define ENGINE_E19_TICK_ARGS                                                                       \
  e19_key(216, 1, 8, 1), e19_key(216, 1, 8, 2), e19_key(216, 4, 8, 1), e19_key(216, 4, 8, 2),      \
      e19_key(216, 8, 8, 1), e19_key(216, 8, 8, 2), e19_key(216, 1, 8, 1, 1),                      \
      e19_key(343, 1, 8, 1, 1), e19_key(512, 1, 8, 1, 1), e19_key(729, 1, 8, 1, 1),                \
      e19_key(343, 1, 4, 1), e19_key(343, 1, 4, 2), e19_key(343, 1, 8, 1), e19_key(343, 1, 8, 2),  \
      e19_key(343, 1, 16, 1), e19_key(343, 1, 16, 2), e19_key(343, 4, 4, 1),                       \
      e19_key(343, 4, 4, 2), e19_key(343, 4, 8, 1), e19_key(343, 4, 8, 2), e19_key(343, 4, 16, 1), \
      e19_key(343, 4, 16, 2), e19_key(343, 8, 4, 1), e19_key(343, 8, 4, 2), e19_key(343, 8, 8, 1), \
      e19_key(343, 8, 8, 2), e19_key(343, 8, 16, 1), e19_key(343, 8, 16, 2),                       \
      e19_key(512, 1, 4, 1), e19_key(512, 1, 4, 2), e19_key(512, 1, 8, 1), e19_key(512, 1, 8, 2),  \
      e19_key(512, 1, 16, 1), e19_key(512, 1, 16, 2), e19_key(512, 4, 4, 1),                       \
      e19_key(512, 4, 4, 2), e19_key(512, 4, 8, 1), e19_key(512, 4, 8, 2), e19_key(512, 4, 16, 1), \
      e19_key(512, 4, 16, 2), e19_key(512, 8, 4, 1), e19_key(512, 8, 4, 2), e19_key(512, 8, 8, 1), \
      e19_key(512, 8, 8, 2), e19_key(512, 8, 16, 1), e19_key(512, 8, 16, 2),                       \
      e19_key(729, 1, 4, 1), e19_key(729, 1, 4, 2), e19_key(729, 1, 8, 1), e19_key(729, 1, 8, 2),  \
      e19_key(729, 1, 16, 1), e19_key(729, 1, 16, 2), e19_key(729, 4, 4, 1),                       \
      e19_key(729, 4, 4, 2), e19_key(729, 4, 8, 1), e19_key(729, 4, 8, 2), e19_key(729, 4, 16, 1), \
      e19_key(729, 4, 16, 2), e19_key(729, 8, 4, 1), e19_key(729, 8, 4, 2), e19_key(729, 8, 8, 1), \
      e19_key(729, 8, 8, 2), e19_key(729, 8, 16, 1), e19_key(729, 8, 16, 2)
#endif

}  // namespace

ENGINE_BENCH_ARGS(physics_e19_tick, "physics.e19.tick", ENGINE_E19_TICK_ARGS) {
  const Config config = decode(state.arg());
  jobs::JobSystemConfig job_config;
  job_config.performance_workers = config.workers;
  jobs::JobSystem job_system(job_config);

  Scene scene;
  scene.build(config, &job_system);
  press_and_hold(scene);
  if (report_once(state.arg(), 1)) print_fixture("tick", scene, config);

  while (state.keep_running())
    scene.step();
  state.set_items(scene.vertex_count);
}

// --- the press cycle ------------------------------------------------------------------------

namespace {

struct PressResult {
  f32 rest_height = 0.0f;
  f32 pressed_ratio = 0.0f;
  f32 peak_strain = 0.0f;
  f32 held_strain = 0.0f;
  f32 height_ratio_plus_1s = 0.0f;
  f64 volume_min_load = 0.0;
  f64 volume_at_release = 0.0;
  f64 volume_plus_1s = 0.0;
  f32 recovery_ms = -1.0f;
  f32 rms_mm = 0.0f;
  u32 interpenetration_peak = 0;
  u32 interpenetration_ticks = 0;
  f64 hold_us_per_tick = 0.0;
  bool diverged = false;
  f32 diverged_ms = -1.0f;
};

PressResult run_press_cycle(Scene& scene) {
  PressResult result;
  scene.settle();
  result.rest_height = scene.rest_height;
  const Extent extent = vertical_extent(std::span<const Vec3>(scene.points));
  const f32 floor = extent.min;
  const f32 target = floor + 0.70f * scene.rest_height + k_plate_half;

  result.volume_min_load = 1.0;
  // A cage whose cells invert under the press can leave the solver with no way back, and what
  // comes out the far side is not a large number but a NaN. Detecting it and freezing the
  // metrics at the last finite pose is the difference between a row that says "diverged at
  // 0.68 s" and a row full of `nan` that a reader has to work out for themselves.
  u32 sample_tick = 0;
  const auto sample = [&scene, &result, &sample_tick]() {
    ++sample_tick;
    if (result.diverged) return false;
    scene.read();
    for (const Vec3& p : scene.points) {
      // Not only "is it a number". A cage can leave the fixture at a hundred million metres and
      // still be finite — 729 elements at sixteen iterations and two sub-steps did exactly that
      // while the clamp's velocity correction was at the wrong time scale — and a row of finite
      // nonsense flagged `diverged: false` is worse than one flagged `nan`. The fixture is a
      // 0.4 m cube on a 4 m ground plane, so anything past a kilometre has left.
      if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
          length_squared(p) < 1.0e6f) {
        continue;
      }
      result.diverged = true;
      result.diverged_ms = 1000.0f * static_cast<f32>(sample_tick) * scene.dt;
      return false;
    }
    const f32 strain = max_stretch(scene);
    if (strain > result.peak_strain) result.peak_strain = strain;
    const u32 inside = interpenetration_count(scene);
    if (inside > result.interpenetration_peak) result.interpenetration_peak = inside;
    if (inside > 0) ++result.interpenetration_ticks;
    return true;
  };

  // Press: 30% of the cage's depth over half a second.
  scene.place_plate(extent.max + k_plate_half + 0.005f);
  const f32 travel = (scene.plate_y - target) / static_cast<f32>(press_steps());
  for (u32 i = 0; i < press_steps(); ++i) {
    scene.move_plate(scene.plate_y - travel);
    scene.step();
    if (!sample()) continue;
    const f64 ratio = surface_volume(std::span<const Vec3>(scene.points),
                                     std::span<const u32>(scene.lattice.faces)) /
                      scene.rest_volume;
    if (ratio < result.volume_min_load) result.volume_min_load = ratio;
  }

  // Hold, and time it. Only the step is inside the clock: reading the particles back and
  // walking 3,258 edges for the strain costs more than the step does, and timing that would be
  // measuring the harness. The bench harness's own `physics.e19.tick` is the authority on cost
  // either way; this number is here so that a single scenario run carries its own.
  i64 hold_ns = 0;
  for (u32 i = 0; i < hold_steps(); ++i) {
    scene.move_plate(target);
    const i64 step_start = time::monotonic_ns();
    scene.step();
    hold_ns += time::monotonic_ns() - step_start;
    // ADR-0029's budget, over the phase it is written against.
    scene.sample_budget();
    if (!sample()) continue;
    const f64 ratio = surface_volume(std::span<const Vec3>(scene.points),
                                     std::span<const u32>(scene.lattice.faces)) /
                      scene.rest_volume;
    if (ratio < result.volume_min_load) result.volume_min_load = ratio;
  }
  result.hold_us_per_tick = static_cast<f64>(hold_ns) / 1000.0 / static_cast<f64>(hold_steps());
  if (!result.diverged) {
    result.pressed_ratio =
        vertical_extent(std::span<const Vec3>(scene.points)).size() / scene.rest_height;
    // Strain in the held pose, as against the peak over the whole run: the peak is a transient
    // of the press and the held value is what the material is actually asked to carry.
    result.held_strain = max_stretch(scene);
  }

  // Release: lift the plate clear. The recovery clock starts here, because this is when the
  // load comes off — measuring from the end of the lift would credit the cage with the 0.33 s
  // it spends recovering while the plate is still moving away.
  u32 since_release = 0;
  const auto watch_recovery = [&scene, &result, &since_release]() {
    ++since_release;
    if (result.recovery_ms < 0.0f &&
        vertical_extent(std::span<const Vec3>(scene.points)).size() >= 0.95f * scene.rest_height) {
      result.recovery_ms = 1000.0f * static_cast<f32>(since_release) * scene.dt;
    }
  };
  for (u32 i = 0; i < lift_steps(); ++i) {
    scene.move_plate(scene.plate_y + 0.05f);
    scene.step();
    if (!sample()) continue;
    watch_recovery();
  }
  if (!result.diverged) {
    result.volume_at_release = surface_volume(std::span<const Vec3>(scene.points),
                                              std::span<const u32>(scene.lattice.faces)) /
                               scene.rest_volume;
  }

  // Recover, watching for 95% of the rest height and for the shape one second later.
  for (u32 i = 0; i < recover_steps(); ++i) {
    scene.step();
    if (!sample()) continue;
    watch_recovery();
    if (i + 1 == one_second()) {
      result.volume_plus_1s = surface_volume(std::span<const Vec3>(scene.points),
                                             std::span<const u32>(scene.lattice.faces)) /
                              scene.rest_volume;
      result.rms_mm = rms_offset_mm(scene);
      result.height_ratio_plus_1s =
          vertical_extent(std::span<const Vec3>(scene.points)).size() / scene.rest_height;
    }
  }
  return result;
}

void print_press(i64 key, const Config& config, const Scene& scene, const PressResult& r) {
  std::printf(
      "{\"experiment\":\"e19\",\"case\":\"press\",\"key\":%lld,\"build\":\"%s\",\"mode\":\"%s\","
      "\"elements\":%u,\"edges\":%u,\"tets\":%u,\"solve_width\":%u,\"workers\":%u,"
      "\"iterations\":%u,\"sub_steps\":%u,\"attach_rigid\":%u,\"attach_spring\":%u,"
      "\"mass_kg\":%.1f,\"cell_m\":%.5f,\"volume_compliance\":%.4e,\"max_strain\":%.2f,"
      "\"rest_height_m\":%.5f,\"pressed_height_ratio\":%.4f,"
      "\"peak_strain\":%.4f,\"held_strain\":%.4f,\"volume_min_under_load\":%.4f,"
      "\"volume_at_release\":%.4f,\"volume_plus_1s\":%.4f,\"height_ratio_plus_1s\":%.4f,"
      "\"recovery_ms\":%.1f,\"rms_mm_plus_1s\":%.4f,"
      "\"interpenetration_peak\":%u,\"interpenetration_ticks\":%u,\"hold_us_per_tick\":%.1f,"
      "\"budget_ms_mean\":%.4f,\"budget_ms_peak\":%.4f,"
      "\"clamp_sweeps_mean\":%.2f,\"clamp_sweeps_peak\":%u,\"clamp_saturated_ticks\":%u,"
      "\"diverged\":%s,\"diverged_ms\":%.1f}\n",
      static_cast<long long>(key), ENGINE_DEBUG ? "debug" : "release", mode_name(config.mode),
      scene.vertex_count, static_cast<u32>(scene.lattice.edges.size()),
      static_cast<u32>(scene.lattice.volumes.size()), soft_body_solve_width(scene.vertex_count),
      config.workers, config.iterations, config.sub_steps, scene.rigid_attachments,
      scene.spring_attachments, static_cast<f64>(scene.total_mass), static_cast<f64>(scene.spacing),
      static_cast<f64>(scene.volume_compliance), static_cast<f64>(scene.max_strain),
      static_cast<f64>(r.rest_height), static_cast<f64>(r.pressed_ratio),
      static_cast<f64>(r.peak_strain), static_cast<f64>(r.held_strain), r.volume_min_load,
      r.volume_at_release, r.volume_plus_1s, static_cast<f64>(r.height_ratio_plus_1s),
      static_cast<f64>(r.recovery_ms), static_cast<f64>(r.rms_mm), r.interpenetration_peak,
      r.interpenetration_ticks, r.hold_us_per_tick, scene.budget_ms_mean(), scene.budget_ms_peak,
      scene.clamp_sweeps_mean(), scene.clamp_sweeps_peak, scene.clamp_saturated_ticks,
      r.diverged ? "true" : "false", static_cast<f64>(r.diverged_ms));
}

#if ENGINE_DEBUG
#define ENGINE_E19_PRESS_ARGS e19_key(343, 4, 8, 1)
#else
// The physics sweep. Worker count is fixed at 8 because the world is deterministic in it: the
// cage's shape does not depend on how many ways the solve was split (the soft-body parallel
// test asserts it), so sweeping workers here would measure nothing the cost grid does not.
//
// The four `Unfixed` keys at the end are the control: the two configurations that latched at 512
// particles, the one that diverged at 343, and the one that latched at 729, run in the same
// session as the fixed grid so that "the fixes did this" is a comparison and not a claim.
#define ENGINE_E19_PRESS_ARGS                                                                      \
  e19_key(216, 8, 8, 1), e19_key(216, 8, 8, 2), e19_key(343, 8, 4, 1), e19_key(343, 8, 4, 2),      \
      e19_key(343, 8, 8, 1), e19_key(343, 8, 8, 2), e19_key(343, 8, 16, 1),                        \
      e19_key(343, 8, 16, 2), e19_key(512, 8, 4, 1), e19_key(512, 8, 4, 2), e19_key(512, 8, 8, 1), \
      e19_key(512, 8, 8, 2), e19_key(512, 8, 16, 1), e19_key(512, 8, 16, 2),                       \
      e19_key(729, 8, 4, 1), e19_key(729, 8, 4, 2), e19_key(729, 8, 8, 1), e19_key(729, 8, 8, 2),  \
      e19_key(729, 8, 16, 1), e19_key(729, 8, 16, 2), e19_key(343, 8, 16, 2, 1),                   \
      e19_key(512, 8, 8, 1, 1), e19_key(512, 8, 8, 2, 1), e19_key(729, 8, 8, 2, 1),                \
      e19_key(343, 8, 16, 2, 2), e19_key(343, 8, 16, 2, 3), e19_key(512, 8, 16, 2, 2),             \
      e19_key(512, 8, 16, 2, 3), e19_key(729, 8, 16, 2, 2), e19_key(729, 8, 16, 2, 3),             \
      e19_key(512, 8, 16, 2, 1), e19_key(729, 8, 16, 2, 1)
#endif

}  // namespace

ENGINE_BENCH_ARGS(physics_e19_press, "physics.e19.press", ENGINE_E19_PRESS_ARGS) {
  const Config config = decode(state.arg());
  jobs::JobSystemConfig job_config;
  job_config.performance_workers = config.workers;
  jobs::JobSystem job_system(job_config);

  while (state.keep_running()) {
    Scene scene;
    scene.build(config, &job_system);
    const PressResult result = run_press_cycle(scene);
    bench::keep(result.peak_strain);
    if (report_once(state.arg(), 2)) {
      print_fixture("press", scene, config);
      print_press(state.arg(), config, scene, result);
    }
  }
  state.set_items(press_steps() + hold_steps() + lift_steps() + recover_steps());
}

// --- sustained load (E22's shape) -------------------------------------------------------------

namespace {

u32 sustained_steps() { return scaled(1200); }  // 20 s at 60 Hz

struct SustainedResult {
  f32 height_at_2s = 0.0f;
  f32 height_at_end = 0.0f;
  f32 creep_mm = 0.0f;
  f32 oscillation_mm = 0.0f;
  f32 speed_mm_s_at_2s = 0.0f;
  f32 speed_mm_s_at_end = 0.0f;
  f32 plane_penetration_mm = 0.0f;
  f64 volume_at_end = 0.0;
  f64 us_per_tick = 0.0;
};

SustainedResult run_sustained(Scene& scene, f32 load_ratio) {
  SustainedResult result;
  scene.settle();

  // A rigid block of `load_ratio` times the cage's own mass, dropped onto it from just above.
  ShapeId shape;
  ENGINE_VERIFY(scene.world.create_box(Vec3(0.18f, 0.09f, 0.18f), shape) == Status::Ok,
                "e19: load shape");
  BodyDesc desc;
  desc.shape = shape;
  const Extent extent = vertical_extent(std::span<const Vec3>(scene.points));
  desc.transform.position = Vec3(0.0f, extent.max + 0.10f, 0.0f);
  desc.motion = MotionType::Dynamic;
  desc.layer = Layer::Moving;
  desc.mass = load_ratio * scene.total_mass;
  desc.friction = 0.6f;
  desc.allow_sleeping = false;
  BodyId load;
  ENGINE_VERIFY(scene.world.create_body(desc, load) == Status::Ok, "e19: load body");

  Vector<Vec3> previous(scene.vertex_count);
  const auto speed_mm_s = [&scene, &previous]() {
    f32 worst = 0.0f;
    for (u32 i = 0; i < scene.vertex_count; ++i) {
      const f32 v = length(scene.points[i] - previous[i]) / scene.dt;
      if (v > worst) worst = v;
    }
    return 1000.0f * worst;
  };

  f32 min_height_after_2s = 1.0e30f;
  f32 max_height_after_2s = 0.0f;
  const u32 settle_under_load = one_second() * 2;
  const i64 start = time::monotonic_ns();
  for (u32 i = 0; i < sustained_steps(); ++i) {
    previous.clear();
    previous.append(std::span<const Vec3>(scene.points));
    scene.step();
    scene.sample_budget();
    scene.read();
    const Extent now = vertical_extent(std::span<const Vec3>(scene.points));
    if (now.min < -result.plane_penetration_mm * 0.001f)
      result.plane_penetration_mm = -1000.0f * now.min;
    if (i + 1 == settle_under_load) {
      result.height_at_2s = now.size();
      result.speed_mm_s_at_2s = speed_mm_s();
    }
    if (i + 1 > settle_under_load) {
      if (now.size() < min_height_after_2s) min_height_after_2s = now.size();
      if (now.size() > max_height_after_2s) max_height_after_2s = now.size();
    }
    if (i + 1 == sustained_steps()) {
      result.height_at_end = now.size();
      result.speed_mm_s_at_end = speed_mm_s();
      result.volume_at_end = surface_volume(std::span<const Vec3>(scene.points),
                                            std::span<const u32>(scene.lattice.faces)) /
                             scene.rest_volume;
    }
  }
  result.us_per_tick =
      static_cast<f64>(time::monotonic_ns() - start) / 1000.0 / static_cast<f64>(sustained_steps());
  result.creep_mm = 1000.0f * (result.height_at_2s - result.height_at_end);
  result.oscillation_mm = 1000.0f * (max_height_after_2s - min_height_after_2s);
  return result;
}

void print_sustained(const Config& config, const Scene& scene, f32 load_ratio,
                     const SustainedResult& r) {
  std::printf(
      "{\"experiment\":\"e19\",\"case\":\"sustained\",\"build\":\"%s\",\"mode\":\"%s\","
      "\"elements\":%u,"
      "\"workers\":%u,\"iterations\":%u,\"sub_steps\":%u,\"seconds\":%.1f,\"load_ratio\":%.1f,"
      "\"load_kg\":%.1f,\"max_strain\":%.2f,\"volume_compliance\":%.4e,"
      "\"height_at_2s_m\":%.5f,\"height_at_end_m\":%.5f,\"creep_mm\":%.3f,"
      "\"oscillation_mm\":%.3f,\"speed_mm_s_at_2s\":%.3f,\"speed_mm_s_at_end\":%.3f,"
      "\"plane_penetration_mm\":%.3f,\"volume_at_end\":%.4f,\"us_per_tick\":%.1f,"
      "\"budget_ms_mean\":%.4f,\"budget_ms_peak\":%.4f}\n",
      ENGINE_DEBUG ? "debug" : "release", mode_name(config.mode), scene.vertex_count,
      config.workers, config.iterations, config.sub_steps,
      static_cast<f64>(sustained_steps()) * static_cast<f64>(scene.dt),
      static_cast<f64>(load_ratio), static_cast<f64>(load_ratio * scene.total_mass),
      static_cast<f64>(scene.max_strain), static_cast<f64>(scene.volume_compliance),
      static_cast<f64>(r.height_at_2s), static_cast<f64>(r.height_at_end),
      static_cast<f64>(r.creep_mm), static_cast<f64>(r.oscillation_mm),
      static_cast<f64>(r.speed_mm_s_at_2s), static_cast<f64>(r.speed_mm_s_at_end),
      static_cast<f64>(r.plane_penetration_mm), r.volume_at_end, r.us_per_tick,
      scene.budget_ms_mean(), scene.budget_ms_peak);
}

}  // namespace

// E22's scenario at E19's fixture: the cage carrying a rigid body of 1x and of 5x its own mass
// for twenty seconds. It shares the cage, the core, and the ground with the press case, so it
// is early data for E22 rather than E22 itself — that row wants sixty seconds and a cushion
// without a rigid core in it.
ENGINE_BENCH_ARGS(physics_e19_sustained, "physics.e19.sustained", 1, 5) {
  const f32 load_ratio = static_cast<f32>(state.arg());
  Config config;
  config.elements = 512;
  config.workers = 8;
  config.iterations = 8;
  config.sub_steps = 1;

  jobs::JobSystemConfig job_config;
  job_config.performance_workers = config.workers;
  jobs::JobSystem job_system(job_config);

  while (state.keep_running()) {
    Scene scene;
    scene.build(config, &job_system);
    const SustainedResult result = run_sustained(scene, load_ratio);
    bench::keep(result.creep_mm);
    if (report_once(state.arg(), 3)) {
      print_fixture("sustained", scene, config);
      print_sustained(config, scene, load_ratio, result);
    }
  }
  state.set_items(sustained_steps());
}
