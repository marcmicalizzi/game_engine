// Animation graphs as data, stage 3 (S35; docs/subsystems/animation.md, "Graphs: layers and
// masks"): a graph's second machine as a layer over its first, by an Override or an Additive node
// with a weight and a joint mask compiled per skeleton.
//
// What stays byte-identical, and the case that holds each: an override at weight 0 is the base and
// at weight 1 with a full mask the layer; an additive of its own reference pose changes nothing; a
// masked override leaves every joint outside its mask as the base had it; one worker and eight, two
// batch sizes and two runs give the same bytes. The pin is one layered graph at three weights.
#include <core/hash/hash.h>
#include <core/ids/id128.h>
#include <core/jobs/job_system.h>
#include <domain/anim/standard_skeleton.h>
#include <domain/assets/gltf.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/world_commands.h>
#include <foundation/tunables/tunables.h>
#include <systems/animation/animation.h>
#include <systems/animation/graph.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

constexpr f32 k_dt = 1.0f / 64.0f;
constexpr u32 k_joints = anim::k_standard_joint_count;  // 23: the humanoid core

// Literal unit quaternions (x, y, z, w), so a key is the same float on every C library.
constexpr f32 k_turns[7][4] = {
    {0.0f, 0.0f, 0.25881905f, 0.96592583f},   // 30 degrees about z
    {0.17364818f, 0.0f, 0.0f, 0.98480775f},   // 20 about x
    {0.0f, 0.30070580f, 0.0f, 0.95371695f},   // 35 about y
    {0.34202015f, 0.0f, 0.0f, 0.93969262f},   // 40 about x
    {-0.25881905f, 0.0f, 0.0f, 0.96592583f},  // -30 about x
    {0.57357644f, 0.0f, 0.0f, 0.81915204f},   // 70 about x
    {0.0f, 0.0f, 0.0f, 1.0f},                 // none
};

i32 parent_of(anim::StandardJoint j) {
  using S = anim::StandardJoint;
  switch (j) {
    case S::Root: return -1;
    case S::Hips: return static_cast<i32>(S::Root);
    case S::Spine: return static_cast<i32>(S::Hips);
    case S::Chest: return static_cast<i32>(S::Spine);
    case S::UpperChest: return static_cast<i32>(S::Chest);
    case S::Neck: return static_cast<i32>(S::UpperChest);
    case S::Head: return static_cast<i32>(S::Neck);
    case S::LeftShoulder:
    case S::RightShoulder: return static_cast<i32>(S::UpperChest);
    case S::LeftUpperLeg:
    case S::RightUpperLeg: return static_cast<i32>(S::Hips);
    default: return static_cast<i32>(j) - 1;  // the rest of each arm and leg chain
  }
}

using Keys = std::initializer_list<std::pair<anim::StandardJoint, u32>>;

// A rotation clip of two keys, at 0 and 1 s: every joint of `from` at its turn at 0 (identity when
// absent) and of `to` at 1. `hips_y`: the hips' height at 1 s, a translation track, when not 0.
assets::Animation clip(const char* name, Keys from, Keys to, f32 hips_y = 0.0f) {
  assets::Animation a;
  a.name = name;
  a.duration = 1.0f;
  const auto key = [](Keys keys, u32 j, f32* out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    for (const auto& [joint, turn] : keys) {
      if (static_cast<u32>(joint) == j) std::memcpy(out, k_turns[turn], sizeof(f32) * 4);
    }
  };
  for (u32 j = 0; j < k_joints; ++j) {
    bool named = false;
    for (Keys keys : {from, to}) {
      for (const auto& kv : keys)
        named = named || static_cast<u32>(kv.first) == j;
    }
    if (!named) continue;
    assets::AnimationSampler s;
    s.components = 4;
    s.times.push_back(0.0f);
    s.times.push_back(1.0f);
    f32 q[4];
    key(from, j, q);
    for (const f32 v : q)
      s.values.push_back(v);
    key(to, j, q);
    for (const f32 v : q)
      s.values.push_back(v);
    assets::AnimationChannel c;
    c.node = static_cast<i32>(j);
    c.sampler = a.samplers.size();
    c.path = assets::k_path_rotation;
    a.samplers.push_back(s);
    a.channels.push_back(c);
  }
  if (hips_y != 0.0f) {
    assets::AnimationSampler s;
    s.components = 3;
    s.times.push_back(0.0f);
    s.times.push_back(1.0f);
    for (const f32 v : {0.0f, 0.95f, 0.0f, 0.0f, 0.95f + hips_y, 0.0f})
      s.values.push_back(v);
    assets::AnimationChannel c;
    c.node = static_cast<i32>(anim::StandardJoint::Hips);
    c.sampler = a.samplers.size();
    c.path = assets::k_path_translation;
    a.samplers.push_back(s);
    a.channels.push_back(c);
  }
  return a;
}

