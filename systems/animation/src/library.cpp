#include "animation_log.h"

#include <core/hash/hash.h>
#include <core/log/log.h>
#include <domain/assets/gltf.h>
#include <systems/animation/library.h>

#include <algorithm>
#include <cmath>

namespace engine::animation {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

Id128 asset_id(std::string_view kind, std::string_view name) noexcept {
  // A pure function of the name, so the id a save holds resolves in the next process and on
  // another machine. `hash_bytes` is documented as deterministic across platforms; it is not a
  // content hash and does not need to be — the name is the identity here, not the bytes.
  const u64 seed = hash_bytes(kind.data(), kind.size());
  const u64 hi = hash_bytes(name.data(), name.size(), seed);
  const u64 lo = hash_combine(hi, static_cast<u64>(name.size()) ^ seed);
  return Id128::from_parts(hi, lo);
}

// The file's own name without its directory or its extension. The id must not depend on where the
// file was on the machine that loaded it: a save that named "D:/work/tree/rig.glb/bar_skin" would
// not resolve on a build server or on a player's install.
std::string path_stem(std::string_view path) {
  usize begin = 0;
  for (usize i = 0; i < path.size(); ++i) {
    if (path[i] == '/' || path[i] == '\\') begin = i + 1;
  }
  usize end = path.size();
  for (usize i = end; i > begin; --i) {
    if (path[i - 1] == '.') {
      end = i - 1;
      break;
    }
  }
  return std::string(path.substr(begin, end - begin));
}

std::string qualify(std::string_view prefix, std::string_view name, const char* fallback,
                    u32 index) {
  std::string out;
  if (!prefix.empty()) {
    out.append(prefix);
    out.push_back('/');
  }
  if (name.empty()) {
    out.append(fallback);
    out.append(std::to_string(index));
  } else {
    out.append(name);
  }
  return out;
}

// glTF's channel path values and anim's channel constants are the same three numbers, and both
// headers say so; the conversion is a copy, and this is the assertion that keeps it one.
static_assert(assets::k_path_translation == anim::k_channel_translation);
static_assert(assets::k_path_rotation == anim::k_channel_rotation);
static_assert(assets::k_path_scale == anim::k_channel_scale);
static_assert(assets::k_interp_linear == anim::k_interp_linear);
static_assert(assets::k_interp_step == anim::k_interp_step);
static_assert(assets::k_interp_cubic == anim::k_interp_cubic);

// --- non-joint ancestors (S92) -------------------------------------------------------------------
//
// A joint's rest is its node's transform composed with every **non-joint** node between it and its
// nearest joint ancestor — or, for a root joint, the scene root: the `Armature` Blender writes, and
// anything a rig puts between two bones. The skeleton keeps the skin's joints and nothing else
// (the palette is the vertex bindings' and cannot grow), so those nodes are folded into the joint's
// own local transform, at bind and in every clip. See docs/subsystems/animation.md, "Non-joint
// ancestors".
//
// The fold is `core/math`'s `compose` with one difference that matters: where the parent is the
// identity, or has no rotation and no scale, it does no arithmetic it does not need. `compose`
// renormalizes the product of the rotations and rotates a zero vector, and either can move the last
// bit of a float; a rig whose ancestors are the identity has to load exactly as it did before the
// fold existed, and one whose ancestor is a pure translation has to pose exactly like the file
// rewritten with that translation moved into the root joint. Both are tested to the bit.
//
// TRS composition is exact for a parent of uniform scale. A non-uniformly scaled ancestor over a
// rotated child is a shear, which no TRS represents; it is approximated the way `compose` always
// has, by multiplying the scales.

bool is_identity(Quat q) noexcept { return q == Quat::identity(); }
bool is_one(Vec3 s) noexcept { return s == Vec3::one(); }
bool is_zero(Vec3 v) noexcept { return v == Vec3{}; }

// The parent's linear part applied to a vector: a tangent, or a child's offset.
Vec3 fold_vector(const Transform3& parent, Vec3 v) noexcept {
  if (!is_one(parent.scale)) v = v * parent.scale;
  if (!is_identity(parent.rotation)) v = rotate(parent.rotation, v);
  return v;
}

Vec3 fold_point(const Transform3& parent, Vec3 p) noexcept {
  const Vec3 v = fold_vector(parent, p);
  return is_zero(parent.position) ? v : v + parent.position;
}

Transform3 fold(const Transform3& parent, const Transform3& child) noexcept {
  if (parent == Transform3::identity()) return child;
  Transform3 out;
  out.position = fold_point(parent, child.position);
  out.rotation =
      is_identity(parent.rotation) ? child.rotation : normalize(parent.rotation * child.rotation);
  out.scale = is_one(parent.scale) ? child.scale : parent.scale * child.scale;
  return out;
}

Quat quat_at(const f32* v) noexcept { return Quat{v[0], v[1], v[2], v[3]}; }
Vec3 vec3_at(const f32* v) noexcept { return Vec3{v[0], v[1], v[2]}; }

// Which of a node's three channels move a joint's channel `c` through the fold: everything moves a
// translation (a parent's rotation and scale carry the child's offset), only a rotation moves a
// rotation, only a scale a scale.
bool channel_moves(u8 node_path, u8 c) noexcept {
  return c == assets::k_path_translation || node_path == c;
}

// How many seconds-between-keys pieces a resampled interval is cut into when the composed channel
// is not linear in its inputs (two animated inputs, an ancestor's rotation carrying an offset, a
// cubic key). Four is a cubic's inflection and a slerp's curvature at the scale a skeleton's keys
// are authored at; the cost is load time and four times the keys of one joint's channel.
constexpr u32 k_resample_steps = 4;

// One animation channel the fold reads: where on the joint's chain it sits (0 the topmost
// ancestor, `chain.size()` the joint itself) and what it drives.
struct FoldInput {
  u32 link = 0;
  u8 path = 0;
  const assets::AnimationSampler* sampler = nullptr;
};

// The keys of a joint's channel `c` when exactly one track moves it and that track drives the same
// kind of channel: the composed channel is then an affine function of the track's value — a
// translation offset and rotated, a rotation multiplied on both sides, a scale scaled — so the keys
// are mapped one by one, the interpolation is kept, a cubic's tangents go through the linear part
// alone, and the result between the keys is exactly the composition of what the file says.
void fold_keys(std::span<const Transform3> links, u32 at, u8 c,
               const assets::AnimationSampler& sampler, Vector<f32>& out) {
  Transform3 prefix = Transform3::identity();
  for (u32 i = 0; i < at; ++i)
    prefix = fold(prefix, links[i]);
  Transform3 suffix = Transform3::identity();
  for (u32 i = at + 1; i < links.size(); ++i)
    suffix = fold(suffix, links[i]);
  const u32 width = sampler.components;
  const u32 per_key = sampler.interpolation == assets::k_interp_cubic ? 3u : 1u;
  out.assign(sampler.values.begin(), sampler.values.end());
  if (width != (c == assets::k_path_rotation ? 4u : 3u)) return;  // the clip's own check refuses it
  const Vec3 offset = fold_vector(links[at], suffix.position);
  for (u32 e = 0; e + width <= out.size(); e += width) {
    f32* v = out.data() + e;
    const bool tangent = per_key == 3u && (e / width) % 3u != 1u;
    if (c == assets::k_path_translation) {
      const Vec3 p = vec3_at(v);
      const Vec3 r =
          tangent ? fold_vector(prefix, p) : fold_point(prefix, is_zero(offset) ? p : p + offset);
      v[0] = r.x;
      v[1] = r.y;
      v[2] = r.z;
    } else if (c == assets::k_path_rotation) {
      Quat q = quat_at(v);
      if (!is_identity(prefix.rotation)) q = prefix.rotation * q;
      if (!is_identity(suffix.rotation)) q = q * suffix.rotation;
      v[0] = q.x;
      v[1] = q.y;
      v[2] = q.z;
      v[3] = q.w;
    } else {
      Vec3 s = vec3_at(v);
      if (!is_one(prefix.scale)) s = prefix.scale * s;
      if (!is_one(suffix.scale)) s = s * suffix.scale;
      v[0] = s.x;
      v[1] = s.y;
      v[2] = s.z;
    }
  }
}

// The keys of a joint's channel `c` when several tracks move it, or one of another kind: sampled
// through `anim`'s own interpolation at every key time of every input, and `k_resample_steps`
// times inside each interval unless every input holds its keys (STEP), then composed down the
// chain. Exact at the keys and for STEP; linear in between otherwise.
bool resample_keys(std::span<const Transform3> links, std::span<const FoldInput> inputs, u8 c,
                   u8& interpolation, Vector<f32>& key_times, Vector<f32>& key_values) {
  const u32 count = static_cast<u32>(links.size());
  anim::Skeleton chain;
  chain.resize(count);
  for (u32 i = 0; i < count; ++i) {
    chain.parents[i] = i == 0 ? anim::k_no_joint : static_cast<i32>(i - 1);
    chain.local_bind[i] = links[i];
  }
  anim::Clip clip;
  clip.joint_count = count;
  bool held = true;
  Vector<f32> times;
  for (const FoldInput& input : inputs) {
    const assets::AnimationSampler& s = *input.sampler;
    if (!clip.add_track(input.link, input.path, s.interpolation, s.components,
                        std::span<const f32>(s.times.data(), s.times.size()),
                        std::span<const f32>(s.values.data(), s.values.size())))
      return false;
    held = held && s.interpolation == assets::k_interp_step;
    for (const f32 t : s.times)
      times.push_back(t);
  }
  std::sort(times.begin(), times.end());
  key_times.clear();
  for (u32 i = 0; i < times.size(); ++i) {
    if (i > 0 && times[i] == times[i - 1]) continue;
    if (!held && !key_times.empty()) {
      const f32 from = key_times.back();
      for (u32 k = 1; k < k_resample_steps; ++k)
        key_times.push_back(from + (times[i] - from) *
                                       (static_cast<f32>(k) / static_cast<f32>(k_resample_steps)));
    }
    key_times.push_back(times[i]);
  }
  interpolation = held ? anim::k_interp_step : anim::k_interp_linear;
  key_values.clear();
  anim::Pose pose;
  for (const f32 time : key_times) {
    anim::rest_pose(chain, pose);
    clip.sample(time, pose, false);
    Transform3 at = Transform3::identity();
    for (u32 i = 0; i < count; ++i)
      at = fold(at, Transform3{pose.translation[i], pose.rotation[i], pose.scale[i]});
    if (c == assets::k_path_rotation) {
      for (const f32 v : {at.rotation.x, at.rotation.y, at.rotation.z, at.rotation.w})
        key_values.push_back(v);
    } else {
      const Vec3 v = c == assets::k_path_translation ? at.position : at.scale;
      for (const f32 f : {v.x, v.y, v.z})
        key_values.push_back(f);
    }
  }
  return true;
}

}  // namespace

