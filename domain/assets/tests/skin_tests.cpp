// Skins and animations out of a glTF file (docs/plan/05-simulation.md §5.11, §7.2): the joint
// hierarchy, the per-vertex influences, the keyframe curves, and the one rule that separates a
// skinned import from a rigid one — a skinned primitive stays in **bind space**, because glTF
// places it through its joints and baking the mesh node's transform in would place it twice.
//
// The fixture (`skinned_glb.h`) is a two-bone bar written at test time; every number below is
// what that writer put in the file.
#include "skinned_glb.h"

#include <domain/assets/gltf.h>
#include <domain/geometry/cluster.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::assets;

// One unguessable scratch directory per object, so a second copy of this binary cannot delete
// the .glb this one just wrote (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

namespace {

i32 node_named(const MeshData& mesh, const char* name) {
  for (u32 i = 0; i < mesh.nodes.size(); ++i) {
    if (mesh.nodes[i].name == name) return static_cast<i32>(i);
  }
  return -1;
}

}  // namespace

TEST_CASE("gltf skins: a skinned primitive stays in bind space with its node transform recorded") {
  TempDir tmp("engine_assets_skin_tests");
  const std::string path = tmp.file("bar.glb");
  REQUIRE(test_fixture::write_skinned_glb(path));

  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf(path, mesh, &error), error);

  // The bind-space rule. The mesh node sits at x = +5 and the vertices do not move with it: they
  // are where the accessor put them, because the joints are what place a skinned mesh. A rigid
  // primitive under the same node would have been flattened.
  REQUIRE(mesh.positions.size() == 6);
  const Vec3* expected = test_fixture::bar_positions();
  for (u32 v = 0; v < 6; ++v) {
    CHECK(mesh.positions[v].x == doctest::Approx(expected[v].x));
    CHECK(mesh.positions[v].y == doctest::Approx(expected[v].y));
    CHECK(mesh.positions[v].z == doctest::Approx(expected[v].z));
  }
  REQUIRE(mesh.primitives.size() == 1);
  CHECK(mesh.primitives[0].skin == 0);
  CHECK(mesh.primitives[0].material == 0);
  // The transform that was not applied is the one thing the import would otherwise lose, so it
  // is recorded on the skin by the node that instanced it.
  REQUIRE(mesh.skins.size() == 1);
  CHECK(mesh.skins[0].name == "bar_skin");
  CHECK(mesh.skins[0].node_transform.c[3].x == doctest::Approx(5.0f));
  CHECK(mesh.skins[0].node_transform.c[3].y == doctest::Approx(0.0f));

  // The hierarchy, reordered so that a parent always comes before its child. The file writes the
  // mesh node before the deepest joint on purpose, so the reordering has something to do.
  REQUIRE(mesh.nodes.size() == 4);
  const i32 rig = node_named(mesh, "rig");
  const i32 root_joint = node_named(mesh, "joint_root");
  const i32 tip_joint = node_named(mesh, "joint_tip");
  const i32 bar = node_named(mesh, "bar");
  REQUIRE(rig >= 0);
  REQUIRE(root_joint >= 0);
  REQUIRE(tip_joint >= 0);
  REQUIRE(bar >= 0);
  CHECK(mesh.nodes[static_cast<u32>(rig)].parent == -1);
  CHECK(mesh.nodes[static_cast<u32>(root_joint)].parent == rig);
  CHECK(mesh.nodes[static_cast<u32>(tip_joint)].parent == root_joint);
  CHECK(mesh.nodes[static_cast<u32>(bar)].parent == rig);
  for (u32 i = 0; i < mesh.nodes.size(); ++i)
    CHECK(mesh.nodes[i].parent < static_cast<i32>(i));  // one forward pass composes the skeleton
  CHECK(tip_joint > bar);  // the file's order was not parent-before-child, and now it is
  // A joint's node transform is its bind pose, kept as a TRS so a blend never has to decompose.
  CHECK(mesh.nodes[static_cast<u32>(tip_joint)].local.position.y == doctest::Approx(1.0f));
  CHECK(mesh.nodes[static_cast<u32>(bar)].local.position.x == doctest::Approx(5.0f));

  // The joint palette, and the inverse bind matrices that take a point into a joint's own space.
  REQUIRE(mesh.skins[0].joints.size() == 2);
  CHECK(mesh.skins[0].joints[0] == root_joint);
  CHECK(mesh.skins[0].joints[1] == tip_joint);
  CHECK(mesh.skins[0].skeleton_root == root_joint);
  REQUIRE(mesh.skins[0].inverse_bind.size() == 2);
  CHECK(mesh.skins[0].inverse_bind[0].c[3].y == doctest::Approx(0.0f));
  CHECK(mesh.skins[0].inverse_bind[1].c[3].y == doctest::Approx(-1.0f));

  // The influences, quantized to weights that sum to 255. The middle row is half each, and the
  // rounding lands on the first influence, which is where make_skin_binding puts it.
  REQUIRE(mesh.skin_bindings.size() == 6);
  for (u32 v = 0; v < 6; ++v) {
    const geometry::SkinBinding& binding = mesh.skin_bindings[v];
    CHECK(u32{binding.weights[0]} + binding.weights[1] + binding.weights[2] + binding.weights[3] ==
          255);
    CHECK(binding.joints[0] == 0);
    CHECK(binding.joints[1] == 1);
  }
  CHECK(mesh.skin_bindings[0].weights[0] == 255);
  CHECK(mesh.skin_bindings[2].weights[0] == 127);
  CHECK(mesh.skin_bindings[2].weights[1] == 128);
  CHECK(mesh.skin_bindings[4].weights[0] == 0);
  CHECK(mesh.skin_bindings[4].weights[1] == 255);

  // The cluster builder takes the stream straight out of `attribute_source`.
  const geometry::AttributeSource attributes = attribute_source(mesh);
  CHECK(attributes.skin.size() == 6);
  CHECK(attributes.joint_count == 2);
  geometry::ClusterMesh clusters;
  REQUIRE_MESSAGE(
      geometry::build_clusters(std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                               std::span<const u32>(mesh.indices.data(), mesh.indices.size()),
                               geometry::ClusterBuildOptions{}, clusters, &error, attributes),
      error);
  CHECK(clusters.skin.size() == clusters.vertices.size());
  CHECK(clusters.skin_joint_count == 2);
  CHECK_MESSAGE(geometry::validate_clusters(
                    clusters, std::span<const u32>(mesh.indices.data(), mesh.indices.size()),
                    geometry::ClusterBuildOptions{}, &error),
                error);
}