// The test rig: the standard skeleton's 23 roles as a humanoid, its joints named by the roles (so
// a mask by role resolves through `map_joints`), with the clips a walker's layers would play.
struct Humanoid {
  Library library;
  Id128 skeleton = Library::skeleton_id("body/body");
  Humanoid() {
    using S = anim::StandardJoint;
    assets::MeshData mesh;
    mesh.nodes.resize(k_joints);
    for (u32 j = 0; j < k_joints; ++j) {
      const S role = static_cast<S>(j);
      mesh.nodes[j].name = anim::standard_joint_name(role);
      mesh.nodes[j].parent = parent_of(role);
      mesh.nodes[j].local.position =
          j == 0 ? Vec3{0.0f, 0.0f, 0.0f}
                 : (role == S::Hips ? Vec3{0.0f, 0.95f, 0.0f} : Vec3{0.0f, 0.125f, 0.0f});
    }
    assets::Skin skin;
    skin.name = "body";
    for (u32 j = 0; j < k_joints; ++j)
      skin.joints.push_back(static_cast<i32>(j));
    mesh.skins.push_back(skin);
    // The base: every joint moving, so a layer has something to differ from.
    Keys all_from = {
        {S::Hips, 0},          {S::Spine, 1},     {S::Chest, 2},         {S::UpperChest, 3},
        {S::Neck, 4},          {S::Head, 5},      {S::LeftShoulder, 0},  {S::LeftUpperArm, 1},
        {S::LeftLowerArm, 2},  {S::LeftHand, 3},  {S::RightShoulder, 4}, {S::RightUpperArm, 5},
        {S::RightLowerArm, 0}, {S::RightHand, 1}, {S::LeftUpperLeg, 2},  {S::LeftLowerLeg, 3},
        {S::LeftFoot, 4},      {S::LeftToes, 5},  {S::RightUpperLeg, 0}, {S::RightLowerLeg, 1},
        {S::RightFoot, 2},     {S::RightToes, 3}};
    Keys all_to = {
        {S::Hips, 2},          {S::Spine, 3},     {S::Chest, 4},         {S::UpperChest, 5},
        {S::Neck, 0},          {S::Head, 1},      {S::LeftShoulder, 2},  {S::LeftUpperArm, 3},
        {S::LeftLowerArm, 4},  {S::LeftHand, 5},  {S::RightShoulder, 0}, {S::RightUpperArm, 1},
        {S::RightLowerArm, 2}, {S::RightHand, 3}, {S::LeftUpperLeg, 4},  {S::LeftLowerLeg, 5},
        {S::LeftFoot, 0},      {S::LeftToes, 1},  {S::RightUpperLeg, 2}, {S::RightLowerLeg, 3},
        {S::RightFoot, 4},     {S::RightToes, 5}};
    mesh.animations.push_back(clip("walk", all_from, all_to, 0.02f));
    mesh.animations.push_back(clip("idle", {}, {{S::Spine, 1}, {S::Head, 2}}));
    // The crouch, held: a lean of the spine and chest, the hips 0.25 m lower, the knees bent.
    Keys crouch = {{S::Spine, 1},         {S::Chest, 1},    {S::LeftUpperLeg, 4},
                   {S::LeftLowerLeg, 5},  {S::LeftFoot, 4}, {S::RightUpperLeg, 4},
                   {S::RightLowerLeg, 5}, {S::RightFoot, 4}};
    assets::Animation c = clip("crouch", crouch, crouch, -0.25f);
    // Held from the first key: the translation's first key is the low one too.
    c.samplers.back().values[1] = c.samplers.back().values[4];
    mesh.animations.push_back(c);
    // The look: the neck and head turned, both ends held.
    mesh.animations.push_back(
        clip("look_left", {{S::Neck, 2}, {S::Head, 2}}, {{S::Neck, 2}, {S::Head, 2}}));
    mesh.animations.push_back(
        clip("look_center", {{S::Neck, 6}, {S::Head, 6}}, {{S::Neck, 6}, {S::Head, 6}}));
    mesh.animations.push_back(
        clip("look_right", {{S::Neck, 0}, {S::Head, 0}}, {{S::Neck, 0}, {S::Head, 0}}));
    std::string error;
    REQUIRE_MESSAGE(library.add(mesh, "body", nullptr, &error), error);
    REQUIRE(library.skeleton_index(skeleton) != Library::k_not_found);
  }
  const anim::Skeleton& skel() const {
    return library.skeleton_data(library.skeleton_index(skeleton));
  }
};