Id128 Library::skeleton_id(std::string_view name) noexcept { return asset_id("skeleton", name); }
Id128 Library::clip_id(std::string_view name) noexcept { return asset_id("clip", name); }

u32 Library::skeleton_index(const Id128& id) const noexcept {
  const u32* found = skeleton_by_id_.find_value(id);
  return found != nullptr ? *found : k_not_found;
}

u32 Library::clip_index(const Id128& id) const noexcept {
  const u32* found = clip_by_id_.find_value(id);
  return found != nullptr ? *found : k_not_found;
}

u32 Library::add_skeleton(SkeletonAsset&& asset) {
  const u32* existing = skeleton_by_id_.find_value(asset.id);
  if (existing != nullptr) {
    // Replace in place: the id is a function of the name, so a reload of the same content must
    // not invalidate a save that names it.
    const u32 index = *existing;
    skeletons_[index] = std::move(asset);
    return index;
  }
  const u32 index = skeletons_.size();
  skeleton_by_id_.insert(asset.id, index);
  skeletons_.push_back(std::move(asset));
  return index;
}

u32 Library::add_clip(ClipAsset&& asset) {
  const u32* existing = clip_by_id_.find_value(asset.id);
  if (existing != nullptr) {
    const u32 index = *existing;
    clips_[index] = std::move(asset);
    return index;
  }
  const u32 index = clips_.size();
  clip_by_id_.insert(asset.id, index);
  clips_.push_back(std::move(asset));
  return index;
}