TEST_CASE("gltf skins: both joint and weight component forms give the same bindings") {
  TempDir tmp("engine_assets_skin_components");
  MeshData reference;
  std::string error;
  const std::string plain = tmp.file("plain.glb");
  REQUIRE(test_fixture::write_skinned_glb(plain));
  REQUIRE_MESSAGE(load_gltf(plain, reference, &error), error);

  // u16 joint indices and normalized u8 weights are the other two forms exporters write; both
  // widen to the same eight bytes a vertex.
  test_fixture::SkinnedGlbOptions packed;
  packed.joints_u16 = true;
  packed.weights_u8 = true;
  const std::string path = tmp.file("packed.glb");
  REQUIRE(test_fixture::write_skinned_glb(path, packed));
  MeshData mesh;
  REQUIRE_MESSAGE(load_gltf(path, mesh, &error), error);
  REQUIRE(mesh.skin_bindings.size() == reference.skin_bindings.size());
  for (u32 v = 0; v < mesh.skin_bindings.size(); ++v) {
    for (u32 k = 0; k < 4; ++k) {
      CHECK(mesh.skin_bindings[v].joints[k] == reference.skin_bindings[v].joints[k]);
      CHECK(mesh.skin_bindings[v].weights[k] == reference.skin_bindings[v].weights[k]);
    }
  }
}

