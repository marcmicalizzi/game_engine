// A soft body from a tissue region (domain/tissue/soft_body.h; docs/subsystems/tissue.md, "A soft
// body from a runtime region"): the synthetic runtime slab and a derived cage built as the physics
// module's soft body — the declared energy as XPBD constraints, the mass lumped, the fixed
// attachments held, the sliding ones in contact, the frame a static target — settled at the fixed
// step under a state's load, and the same bytes twice.
#include "block_fixture.h"

#include <domain/tissue/cage.h>
#include <domain/tissue/soft_body.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

template <class T>
Vector<T> read(const TissueFile& file, const std::string& name) {
  Vector<T> out;
  const TissueBlock* block = file.find(name);
  if (block == nullptr) return out;
  out.resize(static_cast<u32>(block->bytes.size() / sizeof(T)));
  std::memcpy(out.data(), block->bytes.data(), block->bytes.size());
  return out;
}

f64 length(Vec3 a, Vec3 b) {
  const f64 x = static_cast<f64>(a.x) - static_cast<f64>(b.x);
  const f64 y = static_cast<f64>(a.y) - static_cast<f64>(b.y);
  const f64 z = static_cast<f64>(a.z) - static_cast<f64>(b.z);
  return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

TEST_CASE("soft body: the synthetic runtime slab, as the physics module's soft body") {
  const SyntheticTissue synthetic = make_synthetic_tissue();
  SoftBodyOptions options;
  options.load_state = "pressed";
  TissueSoftBody body;
  std::string error;
  REQUIRE_MESSAGE(build_soft_body(synthetic.file, options, body, &error), error);
  CHECK(body.region == "slab");
  CHECK(body.rest_state == "rest");
  CHECK(body.rest.size() == 147);
  CHECK(body.volumes.size() == 432);
  // Every edge once: a 7 x 7 x 3 grid of Kuhn cells.
  CHECK(body.edges.size() > 432);
  for (const physics::SoftEdge& e : body.edges) {
    CHECK(e.a < e.b);
    CHECK(e.compliance > 0.0f);
    CHECK(std::isfinite(e.compliance));
  }
  for (const physics::SoftVolumeConstraint& v : body.volumes)
    CHECK(v.compliance > 0.0f);
  // The mass: density times volume at the construction, every gram of it on a node.
  f64 lumped = 0.0;
  for (const f32 w : body.inverse_masses) {
    REQUIRE(w > 0.0f);
    lumped += 1.0 / static_cast<f64>(w);
  }
  CHECK(lumped == doctest::Approx(body.mass_kg).epsilon(1e-6));
  // The rim is held (fixed to the box), the posterior slides on the support: two targets, both in
  // contact, the support turned so the slab is in front of it.
  const Vector<u32> rim = read<u32>(synthetic.file, "slab.rim");
  CHECK(body.anchors.size() == rim.size());
  for (const TissueSoftBody::Anchor& a : body.anchors)
    CHECK(a.kind == physics::AttachmentKind::Rigid);
  REQUIRE(body.targets.size() == 2);
  CHECK(body.targets[0].name == "box");
  CHECK(body.targets[0].frame);
  CHECK(body.targets[1].name == "support");
  CHECK_FALSE(body.targets[1].frame);
  CHECK(body.targets[1].contact);
  // The pressed state's load, less the air's buoyancy at the slab's density.
  CHECK(body.gravity.y > 9.79f);
  CHECK(body.gravity.y < 9.81f);
  CHECK(!body.notes.empty());  // the membrane, the cables, the rim as a spring held rigid
}

TEST_CASE("soft body: settles under the state's load, holds its rim, and gives the same bytes") {
  const SyntheticTissue synthetic = make_synthetic_tissue();
  SoftBodyOptions options;
  options.load_state = "pressed";
  SettleResult first;
  std::string error;
  REQUIRE_MESSAGE(settle_soft_body(synthetic.file, options, first, &error), error);
  CHECK(first.settled);
  CHECK(first.steps < options.max_steps);
  CHECK(first.inverted == 0);
  REQUIRE(first.nodes.size() == 147);
  for (const Vec3& p : first.nodes)
    CHECK((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
  // The rim is where its frame holds it: the construction, the box having no frame state.
  const Vector<Vec3> construction = read<Vec3>(synthetic.file, "slab.nodes");
  for (const u32 v : read<u32>(synthetic.file, "slab.rim"))
    CHECK(length(first.nodes[v], construction[v]) < 1e-6);
  // Settling moved it: the load is gravity, and the slab sags along it.
  const Vector<Vec3> rest = read<Vec3>(synthetic.file, "state.rest");
  f64 moved = 0.0;
  for (u32 i = 0; i < rest.size(); ++i)
    moved = std::max(moved, length(first.nodes[i], rest[i]));
  CHECK(moved > 1e-4);
  MESSAGE("synthetic slab under 'pressed': " << first.steps << " steps, " << first.wall_ms
                                             << " ms, moved up to " << moved * 1e3 << " mm");
  // A second run from scratch is the same bytes.
  SettleResult second;
  REQUIRE(settle_soft_body(synthetic.file, options, second, &error));
  CHECK(second.steps == first.steps);
  REQUIRE(second.nodes.size() == first.nodes.size());
  CHECK(std::memcmp(second.nodes.data(), first.nodes.data(), first.nodes.size() * sizeof(Vec3)) ==
        0);
  // The JSON line the command prints.
  const JsonValue line = settle_json(synthetic.file, options, first);
  CHECK(line.find("distance_mm") != nullptr);
  CHECK(line.find("largest") != nullptr);
}

TEST_CASE("soft body: a derived cage on its floor, held at its ends") {
  const block::Block reference = block::make_block();
  CageOptions cage_options;
  cage_options.node_budget = 100;
  TissueFile cage;
  CageSummary summary;
  REQUIRE(derive_cage(reference.file, cage_options, cage, summary));
  SoftBodyOptions options;
  options.load_state = "pressed";
  SettleResult result;
  std::string error;
  REQUIRE_MESSAGE(settle_soft_body(cage, options, result, &error), error);
  CHECK(result.settled);
  CHECK(result.inverted == 0);
  // The floor is a surface the floor nodes slide on: the contact holds them on it, one-sided,
  // against a gravity that presses them into it.
  const Vector<u32> floor = read<u32>(cage, cage.definition.regions.front().node_sets[1].nodes);
  REQUIRE(!floor.empty());
  for (const u32 v : floor)
    CHECK(result.nodes[v].z > -1e-4f);
  // And the settled block is measured against the carried pressed state.
  f64 largest = 0.0;
  for (const f64 d : result.distance_m)
    largest = std::max(largest, d);
  CHECK(largest > 0.0);
  MESSAGE("block cage under 'pressed': " << result.steps << " steps, largest " << largest * 1e3
                                         << " mm from the carried state; final speed "
                                         << result.final_speed_m_s << " m/s, clamp sweeps "
                                         << result.strain_clamp_sweeps_max);
}

TEST_CASE("soft body: what it is built from, and what it refuses") {
  SyntheticOptions quadratic;
  quadratic.quadratic = true;
  const SyntheticTissue reference = make_synthetic_tissue(quadratic);
  TissueSoftBody body;
  std::string error;
  // Ten-node cells are a reference body, not a cage.
  CHECK_FALSE(build_soft_body(reference.file, SoftBodyOptions{}, body, &error));
  CHECK(error.find("Runtime") != std::string::npos);
  const SyntheticTissue synthetic = make_synthetic_tissue();
  SoftBodyOptions options;
  options.load_state = "no such state";
  CHECK_FALSE(build_soft_body(synthetic.file, options, body, &error));
  SettleResult result;
  CHECK_FALSE(settle_soft_body(synthetic.file, SoftBodyOptions{}, result, &error));
  CHECK(error.find("load") != std::string::npos);
  // An attachment to a bone is one the body cannot follow yet.
  TissueFile boned = synthetic.file;
  boned.definition.attachments[0].target_kind = TargetKind::Bone;
  boned.definition.attachments[0].target = "spine";
  options.load_state = "pressed";
  CHECK_FALSE(build_soft_body(boned, options, body, &error));
  CHECK(error.find("bone") != std::string::npos);
}