// A layered graph: parameters `moving` (bool), `w` (float, [0, 1]: the layer's weight) and `look`
// (float, [-1, 1]); a base machine `body` of `stand` (idle) and `move` (`base_clip`) with 0.25 s
// cross-fades on `moving`; a layer machine `layer` of one state, a Clip of `layer_clip` or, when
// `look_blend`, a Blend1D of the three look clips by `look`; its layer node of `kind` weighted by
// `w` (by its `weight` alone when `weight_param` is false) through `mask`.
struct LayerSpec {
  GraphNodeKind kind = GraphNodeKind::Override;
  const char* base_clip = "body/walk";
  const char* layer_clip = "body/look_left";
  bool look_blend = false;
  const char* mask = "";
  const char* reference = "";
  f32 weight = 1.0f;
  bool weight_param = true;
  bool stand = false;  // the base machine has a `stand` state and transitions
};

GraphDesc layered(const LayerSpec& spec, std::initializer_list<GraphMaskJoint> mask = {}) {
  GraphDesc d;
  u64 low = 1;
  const auto id = [&] { return Id128::from_parts(0x5c3, low++); };
  d.id = id();
  d.graph.name = "layered";
  for (const char* name : {"moving", "w", "look"}) {
    auto& p = d.params.emplace_back();
    p.id = id();
    p.parent = d.id;
    p.record.name = name;
  }
  d.params[0].record.kind = GraphParamKind::Bool;
  d.params[1].record.range_min = 0.0f;
  d.params[1].record.range_max = 1.0f;
  d.params[2].record.range_min = -1.0f;
  d.params[2].record.range_max = 1.0f;
  auto& body = d.machines.emplace_back();
  body.id = id();
  body.parent = d.id;
  body.record.name = "body";
  const Id128 body_id = body.id;
  const auto state = [&](const Id128& machine, const char* name) {
    auto& s = d.states.emplace_back();
    s.id = id();
    s.parent = machine;
    s.record.name = name;
    return s.id;
  };
  const auto clip_node = [&](const Id128& parent, const char* clip) {
    auto& n = d.nodes.emplace_back();
    n.id = id();
    n.parent = parent;
    n.record.clip = clip;
    n.record.looping = false;
    return n.id;
  };
  const Id128 move = state(body_id, "move");
  clip_node(move, spec.base_clip);
  d.machines[0].record.entry = move;
  if (spec.stand) {
    const Id128 stand = state(body_id, "stand");
    clip_node(stand, "body/idle");
    d.machines[0].record.entry = stand;
    for (u32 k = 0; k < 2; ++k) {
      auto& t = d.transitions.emplace_back();
      t.id = id();
      t.parent = body_id;
      t.record.from = k == 0 ? stand : move;
      t.record.to = k == 0 ? move : stand;
      t.record.duration_s = 0.25f;
      t.record.conditions.push_back(
          GraphCondition{"moving", GraphCompare::Equal, k == 0 ? 1.0f : 0.0f});
    }
  }
  auto& layer = d.machines.emplace_back();
  layer.id = id();
  layer.parent = d.id;
  layer.record.name = "layer";
  const Id128 layer_id = layer.id;
  const Id128 pose = state(layer_id, "pose");
  d.machines[1].record.entry = pose;
  if (spec.look_blend) {
    auto& b = d.nodes.emplace_back();
    b.id = id();
    b.parent = pose;
    b.record.kind = GraphNodeKind::Blend1D;
    b.record.param = "look";
    b.record.looping = false;
    const Id128 blend = b.id;
    const char* clips[3] = {"body/look_right", "body/look_center", "body/look_left"};
    for (u32 i = 0; i < 3; ++i) {
      const Id128 c = clip_node(blend, clips[i]);
      for (auto& n : d.nodes) {
        if (n.id == blend) {
          n.record.inputs.push_back(c);
          n.record.thresholds.push_back(static_cast<f32>(i) - 1.0f);
        }
      }
    }
  } else {
    clip_node(pose, spec.layer_clip);
  }
  auto& node = d.nodes.emplace_back();
  node.id = id();
  node.parent = layer_id;
  node.record.kind = spec.kind;
  node.record.weight = spec.weight;
  node.record.param = spec.weight_param ? "w" : "";
  node.record.mask = spec.mask;
  node.record.reference = spec.reference;
  if (mask.size() != 0) {
    auto& m = d.masks.emplace_back();
    m.id = id();
    m.parent = d.id;
    m.record.name = spec.mask;
    for (const GraphMaskJoint& j : mask)
      m.record.joints.push_back(j);
  }
  return d;
}

// The same base, alone: what a layer at weight 0 must leave.
GraphDesc base_only(const char* clip) {
  LayerSpec spec;
  spec.base_clip = clip;
  GraphDesc d = layered(spec);
  d.machines.pop_back();
  d.states.pop_back();
  // The layer's state's Clip node and the layer node are the last two nodes.
  d.nodes.pop_back();
  d.nodes.pop_back();
  return d;
}