TEST_CASE("gltf animations: three sampler modes and three paths come through as written") {
  TempDir tmp("engine_assets_animation_tests");
  const std::string path = tmp.file("bar.glb");
  REQUIRE(test_fixture::write_skinned_glb(path));
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf(path, mesh, &error), error);

  REQUIRE(mesh.animations.size() == 1);
  const Animation& clip = mesh.animations[0];
  CHECK(clip.name == "wave");
  CHECK(clip.duration == doctest::Approx(1.0f));
  REQUIRE(clip.samplers.size() == 3);
  REQUIRE(clip.channels.size() == 3);
  const i32 root_joint = node_named(mesh, "joint_root");
  const i32 tip_joint = node_named(mesh, "joint_tip");

  // LINEAR rotation on the tip: two quaternions, four floats each.
  CHECK(clip.samplers[0].interpolation == k_interp_linear);
  CHECK(clip.samplers[0].components == 4);
  REQUIRE(clip.samplers[0].times.size() == 2);
  REQUIRE(clip.samplers[0].values.size() == 8);
  CHECK(clip.samplers[0].times[1] == doctest::Approx(1.0f));
  CHECK(clip.samplers[0].values[3] == doctest::Approx(1.0f));         // the identity's w
  CHECK(clip.samplers[0].values[6] == doctest::Approx(0.70710678f));  // a quarter turn about z
  CHECK(clip.channels[0].node == tip_joint);
  CHECK(clip.channels[0].path == k_path_rotation);
  CHECK(clip.channels[0].sampler == 0);

  // STEP translation on the root: the keys are held, which is what the sampler mode means, and
  // the importer reads the curve without ever evaluating it.
  CHECK(clip.samplers[1].interpolation == k_interp_step);
  CHECK(clip.samplers[1].components == 3);
  REQUIRE(clip.samplers[1].values.size() == 6);
  CHECK(clip.samplers[1].times[1] == doctest::Approx(0.5f));
  CHECK(clip.samplers[1].values[4] == doctest::Approx(2.0f));
  CHECK(clip.channels[1].node == root_joint);
  CHECK(clip.channels[1].path == k_path_translation);

  // CUBICSPLINE scale: three values per key, so six VEC3s for two keys.
  CHECK(clip.samplers[2].interpolation == k_interp_cubic);
  CHECK(clip.samplers[2].components == 3);
  REQUIRE(clip.samplers[2].times.size() == 2);
  REQUIRE(clip.samplers[2].values.size() == 18);
  CHECK(clip.samplers[2].values[3] == doctest::Approx(1.0f));   // key 0's value
  CHECK(clip.samplers[2].values[12] == doctest::Approx(2.0f));  // key 1's value
  CHECK(clip.channels[2].node == root_joint);
  CHECK(clip.channels[2].path == k_path_scale);
}

TEST_CASE("gltf skins: a broken skin fails with a sentence rather than a wrong limb") {
  TempDir tmp("engine_assets_skin_failures");
  std::string error;

  // A joint index outside the skin's palette. Clamping it would attach a limb to the wrong bone
  // and the mesh would look right until it moved, so the load fails instead.
  test_fixture::SkinnedGlbOptions bad_joint;
  bad_joint.joint_out_of_range = true;
  const std::string bad_path = tmp.file("bad_joint.glb");
  REQUIRE(test_fixture::write_skinned_glb(bad_path, bad_joint));
  MeshData mesh;
  CHECK_FALSE(load_gltf(bad_path, mesh, &error));
  CHECK(error.find("JOINTS_0") != std::string::npos);
  CHECK(mesh.positions.empty());

  // More joints than a u8 per-vertex index can name: refused with the two numbers, because the
  // per-vertex format is the whole reason for the limit.
  test_fixture::SkinnedGlbOptions wide;
  wide.extra_joints = 300;
  const std::string wide_path = tmp.file("wide.glb");
  REQUIRE(test_fixture::write_skinned_glb(wide_path, wide));
  error.clear();
  CHECK_FALSE(load_gltf(wide_path, mesh, &error));
  CHECK(error.find("302") != std::string::npos);
  CHECK(error.find("256") != std::string::npos);
  CHECK(mesh.positions.empty());
}