bool Library::add(const assets::MeshData& mesh, std::string_view prefix, LoadStats* stats,
                  std::string* error) {
  // A file with no skin is still worth loading when it animates morph weights: those clips drive
  // no joint, so they need no skeleton (see the skeleton-less branch below). One with neither has
  // nothing this library could hold, and says so.
  bool has_weight_curves = false;
  for (const assets::Animation& animation : mesh.animations) {
    for (const assets::AnimationChannel& channel : animation.channels)
      has_weight_curves = has_weight_curves || channel.path == assets::k_path_weights;
  }
  if (mesh.skins.empty() && !has_weight_curves)
    return fail(error, "animation: the mesh has no skin to build a skeleton from");

  LoadStats local;
  // The skin palette index of every node, so an animation channel's node becomes a joint in one
  // lookup rather than a search per channel per skin.
  Vector<Vector<i32>> node_to_joint(mesh.skins.size());
  Vector<u32> skeleton_index(mesh.skins.size(), 0u);
  // Per skin: each joint's non-joint ancestors up to its nearest joint ancestor (or the scene
  // root), topmost first, and which nodes are on any such chain — so a channel on the Armature is
  // recognised as moving the skeleton rather than counted as skipped.
  struct Ancestry {
    Vector<Vector<u32>> chain;
    Vector<u8> on_chain;
  };
  Vector<Ancestry> ancestry(mesh.skins.size());

  for (u32 s = 0; s < mesh.skins.size(); ++s) {
    const assets::Skin& skin = mesh.skins[s];
    const u32 joints = skin.joints.size();
    if (joints == 0) return fail(error, "animation: skin " + std::to_string(s) + " has no joints");

    Vector<i32>& to_joint = node_to_joint[s];
    to_joint.assign(mesh.nodes.size(), anim::k_no_joint);
    for (u32 j = 0; j < joints; ++j) {
      const i32 node = skin.joints[j];
      if (node < 0 || static_cast<u32>(node) >= mesh.nodes.size())
        return fail(error, "animation: skin " + std::to_string(s) + " joint " + std::to_string(j) +
                               " names node " + std::to_string(node) + ", which is not a node");
      to_joint[static_cast<u32>(node)] = static_cast<i32>(j);
    }

    SkeletonAsset asset;
    asset.name = qualify(prefix, skin.name, "skin", s);
    asset.id = skeleton_id(asset.name);
    anim::Skeleton& bones = asset.skeleton;
    bones.resize(joints);
    Ancestry& ancestors = ancestry[s];
    ancestors.chain.resize(joints);
    ancestors.on_chain.assign(mesh.nodes.size(), u8{0});
    for (u32 j = 0; j < joints; ++j) {
      const assets::Node& node = mesh.nodes[static_cast<u32>(skin.joints[j])];
      bones.names[j] = node.name;
      // Up through the non-joint nodes to the nearest joint or the scene root. The importer sorts
      // a parent before its child, so the walk only descends in index and ends; the count is the
      // guard against a hand-built `MeshData` that does not.
      Vector<u32>& chain = ancestors.chain[j];
      i32 up = node.parent;
      for (u32 steps = 0;
           up >= 0 && static_cast<u32>(up) < mesh.nodes.size() &&
           to_joint[static_cast<u32>(up)] == anim::k_no_joint && steps < mesh.nodes.size();
           ++steps) {
        chain.push_back(static_cast<u32>(up));
        ancestors.on_chain[static_cast<u32>(up)] = 1;
        up = mesh.nodes[static_cast<u32>(up)].parent;
      }
      std::reverse(chain.begin(), chain.end());
      Transform3 rest = Transform3::identity();
      for (const u32 link : chain)
        rest = fold(rest, mesh.nodes[link].local);
      bones.local_bind[j] = fold(rest, node.local);
      const i32 parent_joint = up >= 0 && static_cast<u32>(up) < mesh.nodes.size()
                                   ? to_joint[static_cast<u32>(up)]
                                   : anim::k_no_joint;
      // **The palette order is the skeleton's order and cannot be changed**: a vertex's
      // `geometry::SkinBinding` names a palette slot, and those bindings live in the mesh streams
      // this capability does not own. So a palette that is not parent-before-child is refused
      // rather than reordered. `assets::load_gltf` sorts the node array that way and every
      // exporter writes the palette in node order, so this is a stated limit and not a hidden one
      // (ADR-0017).
      if (parent_joint != anim::k_no_joint && static_cast<u32>(parent_joint) >= j) {
        return fail(error, "animation: skin " + std::to_string(s) + " lists joint " +
                               std::to_string(j) + " before its parent " +
                               std::to_string(parent_joint) +
                               "; the palette order is fixed by the vertex bindings and cannot be "
                               "sorted here");
      }
      bones.parents[j] = parent_joint;
    }
    if (skin.inverse_bind.size() == joints) {
      // Authored data wins over the node transforms: where they disagree, the file is right about
      // how the mesh was bound (docs/subsystems/anim.md).
      for (u32 j = 0; j < joints; ++j)
        bones.inverse_bind[j] = skin.inverse_bind[j];
    } else {
      anim::compute_inverse_bind(bones);
    }
    if (!bones.validate(error)) return false;

    asset.mapping = anim::map_joints(bones);
    asset.mapped_roles = asset.mapping.mapped;
    skeleton_index[s] = add_skeleton(std::move(asset));
    ++local.skeletons;
  }

  for (u32 a = 0; a < mesh.animations.size(); ++a) {
    const assets::Animation& animation = mesh.animations[a];
    // Which skin this animation drives: the one whose palette the most channels land in. A file
    // with one skin — which is nearly all of them — takes the first branch of that comparison and
    // nothing else happens.
    u32 best_skin = 0;
    u32 best_hits = 0;
    for (u32 s = 0; s < mesh.skins.size(); ++s) {
      u32 hits = 0;
      for (const assets::AnimationChannel& channel : animation.channels) {
        if (channel.node < 0 || static_cast<u32>(channel.node) >= mesh.nodes.size()) continue;
        if (node_to_joint[s][static_cast<u32>(channel.node)] != anim::k_no_joint ||
            ancestry[s].on_chain[static_cast<u32>(channel.node)] != 0)
          ++hits;
      }
      if (hits > best_hits) {
        best_hits = hits;
        best_skin = s;
      }
    }
    // **No channel in any palette is not the same as nothing to play.** An animation whose only
    // curves are morph weights — every one of the Khronos morph samples, and a facial take over a
    // body another clip poses — lands in no skin at all, and used to be skipped whole here, weight
    // tracks included. It becomes a skeleton-less clip instead: `skeleton` is `k_not_found`,
    // `joint_count` is zero, and its joint channels (a node that is not a joint) are counted as
    // skipped exactly as they would be beside a skin.
    const bool skeletal = best_hits > 0;
    bool weights_only = false;
    if (!skeletal) {
      for (const assets::AnimationChannel& channel : animation.channels)
        weights_only = weights_only || channel.path == assets::k_path_weights;
      if (!weights_only) {
        local.skipped_channels += animation.channels.size();
        continue;
      }
    }

    ClipAsset asset;
    asset.name = qualify(prefix, animation.name, "animation", a);
    asset.id = clip_id(asset.name);
    asset.skeleton = skeletal ? skeleton_index[best_skin] : k_not_found;
    asset.clip.name = asset.name;
    asset.clip.joint_count = skeletal ? skeletons_[asset.skeleton].skeleton.joint_count() : 0u;

    const Vector<i32>* to_joint = skeletal ? &node_to_joint[best_skin] : nullptr;
    const assets::Skin* skin = skeletal ? &mesh.skins[best_skin] : nullptr;
    const Ancestry* ancestors = skeletal ? &ancestry[best_skin] : nullptr;
    // The sampler driving each (node, path), for the nodes the fold reads; the first channel wins
    // where a file names one twice. Built only when some channel lands on an ancestor: a file
    // without one never pays for it.
    auto node_channel = [&](const assets::AnimationChannel& channel) {
      return channel.path <= assets::k_path_scale && channel.node >= 0 &&
             static_cast<u32>(channel.node) < mesh.nodes.size() &&
             channel.sampler < animation.samplers.size();
    };
    bool ancestor_animated = false;
    for (const assets::AnimationChannel& channel : animation.channels) {
      ancestor_animated =
          ancestor_animated || (ancestors != nullptr && node_channel(channel) &&
                                ancestors->on_chain[static_cast<u32>(channel.node)] != 0);
    }
    Vector<const assets::AnimationSampler*> driven;
    if (ancestor_animated) {
      driven.assign(mesh.nodes.size() * 3u, nullptr);
      for (const assets::AnimationChannel& channel : animation.channels) {
        if (!node_channel(channel)) continue;
        const u32 slot = static_cast<u32>(channel.node) * 3u + channel.path;
        if (driven[slot] == nullptr) driven[slot] = &animation.samplers[channel.sampler];
      }
    }
    // Whether an animated ancestor moves joint `j`'s channel `c`, in which case that channel is
    // built after the loop from every track that moves it, not from the joint's own track alone.
    auto chain_moves = [&](u32 j, u8 c) {
      if (driven.empty()) return false;
      for (const u32 link : ancestors->chain[j]) {
        for (u8 p = 0; p < 3; ++p) {
          if (driven[link * 3u + p] != nullptr && channel_moves(p, c)) return true;
        }
      }
      return false;
    };
    Vector<Transform3> links;
    auto chain_links = [&](u32 j) {
      links.clear();
      for (const u32 link : ancestors->chain[j])
        links.push_back(mesh.nodes[link].local);
      links.push_back(mesh.nodes[static_cast<u32>(skin->joints[j])].local);
      return std::span<const Transform3>(links.data(), links.size());
    };
    Vector<f32> folded_values;
    Vector<f32> folded_times;
    Vector<FoldInput> inputs;
    for (const assets::AnimationChannel& channel : animation.channels) {
      // A weights channel names a node only to say which mesh it drives, and the importer has
      // already turned that into `morph_channel`, so neither a skin nor a node table (a file
      // with no skin has none) is needed to place it.
      const bool in_range =
          channel.path == assets::k_path_weights ||
          (channel.node >= 0 && static_cast<u32>(channel.node) < mesh.nodes.size());
      if (!in_range || channel.sampler >= animation.samplers.size()) {
        ++local.skipped_channels;
        continue;
      }
      // A **weights** channel is not per joint: it drives a run of the mesh's morph channels, so
      // it becomes a `WeightTrack` rather than being matched against the skeleton at all. That
      // also means a clip may consist of nothing but weight tracks — a facial performance over a
      // body pose another clip owns — which is why the "no tracks" test below looks at both.
      if (channel.path == assets::k_path_weights) {
        const assets::AnimationSampler& sampler = animation.samplers[channel.sampler];
        if (!asset.clip.add_weight_track(
                channel.morph_channel, sampler.components, sampler.interpolation,
                std::span<const f32>(sampler.times.data(), sampler.times.size()),
                std::span<const f32>(sampler.values.data(), sampler.values.size()))) {
          return fail(error, "animation: clip '" + asset.name + "' has a weight track whose keys " +
                                 "and values do not agree");
        }
        continue;
      }
      const i32 joint =
          to_joint != nullptr ? (*to_joint)[static_cast<u32>(channel.node)] : anim::k_no_joint;
      if (joint == anim::k_no_joint) {
        // An ancestor's own channel is not dropped: it is read below, into the joints it moves.
        if (ancestors == nullptr || ancestors->on_chain[static_cast<u32>(channel.node)] == 0)
          ++local.skipped_channels;
        continue;
      }
      const u32 j = static_cast<u32>(joint);
      if (channel.path <= assets::k_path_scale && chain_moves(j, channel.path)) continue;
      const assets::AnimationSampler& sampler = animation.samplers[channel.sampler];
      std::span<const f32> values(sampler.values.data(), sampler.values.size());
      // A still ancestor folds into the joint's own keys, one by one; the identity — every rig
      // with no transformed ancestor — leaves them as the file wrote them.
      if (channel.path <= assets::k_path_scale && !ancestors->chain[j].empty()) {
        const std::span<const Transform3> chain = chain_links(j);
        fold_keys(chain, static_cast<u32>(chain.size() - 1), channel.path, sampler, folded_values);
        values = std::span<const f32>(folded_values.data(), folded_values.size());
      }
      if (!asset.clip.add_track(j, channel.path, sampler.interpolation, sampler.components,
                                std::span<const f32>(sampler.times.data(), sampler.times.size()),
                                values)) {
        return fail(error, "animation: clip '" + asset.name + "' has a track whose keys and " +
                               "values do not agree");
      }
    }
    // The channels an animated ancestor moves, built from every track that moves them: the
    // ancestors' and the joint's own. One track of the channel's own kind maps key by key and
    // stays what it was; anything else is resampled (`resample_keys`).
    for (u32 j = 0; ancestor_animated && j < asset.clip.joint_count; ++j) {
      for (u8 c = 0; c < 3; ++c) {
        if (!chain_moves(j, c)) continue;
        const std::span<const Transform3> chain = chain_links(j);
        inputs.clear();
        for (u32 link = 0; link < chain.size(); ++link) {
          const u32 node = link + 1 < chain.size() ? ancestors->chain[j][link]
                                                   : static_cast<u32>(skin->joints[j]);
          for (u8 p = 0; p < 3; ++p) {
            const assets::AnimationSampler* sampler = driven[node * 3u + p];
            if (sampler != nullptr && channel_moves(p, c))
              inputs.push_back(FoldInput{link, p, sampler});
          }
        }
        u8 interpolation = assets::k_interp_linear;
        u8 components = c == assets::k_path_rotation ? 4u : 3u;
        if (inputs.size() == 1 && inputs[0].path == c) {
          const assets::AnimationSampler& sampler = *inputs[0].sampler;
          fold_keys(chain, inputs[0].link, c, sampler, folded_values);
          folded_times.assign(sampler.times.begin(), sampler.times.end());
          interpolation = sampler.interpolation;
          components = sampler.components;
        } else if (!resample_keys(chain, std::span<const FoldInput>(inputs.data(), inputs.size()),
                                  c, interpolation, folded_times, folded_values)) {
          return fail(error, "animation: clip '" + asset.name + "' has an ancestor track whose " +
                                 "keys and values do not agree");
        }
        if (!asset.clip.add_track(
                j, c, interpolation, components,
                std::span<const f32>(folded_times.data(), folded_times.size()),
                std::span<const f32>(folded_values.data(), folded_values.size()))) {
          return fail(error, "animation: clip '" + asset.name + "' has an ancestor track whose " +
                                 "keys and values do not agree");
        }
      }
    }
    if (asset.clip.tracks.empty() && asset.clip.weight_tracks.empty()) continue;
    if (animation.duration > asset.clip.duration) asset.clip.duration = animation.duration;
    if (!asset.clip.validate(error)) return false;
    add_clip(std::move(asset));
    ++local.clips;
  }

  ENGINE_LOG_INFO(log_animation, "clip library loaded", log::field("skeletons", local.skeletons),
                  log::field("clips", local.clips),
                  log::field("skipped_channels", local.skipped_channels));
  if (stats != nullptr) *stats = local;
  return true;
}