CompiledGraph compiled(const Humanoid& rig, const GraphDesc& d) {
  CompiledGraph g;
  std::string why;
  REQUIRE_MESSAGE(compile_graph(d, &rig.library, g, why, &rig.skel()), why);
  REQUIRE_MESSAGE(g.program_why.empty(), g.program_why);
  return g;
}

// One instance of `g`, every machine in its first state at `time`, its inputs `values`.
void evaluate(const Humanoid& rig, const CompiledGraph& g, std::initializer_list<f32> values,
              f32 time, anim::Pose& out) {
  AnimationGraphInput in;
  AnimationGraphState st;
  init_graph_instance(g, in, st);
  u32 i = 0;
  for (const f32 v : values)
    in.values[i++] = v;
  for (u32 m = 0; m < g.machines.size(); ++m) {
    st.state[m] = 1;
    st.time[m] = time;
  }
  anim::rest_pose(rig.skel(), out);
  GraphInstance gi;
  gi.input = &in;
  gi.state = &st;
  gi.out = anim::PoseView(out);
  PoseEvaluator evaluator;
  evaluator.evaluate(g, rig.library, rig.skel(), std::span<const GraphInstance>(&gi, 1), 64);
}

bool same_joint(const anim::Pose& a, const anim::Pose& b, u32 j) {
  return std::memcmp(&a.translation[j], &b.translation[j], sizeof(Vec3)) == 0 &&
         std::memcmp(&a.rotation[j], &b.rotation[j], sizeof(Quat)) == 0 &&
         std::memcmp(&a.scale[j], &b.scale[j], sizeof(Vec3)) == 0;
}

bool same_pose(const anim::Pose& a, const anim::Pose& b) {
  if (a.joint_count() != b.joint_count()) return false;
  for (u32 j = 0; j < a.joint_count(); ++j) {
    if (!same_joint(a, b, j)) return false;
  }
  return true;
}

u64 hash_pose(const anim::Skeleton& skeleton, const anim::Pose& pose, u64 seed) {
  const u32 joints = pose.joint_count();
  Vector<Mat4> model(joints, Mat4::identity());
  Vector<anim::JointMatrix> matrices(joints, anim::JointMatrix{});
  anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
  u64 h = hash_bytes(pose.translation.data(), sizeof(Vec3) * joints, seed);
  h = hash_bytes(pose.rotation.data(), sizeof(Quat) * joints, h);
  h = hash_bytes(pose.scale.data(), sizeof(Vec3) * joints, h);
  return hash_bytes(matrices.data(), sizeof(anim::JointMatrix) * joints, h);
}

u64 next_random(u64& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

u32 joint(anim::StandardJoint j) { return static_cast<u32>(j); }

GraphMaskJoint entry(const char* name, bool subtree = true, f32 weight = 1.0f, u8 depth = 0) {
  GraphMaskJoint j;
  j.joint = name;
  j.subtree = subtree;
  j.weight = weight;
  j.depth = depth;
  return j;
}

}  // namespace