TEST_CASE("gltf morph targets: deltas, names, default weights, and a weights track") {
  TempDir tmp("engine_assets_morph_tests");
  const std::string path = tmp.file("bar_morph.glb");
  test_fixture::SkinnedGlbOptions options;
  options.morph_targets = true;
  REQUIRE(test_fixture::write_skinned_glb(path, options));

  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf(path, mesh, &error), error);

  // Two channels, named through `extras.targetNames`, with the mesh's default weights.
  REQUIRE(mesh.morph.size() == 2);
  CHECK(mesh.morph[0].name == "bulge");
  CHECK(mesh.morph[1].name == "taper");
  CHECK(mesh.morph[0].default_weight == doctest::Approx(0.25f));
  CHECK(mesh.morph[1].default_weight == doctest::Approx(0.0f));

  // Sparse: only the vertices the target actually moves. The bar's middle row is two vertices
  // and its top row two more, so each channel holds exactly two entries out of six.
  REQUIRE(mesh.morph[0].vertices.size() == 2);
  REQUIRE(mesh.morph[1].vertices.size() == 2);
  CHECK(mesh.morph[0].vertices[0] == 2);
  CHECK(mesh.morph[0].vertices[1] == 3);
  CHECK(mesh.morph[1].vertices[0] == 4);
  CHECK(mesh.morph[1].vertices[1] == 5);
  for (u32 i = 0; i < 2; ++i) {
    CHECK(mesh.morph[0].position_deltas[i].z == doctest::Approx(0.5f));
    CHECK(std::fabs(mesh.morph[1].position_deltas[i].x) == doctest::Approx(0.25f));
  }
  // One channel carries NORMAL deltas and the other does not, and the parallel-array rule holds
  // for both: present and the same length, or empty.
  CHECK(mesh.morph[0].normal_deltas.size() == mesh.morph[0].vertices.size());
  CHECK(mesh.morph[1].normal_deltas.empty());
  CHECK(mesh.morph[0].normal_deltas[0].y == doctest::Approx(-0.287f).epsilon(0.05));

  // The weights track: a SCALAR sampler with one value per target per key, and the channel says
  // which entry of the flattened channel array its first value drives.
  REQUIRE(mesh.animations.size() == 1);
  const Animation& clip = mesh.animations[0];
  const AnimationChannel* weights = nullptr;
  for (const AnimationChannel& channel : clip.channels) {
    if (channel.path == k_path_weights) weights = &channel;
  }
  REQUIRE(weights != nullptr);
  CHECK(weights->morph_channel == 0);
  const AnimationSampler& sampler = clip.samplers[weights->sampler];
  CHECK(sampler.components == 2);  // two targets, not two vector components
  CHECK(sampler.interpolation == k_interp_linear);
  REQUIRE(sampler.times.size() == 2);
  REQUIRE(sampler.values.size() == 4);
  CHECK(sampler.values[2] == doctest::Approx(1.0f));
  CHECK(sampler.values[3] == doctest::Approx(0.5f));

  // And the three joint tracks still come through exactly as they did.
  u32 joint_channels = 0;
  for (const AnimationChannel& channel : clip.channels)
    joint_channels += channel.path == k_path_weights ? 0u : 1u;
  CHECK(joint_channels == 3);

  // `attribute_source` hands the channels to the cluster builders beside the bindings.
  const geometry::AttributeSource source = attribute_source(mesh);
  CHECK(source.morph.size() == 2);
  CHECK(source.skin.size() == mesh.positions.size());
}

TEST_CASE("gltf morph targets: a file without them imports exactly as it did") {
  TempDir tmp("engine_assets_morph_none");
  const std::string path = tmp.file("bar.glb");
  REQUIRE(test_fixture::write_skinned_glb(path));
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf(path, mesh, &error), error);
  CHECK(mesh.morph.empty());
  CHECK(attribute_source(mesh).morph.empty());
}