bool Library::load_gltf(std::string_view path, std::string_view prefix, LoadStats* stats,
                        std::string* error) {
  assets::MeshData mesh;
  if (!assets::load_gltf(path, mesh, error)) return false;
  const std::string stem = prefix.empty() ? path_stem(path) : std::string(prefix);
  return add(mesh, stem, stats, error);
}

bool Library::load_gltf_memory(std::span<const u8> bytes, std::string_view base_dir,
                               std::string_view prefix, LoadStats* stats, std::string* error) {
  assets::MeshData mesh;
  if (!assets::load_gltf_memory(bytes, base_dir, mesh, error)) return false;
  return add(mesh, prefix, stats, error);
}

bool Library::build_retarget(u32 from, u32 to, anim::Retarget& out) const {
  if (from >= skeletons_.size() || to >= skeletons_.size()) return false;
  return anim::build_retarget(skeletons_[from].skeleton, skeletons_[to].skeleton, out);
}

void Library::clear() noexcept {
  skeletons_.clear();
  clips_.clear();
  skeleton_by_id_.clear();
  clip_by_id_.clear();
}

void joint_influence_bounds(std::span<const Vec3> positions,
                            std::span<const geometry::SkinBinding> skin, u32 joint_count,
                            JointBounds& out) {
  out.sphere.clear();
  out.sphere.resize(joint_count, Vec4{0.0f, 0.0f, 0.0f, -1.0f});
  if (joint_count == 0 || positions.size() != skin.size()) return;
  Vector<Vec3> lo(joint_count, Vec3{1e30f, 1e30f, 1e30f});
  Vector<Vec3> hi(joint_count, Vec3{-1e30f, -1e30f, -1e30f});
  Vector<u32> seen(joint_count, 0u);
  for (usize v = 0; v < positions.size(); ++v) {
    const Vec3 p = positions[v];
    for (u32 k = 0; k < 4; ++k) {
      if (skin[v].weights[k] == 0) continue;
      const u32 joint = skin[v].joints[k];
      if (joint >= joint_count) continue;
      lo[joint] =
          Vec3{std::min(lo[joint].x, p.x), std::min(lo[joint].y, p.y), std::min(lo[joint].z, p.z)};
      hi[joint] =
          Vec3{std::max(hi[joint].x, p.x), std::max(hi[joint].y, p.y), std::max(hi[joint].z, p.z)};
      ++seen[joint];
    }
  }
  for (u32 j = 0; j < joint_count; ++j) {
    if (seen[j] == 0) continue;
    out.sphere[j] = Vec4{(lo[j] + hi[j]) * 0.5f, 0.0f};
  }
  // A second pass for the radius: the box centre is a better centre than a running one, and a
  // sphere around it is tighter than the box's own diagonal for the long thin sets a limb makes.
  for (usize v = 0; v < positions.size(); ++v) {
    for (u32 k = 0; k < 4; ++k) {
      if (skin[v].weights[k] == 0) continue;
      const u32 joint = skin[v].joints[k];
      if (joint >= joint_count || seen[joint] == 0) continue;
      Vec4& sphere = out.sphere[joint];
      sphere.w = std::max(sphere.w, length(positions[v] - sphere.xyz()));
    }
  }
}