TEST_CASE("animation graph layers: a layer compiles to ops and a mask, and what is not is named") {
  Humanoid rig;
  LayerSpec spec;
  spec.mask = "head";
  const CompiledGraph g = compiled(rig, layered(spec, {entry("Neck", true, 1.0f, 2)}));
  // body: sample, accumulate, normalize; layer: the same; the override.
  REQUIRE(g.program.size() == 7);
  CHECK(g.registers == 3);  // 0 the slot, 1 a sample, 2 the layer (no Blend1D)
  CHECK(g.layer_register == 2);
  CHECK(g.program.back().kind == static_cast<u8>(PoseOpKind::Override));
  REQUIRE(g.layers.size() == 1);
  CHECK(g.layers[0].machine == 1);
  CHECK(g.base_machine == 0);
  // Neck at depth 2 tapers: the neck at half, the head (one generation below) at the full weight.
  CHECK(g.layers[0].full == 1);
  CHECK(g.layers[0].partial == 1);
  CHECK(g.layer_joints[g.layers[0].first_joint] == joint(anim::StandardJoint::Head));
  CHECK(g.layer_joints[g.layers[0].first_joint + 1] == joint(anim::StandardJoint::Neck));
  CHECK(g.layer_weights[g.layers[0].first_joint + 1] == 0.5f);
  CHECK(g.skeleton_joints == k_joints);
  MESSAGE("compiled layered graph bytes: " << g.bytes() << ", program " << g.program.size()
                                           << " ops");
  {  // with a Blend1D in the layer: four registers, the most the design allows
    LayerSpec blend = spec;
    blend.look_blend = true;
    const CompiledGraph b = compiled(rig, layered(blend, {entry("Neck", true, 1.0f, 2)}));
    CHECK(b.registers == 4);
    CHECK(b.layer_register == 3);
    MESSAGE("compiled look graph bytes: " << b.bytes() << ", program " << b.program.size()
                                          << " ops");
  }
  {  // without a skeleton: the control tables, no program, and why
    CompiledGraph none;
    std::string why;
    REQUIRE(compile_graph(layered(spec, {entry("Neck")}), &rig.library, none, why));
    CHECK(none.program.empty());
    CHECK(none.program_why.find("compiled per skeleton") != std::string::npos);
  }

  std::string why;
  CompiledGraph out;
  const auto refused = [&](const GraphDesc& d, const char* says) {
    CHECK_FALSE(compile_graph(d, &rig.library, out, why, &rig.skel()));
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
  };
  refused(layered(spec, {entry("Tail")}), "'Tail' is neither a role this skeleton maps");
  refused(layered(spec, {entry("Neck", true, 1.5f)}), "a weight outside [0, 1]");
  {
    LayerSpec s = spec;
    s.mask = "nowhere";
    refused(layered(s), "names mask 'nowhere', not a mask of the graph");
  }
  {
    LayerSpec s = spec;
    s.weight = 2.0f;
    refused(layered(s), "a weight outside [0, 1]");
  }
  {
    GraphDesc d = layered(spec, {entry("Neck")});
    d.nodes.back().record.param = "moving";
    refused(d, "its weight reads a float parameter");
  }
  {
    LayerSpec s = spec;
    s.reference = "body/idle";
    refused(layered(s, {entry("Neck")}), "a reference pose is an Additive layer's");
  }
  {
    LayerSpec s = spec;
    s.kind = GraphNodeKind::Additive;
    s.reference = "body/sprint";
    refused(layered(s, {entry("Neck")}), "reference clip 'body/sprint' is not in the library");
  }
  {  // both machines layers
    GraphDesc d = layered(spec, {entry("Neck")});
    auto& n = d.nodes.emplace_back();
    n.id = Id128::from_parts(0x5c4, 1);
    n.parent = d.machines[0].id;
    n.record.kind = GraphNodeKind::Additive;
    refused(d, "both machines are layers");
  }
  {  // two layer nodes on one machine
    GraphDesc d = layered(spec, {entry("Neck")});
    GraphItem<GraphNode> n = d.nodes.back();
    n.id = Id128::from_parts(0x5c4, 2);
    d.nodes.push_back(n);
    refused(d, "has two layer nodes");
  }
  {  // a mask on a state's pose node
    GraphDesc d = layered(spec, {entry("Neck")});
    d.nodes[0].record.mask = "head";
    refused(d, "a mask or a reference is a layer node's");
  }
  {  // a Clip as a machine's child
    GraphDesc d = layered(spec, {entry("Neck")});
    d.nodes.back().record.kind = GraphNodeKind::Clip;
    refused(d, "a machine's child node is its layer node");
  }
}

TEST_CASE("animation graph layers: an override at 0 is the base, at 1 with a full mask the layer") {
  Humanoid rig;
  LayerSpec spec;  // an override of look_left over the walk, full mask, weighted by `w`
  const CompiledGraph g = compiled(rig, layered(spec));
  const CompiledGraph base = compiled(rig, base_only("body/walk"));
  const CompiledGraph layer = compiled(rig, base_only("body/look_left"));
  anim::Pose got;
  anim::Pose want;
  for (const f32 time : {0.0f, 0.375f, 1.0f}) {
    evaluate(rig, g, {0.0f, 0.0f, 0.0f}, time, got);
    evaluate(rig, base, {}, time, want);
    CHECK(same_pose(got, want));
    evaluate(rig, g, {0.0f, 1.0f, 0.0f}, time, got);
    evaluate(rig, layer, {}, time, want);
    CHECK(same_pose(got, want));
    // And between: neither.
    evaluate(rig, g, {0.0f, 0.5f, 0.0f}, time, got);
    CHECK_FALSE(same_pose(got, want));
  }
  // A weight from the node alone (no parameter): 0 is the base.
  LayerSpec fixed = spec;
  fixed.weight_param = false;
  fixed.weight = 0.0f;
  evaluate(rig, compiled(rig, layered(fixed)), {0.0f, 1.0f, 0.0f}, 1.0f, got);
  evaluate(rig, base, {}, 1.0f, want);
  CHECK(same_pose(got, want));
}

