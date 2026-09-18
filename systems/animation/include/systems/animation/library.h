#pragma once

// The clip library: the skeletons and clips this capability plays, loaded from glTF through
// `domain/assets` and addressed by `Id128`.
//
// **Why the capability owns it and `domain/anim` does not.** `anim` is the data half — a
// `Skeleton`, a `Clip`, the blends over a pose — and deliberately has no notion of an asset, a
// name, or a load. Something has to turn `assets::MeshData`'s skins and animation curves into
// those types and give them names that survive a save, and that something is a capability
// concern: which clips a game has is a game's business, and a build without this capability has
// no reason to link glTF import to skeletons at all.
//
// **Why `Id128` and not a pointer or an index.** `AnimationPlayer::clip` is persistent state
// (ADR-0028 seam 3: across a tick, across a save, across the wire, name an `Id128`). The ids are
// derived from the asset's own name with a stable hash, so the same file gives the same ids in
// every process and on every machine, and a save written by one build resolves in the next.
// Index lookups for the hot path are resolved once, when an instance is attached.
//
// **The standard-skeleton mapping is computed once per skeleton, at load.** `anim::map_joints` is
// three passes of normalized string matching over every joint (docs/subsystems/anim.md, "The
// standard skeleton"); doing it per instance, or per retarget, would be paying a load cost every
// tick. What it buys is that a clip authored against the 23-role humanoid core can be retargeted
// onto any skeleton in this library without the caller knowing either rig's joint names.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <domain/anim/clip.h>
#include <domain/anim/skeleton.h>
#include <domain/anim/standard_skeleton.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::assets {
struct MeshData;
}

namespace engine::animation {

// What one load added. `skipped_channels` counts animation channels whose target node is not a
// joint of the skin the clip was matched to — a real thing in real files (a camera track, a prop
// on the same timeline), and a number worth seeing rather than a silent drop.
struct LoadStats {
  u32 skeletons = 0;
  u32 clips = 0;
  u32 skipped_channels = 0;
};

// One skeleton of the library, with the standard-skeleton mapping computed at load.
struct SkeletonAsset {
  Id128 id;
  std::string name;
  anim::Skeleton skeleton;
  anim::JointMapping mapping;
  u32 mapped_roles = 0;  // how many of the 23 roles this rig filled; a content-quality signal
};

struct ClipAsset {
  Id128 id;
  std::string name;
  anim::Clip clip;
  u32 skeleton = 0;  // index into the library's skeletons
};

class Library {
 public:
  // Loads every skin and animation of a glTF or GLB file. Additive: a second call adds to what is
  // already here, and a name that is already taken replaces that asset in place so its `Id128`
  // (and therefore every save that names it) stays valid.
  //
  // **`prefix` is part of the asset's name and therefore of its id, and an empty one means the
  // file's stem** — never the path. The path is where the file happened to be on the machine that
  // loaded it, and an id derived from it would differ between a developer's tree, a build server
  // and a player's install, which would make every save unloadable somewhere else. Two files with
  // the same stem and a skin of the same name collide, and the answer to that is to pass a prefix,
  // which is why the parameter is there.
  bool load_gltf(std::string_view path, std::string_view prefix = {}, LoadStats* stats = nullptr,
                 std::string* error = nullptr);
  bool load_gltf_memory(std::span<const u8> bytes, std::string_view base_dir,
                        std::string_view prefix = {}, LoadStats* stats = nullptr,
                        std::string* error = nullptr);
  // The same from an already-imported mesh, which is what a content build hands over and what a
  // test builds directly.
  bool add(const assets::MeshData& mesh, std::string_view prefix, LoadStats* stats = nullptr,
           std::string* error = nullptr);

  // The stable id of a named asset, whether or not this library holds it. Deterministic: a pure
  // function of the name, so an id written into a save resolves in any process that loaded the
  // same content.
  static Id128 skeleton_id(std::string_view name) noexcept;
  static Id128 clip_id(std::string_view name) noexcept;

  u32 skeleton_count() const noexcept { return skeletons_.size(); }
  u32 clip_count() const noexcept { return clips_.size(); }
  // `k_not_found` when the library does not hold it. Cold: resolved once per instance.
  u32 skeleton_index(const Id128& id) const noexcept;
  u32 clip_index(const Id128& id) const noexcept;
  static constexpr u32 k_not_found = 0xFFFF'FFFFu;

  // The hot-path form: the index for `id`, given the index it had last time. A cached index that
  // still names `id` costs a bounds check and a 16-byte compare; only a clip that actually changed
  // pays for a hash lookup. This is what keeps an `Id128` handle — the right thing to save and the
  // wrong thing to hash per instance per tick — out of the sampling loop's cost.
  u32 resolve_clip(const Id128& id, u32 cached) const noexcept {
    if (id.is_null()) return k_not_found;
    if (cached < clips_.size() && clips_[cached].id == id) return cached;
    return clip_index(id);
  }

  const SkeletonAsset& skeleton(u32 index) const noexcept { return skeletons_[index]; }
  const ClipAsset& clip(u32 index) const noexcept { return clips_[index]; }
  // The hot-path accessors the sampling systems use, after the index has been resolved once.
  const anim::Skeleton& skeleton_data(u32 index) const noexcept {
    return skeletons_[index].skeleton;
  }
  const anim::Clip& clip_data(u32 index) const noexcept { return clips_[index].clip; }

  // A retarget from one of this library's skeletons onto another, through the mappings computed
  // at load. False when the two share no standard-skeleton role, which is a content error.
  bool build_retarget(u32 from, u32 to, anim::Retarget& out) const;

  void clear() noexcept;

 private:
  u32 add_skeleton(SkeletonAsset&& asset);
  u32 add_clip(ClipAsset&& asset);

  Vector<SkeletonAsset> skeletons_;
  Vector<ClipAsset> clips_;
  HashMap<Id128, u32> skeleton_by_id_;
  HashMap<Id128, u32> clip_by_id_;
};

}  // namespace engine::animation