f32 clip_displacement_bound(const anim::Skeleton& skeleton, const anim::Clip& clip,
                            const JointBounds& bounds, u32 samples) {
  f32 out = 0.0f;
  const u32 joint_count = skeleton.joint_count();
  if (joint_count == 0) return out;

  // The times to evaluate: every keyframe time of every track, plus a uniform grid. The keyframe
  // times are what makes the answer exact for STEP and LINEAR tracks — a joint's rotation runs
  // along the geodesic between two keys, so what it induces is largest at an end of the interval
  // — and the grid is what catches a CUBICSPLINE track overshooting inside one.
  Vector<f32> times;
  times.reserve(clip.times.size() + samples + 1);
  times.push_back(0.0f);
  for (const f32 t : clip.times) {
    if (t >= 0.0f && t <= clip.duration) times.push_back(t);
  }
  for (u32 i = 0; i < samples && clip.duration > 0.0f; ++i) {
    times.push_back(clip.duration * static_cast<f32>(i) / static_cast<f32>(samples));
  }

  anim::Pose pose;
  Vector<Mat4> model(joint_count, Mat4::identity());
  Vector<anim::JointMatrix> matrices(joint_count, anim::JointMatrix{});
  for (const f32 time : times) {
    anim::rest_pose(skeleton, pose);
    // Clamped, not looped: every time here is inside [0, duration], so the two agree everywhere
    // but at `duration` itself, which a loop wraps to the first key and a clip held at its end
    // (a player that does not loop) stands on. Looped, the last key was never evaluated, and a
    // clip ending on its farthest pose was padded by less than it reaches.
    clip.sample(time, pose, false);
    anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    anim::skinning_matrices(
        std::span<const Mat4>(model.data(), model.size()),
        std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
        std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
    for (u32 j = 0; j < joint_count && j < bounds.sphere.size(); ++j) {
      const Vec4 sphere = bounds.sphere[j];
      if (sphere.w < 0.0f) continue;  // nothing binds to this slot; it moves nothing
      const anim::JointMatrix& m = matrices[j];
      // `anim::JointMatrix` is three rows of the 3x4, so the translation is the rows' `w` and the
      // linear part is their `xyz`. The centre term is kept whole — `(A - I) centre + t`, which is
      // how far this joint moves the centre of what it influences — and only the radius is bounded
      // by a norm. `||A - I||` is the Frobenius norm, at or above the spectral norm the inequality
      // needs: conservative, and nine multiply-adds instead of an eigenvalue.
      f32 sum = 0.0f;
      f32 moved[3] = {};
      const f32 p[3] = {sphere.x, sphere.y, sphere.z};
      for (u32 r = 0; r < 3; ++r) {
        const f32 a[3] = {m.rows[r].x, m.rows[r].y, m.rows[r].z};
        f32 row = m.rows[r].w;  // the translation
        for (u32 c = 0; c < 3; ++c) {
          const f32 d = a[c] - (r == c ? 1.0f : 0.0f);
          sum += d * d;
          row += d * p[c];
        }
        moved[r] = row;
      }
      const f32 displacement =
          length(Vec3{moved[0], moved[1], moved[2]}) + std::sqrt(sum) * sphere.w;
      out = std::max(out, displacement);
    }
  }
  return out;
}

f32 skeleton_displacement_bound(const Library& library, u32 skeleton_index,
                                const JointBounds& bounds, u32 samples) {
  f32 out = 0.0f;
  if (skeleton_index >= library.skeleton_count()) return out;
  const anim::Skeleton& skeleton = library.skeleton_data(skeleton_index);
  // The rest pose itself first, through a clip with no tracks: a skin whose inverse binds do not
  // undo its joints' rest — a root joint under a transformed ancestor the file's inverse binds
  // leave out (S92) — moves the mesh off its bind-space positions before any clip plays, and a
  // character that holds its rest pose has to be padded by that.
  out = clip_displacement_bound(skeleton, anim::Clip{}, bounds, 0);
  for (u32 i = 0; i < library.clip_count(); ++i) {
    if (library.clip(i).skeleton != skeleton_index) continue;
    out = std::max(out, clip_displacement_bound(skeleton, library.clip_data(i), bounds, samples));
  }
  return out;
}

}  // namespace engine::animation