TEST_CASE("animation graph layers: an additive of its reference pose changes nothing") {
  Humanoid rig;
  const CompiledGraph base = compiled(rig, base_only("body/walk"));
  anim::Pose got;
  anim::Pose want;
  // The crouch over its own first key, at its keys; the rest pose over the rest pose (an empty
  // clip, an empty reference) at any time; masked and not. Between two equal keys the crouch is not
  // its key to the bit — the sampler's slerp of a rotation with itself moves it by an ulp (anim's,
  // not the layer's) — so a held clip is the reference only where it is sampled at a key.
  struct Case {
    const char* clip;
    const char* reference;
    const char* mask;
  };
  for (const Case c : {Case{"body/crouch", "body/crouch", ""}, Case{"", "", ""},
                       Case{"body/crouch", "body/crouch", "legs"}}) {
    LayerSpec spec;
    spec.kind = GraphNodeKind::Additive;
    spec.layer_clip = c.clip;
    spec.reference = c.reference;
    spec.mask = c.mask;
    const CompiledGraph g =
        compiled(rig, layered(spec, {entry("LeftUpperLeg"), entry("Spine", false)}));
    for (const f32 w : {0.25f, 0.5f, 1.0f}) {
      for (const f32 time : {0.0f, 0.375f, 1.0f}) {
        if (c.clip[0] != '\0' && time != 0.0f && time != 1.0f) continue;
        evaluate(rig, g, {0.0f, w, 0.0f}, time, got);
        evaluate(rig, base, {}, time, want);
        CHECK(same_pose(got, want));
      }
    }
  }
  // And the crouch over the rest pose is not nothing: at weight 1 the spine is the walk's turned
  // on the right by the crouch's (ozz's order), the hips 0.25 m lower.
  LayerSpec spec;
  spec.kind = GraphNodeKind::Additive;
  spec.layer_clip = "body/crouch";
  const CompiledGraph g = compiled(rig, layered(spec));
  evaluate(rig, g, {0.0f, 1.0f, 0.0f}, 1.0f, got);
  evaluate(rig, base, {}, 1.0f, want);
  const u32 spine = joint(anim::StandardJoint::Spine);
  const Quat expect = normalize(want.rotation[spine] *
                                Quat{k_turns[1][0], k_turns[1][1], k_turns[1][2], k_turns[1][3]});
  CHECK(std::fabs(got.rotation[spine].x - expect.x) < 1e-6f);
  CHECK(std::fabs(got.rotation[spine].w - expect.w) < 1e-6f);
  const u32 hips = joint(anim::StandardJoint::Hips);
  CHECK(std::fabs(got.translation[hips].y - (want.translation[hips].y - 0.25f)) < 1e-6f);
  // The head, which the crouch does not move, is the base's to the bit.
  CHECK(same_joint(got, want, joint(anim::StandardJoint::Head)));
}

TEST_CASE("animation graph layers: a masked override leaves every joint outside its mask") {
  Humanoid rig;
  const CompiledGraph base = compiled(rig, base_only("body/walk"));
  const CompiledGraph look = compiled(rig, base_only("body/look_left"));
  LayerSpec spec;
  spec.mask = "head";
  const CompiledGraph g = compiled(rig, layered(spec, {entry("Neck", true, 1.0f, 2)}));
  anim::Pose got;
  anim::Pose b;
  anim::Pose l;
  const u32 neck = joint(anim::StandardJoint::Neck);
  const u32 head = joint(anim::StandardJoint::Head);
  for (const f32 w : {0.0f, 0.3f, 0.7f, 1.0f}) {
    for (const f32 time : {0.0f, 0.5f, 1.0f}) {
      evaluate(rig, g, {0.0f, w, 0.0f}, time, got);
      evaluate(rig, base, {}, time, b);
      evaluate(rig, look, {}, time, l);
      for (u32 j = 0; j < k_joints; ++j) {
        if (j == neck || j == head) continue;
        CHECK(same_joint(got, b, j));
      }
      if (w == 0.0f) CHECK(same_joint(got, b, head));
      if (w == 1.0f) CHECK(same_joint(got, l, head));  // the full joint of the mask is copied
      if (w > 0.0f) CHECK_FALSE(same_joint(got, b, neck));
      if (w > 0.0f) CHECK_FALSE(same_joint(got, l, neck));  // the taper's half joint
    }
  }
}

