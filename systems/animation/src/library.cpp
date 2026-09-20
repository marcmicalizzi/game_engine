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
  if (mesh.skins.empty())
    return fail(error, "animation: the mesh has no skin to build a skeleton from");

  LoadStats local;
  // The skin palette index of every node, so an animation channel's node becomes a joint in one
  // lookup rather than a search per channel per skin.
  Vector<Vector<i32>> node_to_joint(mesh.skins.size());
  Vector<u32> skeleton_index(mesh.skins.size(), 0u);

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
    for (u32 j = 0; j < joints; ++j) {
      const assets::Node& node = mesh.nodes[static_cast<u32>(skin.joints[j])];
      bones.names[j] = node.name;
      bones.local_bind[j] = node.local;
      const i32 parent_node = node.parent;
      const i32 parent_joint =
          parent_node >= 0 ? to_joint[static_cast<u32>(parent_node)] : anim::k_no_joint;
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
        if (node_to_joint[s][static_cast<u32>(channel.node)] != anim::k_no_joint) ++hits;
      }
      if (hits > best_hits) {
        best_hits = hits;
        best_skin = s;
      }
    }
    if (best_hits == 0) {
      local.skipped_channels += animation.channels.size();
      continue;
    }

    ClipAsset asset;
    asset.name = qualify(prefix, animation.name, "animation", a);
    asset.id = clip_id(asset.name);
    asset.skeleton = skeleton_index[best_skin];
    asset.clip.name = asset.name;
    asset.clip.joint_count = skeletons_[asset.skeleton].skeleton.joint_count();

    const Vector<i32>& to_joint = node_to_joint[best_skin];
    for (const assets::AnimationChannel& channel : animation.channels) {
      if (channel.node < 0 || static_cast<u32>(channel.node) >= mesh.nodes.size() ||
          channel.sampler >= animation.samplers.size()) {
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
      const i32 joint = to_joint[static_cast<u32>(channel.node)];
      if (joint == anim::k_no_joint) {
        ++local.skipped_channels;
        continue;
      }
      const assets::AnimationSampler& sampler = animation.samplers[channel.sampler];
      if (!asset.clip.add_track(
              static_cast<u32>(joint), channel.path, sampler.interpolation, sampler.components,
              std::span<const f32>(sampler.times.data(), sampler.times.size()),
              std::span<const f32>(sampler.values.data(), sampler.values.size()))) {
        return fail(error, "animation: clip '" + asset.name + "' has a track whose keys and " +
                               "values do not agree");
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
    clip.sample(time, pose, true);
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
  for (u32 i = 0; i < library.clip_count(); ++i) {
    if (library.clip(i).skeleton != skeleton_index) continue;
    out = std::max(out, clip_displacement_bound(skeleton, library.clip_data(i), bounds, samples));
  }
  return out;
}

}  // namespace engine::animation