TEST_CASE("animation graph layers: the walker's crouch and head-look as layers") {
  // The two layers PlayerBody applies by hand in model space today (apps.md, "The head-look
  // layer"), as a graph would carry them in joint space: an additive crouch by a `crouch`
  // parameter (here `w`) over the legs and the spine, and an override of the head and neck by a
  // `look` parameter through a Blend1D of three looks. Two graphs: a graph holds two machines (a
  // stated limit, ADR-0017), and the walker's would need three (animation-graphs.md, "The use it
  // is for").
  Humanoid rig;
  const CompiledGraph base = compiled(rig, base_only("body/walk"));
  anim::Pose got;
  anim::Pose want;
  {
    LayerSpec spec;
    spec.kind = GraphNodeKind::Additive;
    spec.layer_clip = "body/crouch";
    spec.mask = "crouch";
    spec.stand = true;
    const CompiledGraph g = compiled(
        rig, layered(spec, {entry("Hips", false), entry("Spine", false), entry("Chest", false),
                            entry("LeftUpperLeg"), entry("RightUpperLeg")}));
    CHECK(g.layers[0].full == 11);
    // Standing up (crouch 0): the walk; crouched: the hips lower, the arms and head untouched.
    evaluate(rig, g, {1.0f, 0.0f, 0.0f}, 1.0f, got);  // state index 0 of `body` is `move`
    evaluate(rig, base, {}, 1.0f, want);
    CHECK(same_pose(got, want));
    AnimationGraphInput in;
    AnimationGraphState st;
    init_graph_instance(g, in, st);
    in.values[0] = 1.0f;  // moving: the base walks
    GraphStep steps[k_graph_max_machines];
    for (u32 t = 0; t < 40; ++t)
      (void)graph_control(g, in, st, k_dt, steps);
    CHECK(st.state[0] == 1 + g.state_index(0, "move"));
    anim::Pose half;
    anim::Pose full;
    for (const f32 crouch : {0.5f, 1.0f}) {
      in.values[1] = crouch;
      anim::Pose& out = crouch < 1.0f ? half : full;
      anim::rest_pose(rig.skel(), out);
      GraphInstance gi;
      gi.input = &in;
      gi.state = &st;
      gi.out = anim::PoseView(out);
      PoseEvaluator evaluator;
      evaluator.evaluate(g, rig.library, rig.skel(), std::span<const GraphInstance>(&gi, 1), 64);
    }
    const u32 hips = joint(anim::StandardJoint::Hips);
    CHECK(full.translation[hips].y < half.translation[hips].y);
    for (const anim::StandardJoint j : {anim::StandardJoint::Head, anim::StandardJoint::LeftHand,
                                        anim::StandardJoint::RightUpperArm})
      CHECK(same_joint(half, full, joint(j)));
  }
  {
    LayerSpec spec;
    spec.look_blend = true;
    spec.weight_param = false;  // the look's weight is 1; `look` chooses where it looks
    spec.mask = "head";
    const CompiledGraph g = compiled(rig, layered(spec, {entry("Neck", true, 1.0f, 2)}));
    const CompiledGraph left = compiled(rig, base_only("body/look_left"));
    const CompiledGraph center = compiled(rig, base_only("body/look_center"));
    anim::Pose l;
    for (const f32 look : {-1.0f, 0.0f, 0.5f, 1.0f}) {
      evaluate(rig, g, {0.0f, 0.0f, look}, 1.0f, got);
      evaluate(rig, base, {}, 1.0f, want);
      for (u32 j = 0; j < k_joints; ++j) {
        if (j == joint(anim::StandardJoint::Neck) || j == joint(anim::StandardJoint::Head))
          continue;
        CHECK(same_joint(got, want, j));
      }
      if (look == 1.0f) {
        evaluate(rig, left, {}, 1.0f, l);
        CHECK(same_joint(got, l, joint(anim::StandardJoint::Head)));
      }
      if (look == 0.0f) {
        evaluate(rig, center, {}, 1.0f, l);
        CHECK(same_joint(got, l, joint(anim::StandardJoint::Head)));
      }
    }
  }
}

TEST_CASE("animation graph layers: one layered graph at three weights is pinned (MSVC)") {
  // The look's override (neck at half, head full) over the walk, at its last key, weights 0.25,
  // 0.5 and 0.75: every clip on a key (no slerp), so the pose is the program's + - * / sqrt alone.
  // Taken on MSVC; GCC and Clang are the stack's containers to confirm.
  Humanoid rig;
  LayerSpec spec;
  spec.mask = "head";
  const CompiledGraph g = compiled(rig, layered(spec, {entry("Neck", true, 1.0f, 2)}));
  LayerSpec add;
  add.kind = GraphNodeKind::Additive;
  add.layer_clip = "body/crouch";
  const CompiledGraph a = compiled(rig, layered(add));
  anim::Pose pose;
  u64 h = 0;
  for (const f32 w : {0.25f, 0.5f, 0.75f}) {
    evaluate(rig, g, {0.0f, w, 0.0f}, 1.0f, pose);
    h = hash_pose(rig.skel(), pose, h);
  }
  u64 ha = 0;
  for (const f32 w : {0.25f, 0.5f, 0.75f}) {
    evaluate(rig, a, {0.0f, w, 0.0f}, 1.0f, pose);
    ha = hash_pose(rig.skel(), pose, ha);
  }
  MESSAGE("layer pin (override): " << h << ", (additive): " << ha);
  // Pinned 2026-11-18 on MSVC (msvc-debug, x86-64-v3) when the layer ops were written (S35 stage
  // 3); GCC and Clang are the stack's containers to confirm.
  CHECK(h == 4753967804745800757ull);
  CHECK(ha == 8136567502327670236ull);
}

TEST_CASE("animation graph layers: one worker and eight, two batches, two runs: the same bytes") {
  Humanoid rig;
  LayerSpec spec;
  spec.look_blend = true;
  spec.mask = "upper";
  spec.stand = true;
  const GraphDesc desc =
      layered(spec, {entry("Spine", true, 1.0f, 3), entry("LeftUpperLeg", false, 0.5f)});
  constexpr u32 k_instances = 96;

  const auto run = [&](u32 workers, i64 batch, std::vector<anim::JointMatrix>& out,
                       std::vector<AnimationGraphState>& states) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    ecs::JobOsApi adapter(js);
    ecs::SimWorldConfig wc;
    wc.hz = 64;
    ecs::SimWorld sim(wc);
    AnimationSystem animation(rig.library);
    animation.install(sim);
    if (workers > 1) {
      const u32 requested = workers <= adapter.max_workers() ? workers : adapter.max_workers();
      REQUIRE(ecs::set_workers(sim.world(), requested));
    }
    REQUIRE(tunables::find("animation.graph.batch")->set_from_text(std::to_string(batch), nullptr));
    std::string why;
    REQUIRE_MESSAGE(animation.add_graph(desc, rig.skeleton, why), why);
    ecs::WorldCommands commands(sim.world());
    std::vector<Id128> ids;
    for (u32 i = 0; i < k_instances; ++i) {
      ids.push_back(Id128::from_seed(0x5c5, i));
      commands.create(ids.back());
    }
    commands.apply();
    for (u32 i = 0; i < k_instances; ++i) {
      REQUIRE(animation.attach_graph(ids[i], desc.id, rig.skeleton));
      if (i % 5 == 4) animation.set_tier(ids[i], 1);
    }
    u64 seed = 0x1a7e5u;
    for (u32 tick = 0; tick < 48; ++tick) {
      for (u32 i = 0; i < k_instances; ++i) {
        const u64 r = next_random(seed);
        // The weight exactly 0 or 1 a third of the time each, else between.
        const u32 pick = static_cast<u32>((r >> 11) % 3u);
        const f32 w = pick == 0 ? 0.0f : (pick == 1 ? 1.0f : static_cast<f32>(r % 997u) / 997.0f);
        const f32 values[3] = {((r >> 5) % 7u) != 0 ? 1.0f : 0.0f, w,
                               static_cast<f32>((r >> 17) % 2001u) / 1000.0f - 1.0f};
        REQUIRE(animation.set_graph_inputs(ids[i], values, 0));
      }
      sim.step();
    }
    const std::span<const anim::JointMatrix> m = animation.joint_matrices();
    out.assign(m.begin(), m.end());
    states.clear();
    for (const Id128& id : ids) {
      AnimationGraphState st;
      REQUIRE(animation.graph_state(id, st));
      states.push_back(st);
    }
  };
  std::vector<anim::JointMatrix> one;
  std::vector<anim::JointMatrix> again;
  std::vector<anim::JointMatrix> eight;
  std::vector<anim::JointMatrix> small;
  std::vector<AnimationGraphState> s1;
  std::vector<AnimationGraphState> s2;
  std::vector<AnimationGraphState> s8;
  std::vector<AnimationGraphState> s7;
  run(1, 64, one, s1);
  run(1, 64, again, s2);
  run(8, 64, eight, s8);
  run(1, 7, small, s7);
  REQUIRE(tunables::find("animation.graph.batch")->set_from_text("64", nullptr));
  REQUIRE(one.size() == k_instances * k_joints);
  REQUIRE(eight.size() == one.size());
  REQUIRE(small.size() == one.size());
  const usize bytes = one.size() * sizeof(anim::JointMatrix);
  CHECK(std::memcmp(one.data(), again.data(), bytes) == 0);
  CHECK(std::memcmp(one.data(), eight.data(), bytes) == 0);
  CHECK(std::memcmp(one.data(), small.data(), bytes) == 0);
  const usize sbytes = s1.size() * sizeof(AnimationGraphState);
  CHECK(std::memcmp(s1.data(), s2.data(), sbytes) == 0);
  CHECK(std::memcmp(s1.data(), s8.data(), sbytes) == 0);
  CHECK(std::memcmp(s1.data(), s7.data(), sbytes) == 0);
}
