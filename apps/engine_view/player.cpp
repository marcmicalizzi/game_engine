// The player's body (player.h; docs/subsystems/apps.md, "The player's body"; roadmap P52).
#include "player.h"

#if ENGINE_VIEW_ANIMATION && ENGINE_VIEW_GAME

#include "walk.h"

#include <domain/assets/gltf.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/animation/library.h>

#include <cstdio>
#include <span>

namespace engine::view {

namespace {

tunables::Float player_scale{"view.player.scale", 1.0, 0.1, 10.0,
                             "The player's body's standing height as a multiple of "
                             "view.walk.height (--player)"};
tunables::Bool player_collapse_head{"view.player.collapse_head", true,
                                    "First person: collapse the player's head joint and the joints "
                                    "below it to the head's position (--player)"};
tunables::String player_walk_clip{
    "view.player.walk_clip", "walk",
    "The clip the player's body plays while the walker moves, by name, any case (--player); a "
    "name the rig lacks plays its first clip; 'none' plays no clip, the body stays at rest"};
tunables::String player_idle_clip{
    "view.player.idle_clip", "idle",
    "The clip the player's body plays while the walker stands, by name, any case (--player); a "
    "name the rig lacks, or 'none', stands the rest pose"};

// The library's own name for a file's assets, as animation.cpp's `path_stem` gives it, so a clip
// named here is the one the animated world's library holds.
std::string stem_of(std::string_view path) {
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

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// A clip's name without the library's `<stem>/` prefix equals `want`, ignoring case.
bool clip_named(const std::string& name, std::string_view want) noexcept {
  const usize slash = name.rfind('/');
  const std::string_view bare = slash == std::string::npos
                                    ? std::string_view(name)
                                    : std::string_view(name).substr(slash + 1);
  if (bare.size() != want.size()) return false;
  for (usize i = 0; i < bare.size(); ++i) {
    if (lower(bare[i]) != lower(want[i])) return false;
  }
  return true;
}

// The tunables' `none`: no clip, any case.
bool names_no_clip(const std::string& want) noexcept { return clip_named(want, "none"); }

constexpr f32 k_gait_fade_s = 0.2f;  // walking to standing and back

}  // namespace

i32 player_head_joint(const anim::Skeleton& skeleton, const anim::JointMapping& mapping,
                      const char*& from) noexcept {
  from = "none";
  const i32 head = mapping[anim::StandardJoint::Head];
  if (head != anim::k_no_joint) {
    from = "Head";
    return head;
  }
  // No joint named for a head: a neck chain that ends in one child is the head's (RiggedFigure's
  // `neck_joint_1` -> `neck_joint_2`, which the head's vertices are skinned to).
  const i32 neck = mapping[anim::StandardJoint::Neck];
  if (neck == anim::k_no_joint) return -1;
  i32 child = -1;
  u32 children = 0;
  for (u32 j = 0; j < skeleton.joint_count(); ++j) {
    if (skeleton.parents[j] == neck) {
      child = static_cast<i32>(j);
      ++children;
    }
  }
  if (children != 1) return -1;
  from = "Neck's one child";
  return child;
}

void player_collapse_mask(const anim::Skeleton& skeleton, i32 head, Vector<u32>& out) {
  out.clear();
  if (head < 0 || static_cast<u32>(head) >= skeleton.joint_count()) return;
  // A joint's parent comes before it (skeleton.h), so one forward pass finds the subtree.
  Vector<u8> in(skeleton.joint_count(), u8{0});
  for (u32 j = 0; j < skeleton.joint_count(); ++j) {
    const i32 parent = skeleton.parents[j];
    if (static_cast<i32>(j) == head ||
        (parent >= 0 && static_cast<u32>(parent) < j && in[static_cast<u32>(parent)] != 0)) {
      in[j] = 1;
      out.push_back(j);
    }
  }
}

bool measure_player_rig(const std::string& path, f32 target_height_m, f32 eye_height_m,
                        PlayerRig& out, std::string& error) {
  out = PlayerRig{};
  out.mesh = path;
  out.eye_height_m = eye_height_m;
  out.target_height_m = target_height_m * static_cast<f32>(player_scale.get());
  if (io::extension(path) == ".clusters") {
    error = "--player " + path +
            " is a .clusters container, which carries the skin's bindings but not its skeleton or "
            "its clips; name the skinned glTF it was built from";
    return false;
  }
  assets::MeshData mesh;
  std::string why;
  if (!assets::load_gltf(path, mesh, &why)) {
    error = "--player " + path + " cannot be read as glTF: " + why;
    return false;
  }
  if (mesh.skins.empty() || mesh.skin_bindings.size() != mesh.positions.size() ||
      mesh.positions.empty()) {
    error = "--player " + path +
            " has no skin: the player's body is a rigged character (a skinned glTF), and this "
            "mesh would stand rigid";
    return false;
  }
  animation::Library library;
  animation::LoadStats stats;
  if (!library.add(mesh, stem_of(path), &stats, &error) || library.skeleton_count() == 0) {
    error = "--player " + path + ": " + (error.empty() ? std::string("no skeleton") : error);
    return false;
  }
  const animation::SkeletonAsset& asset = library.skeleton(0);
  const anim::Skeleton& skeleton = asset.skeleton;
  out.joints = skeleton.joint_count();
  // The bind pose as drawn: the rest skinning matrices over the vertices.
  anim::Pose rest;
  anim::rest_pose(skeleton, rest);
  Vector<Mat4> model(out.joints);
  anim::local_to_model(skeleton, rest, std::span<Mat4>(model.data(), model.size()));
  Vector<anim::JointMatrix> matrices(out.joints);
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
  Vector<Vec3> skinned(mesh.positions.size());
  anim::skin_positions(
      std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
      std::span<const geometry::SkinBinding>(mesh.skin_bindings.data(), mesh.skin_bindings.size()),
      std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
      std::span<Vec3>(skinned.data(), skinned.size()));
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (const Vec3 p : skinned) {
    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  out.bind_lo = lo;
  out.bind_hi = hi;
  out.bind_height = hi.y - lo.y;
  if (!(out.bind_height > 0.0f)) {
    error = "--player " + path + " stands no height in its bind pose";
    return false;
  }
  out.scale = out.target_height_m / out.bind_height;
  out.origin_above_feet_m = -lo.y * out.scale;
  out.head = player_head_joint(skeleton, asset.mapping, out.head_from);
  if (out.head >= 0) {
    const u32 h = static_cast<u32>(out.head);
    out.head_name = h < skeleton.names.size() ? skeleton.names[h] : std::string();
    out.head_bind = transform_point(model[h], Vec3{});
    // The body faces +z, turned a half turn to the walker's heading: its right is -x, its ahead -z.
    // The eye is at the walker's feet (the body's origin less `origin_above_feet_m`) and
    // `eye_height_m` up.
    const Vec3 head = out.head_bind * out.scale;
    const f32 up = eye_height_m - out.origin_above_feet_m - head.y;
    out.eye_from_head = Vec3{head.x, up, head.z};
  }
  // The clips by the tunables' names (`view.player.walk_clip`, `idle_clip`; the defaults are the
  // names this always looked for). Read here, once, before any device.
  const std::string want_walk = player_walk_clip.get();
  const std::string want_idle = player_idle_clip.get();
  const bool walk_none = names_no_clip(want_walk);
  const bool idle_none = names_no_clip(want_idle);
  for (u32 i = 0; i < library.clip_count(); ++i) {
    const animation::ClipAsset& clip = library.clip(i);
    if (clip.skeleton != 0) continue;
    if (!walk_none && out.walk_clip.empty() && clip_named(clip.name, want_walk))
      out.walk_clip = clip.name;
    if (!idle_none && out.idle_clip.empty() && clip_named(clip.name, want_idle))
      out.idle_clip = clip.name;
  }
  // A walk the rig does not name plays its first clip (an unnamed one is the walk of RiggedFigure);
  // an idle it does not name is the rest pose. `none` is neither: it plays nothing.
  if (out.walk_clip.empty() && !walk_none) {
    for (u32 i = 0; i < library.clip_count(); ++i) {
      if (library.clip(i).skeleton == 0) {
        out.walk_clip = library.clip(i).name;
        break;
      }
    }
  }
  return true;
}

DeclaredActor PlayerBody::declared() const {
  DeclaredActor d;
  d.name = k_player_name;
  d.instance = k_player_actor;
  d.read = read_;
  return d;
}

void PlayerBody::append(renderer::SceneDesc& desc, const WorldPos& start) {
  const u32 mesh = static_cast<u32>(desc.meshes.size());
  desc.meshes.push_back(rig_.mesh);
  if (!desc.mesh_info.empty() || mesh == 0) {
    // `mesh_info` is empty or parallel to `meshes` (scene.h): keep it parallel.
    if (desc.mesh_info.size() < mesh) desc.mesh_info.resize(mesh);
    renderer::SceneMeshInfo info;
    info.name = k_player_name;
    desc.mesh_info.push_back(std::move(info));
  }
  renderer::SceneInstance instance;
  instance.mesh = mesh;
  instance.origin = start;
  instance.transform.scale = Vec3{rig_.scale, rig_.scale, rig_.scale};
  instance.animation.play = true;  // its first clip; the session picks walk or rest at tick 1
  read_ = static_cast<u32>(desc.instances.size());
  desc.instances.push_back(instance);
}

void PlayerBody::chain(FlySession& session) noexcept {
  next_ = session.tick_observer();
  next_user_ = session.tick_observer_user();
  session.set_tick_observer(&PlayerBody::observe, this);
}

bool PlayerBody::bind(AnimatedScene& scene, GameActors& actors, std::string& error) {
  scene_ = &scene;
  actors_ = &actors;
  hotload::ActorState state;
  if (actors.state(k_player_actor, state) != hotload::Result::Ok) {
    error = "--player " + rig_.mesh + ": the body is not an actor of the animated world";
    return false;
  }
  // The loaded instance the actors bound it to (the same map they used).
  const u32 loaded = read_ < scene.loaded_of_read.size() ? scene.loaded_of_read[read_] : read_;
  mask_.clear();
  collapse_ = false;
  if (loaded >= scene.entities.size() || scene.entities[loaded].is_null()) {
    error = "--player " + rig_.mesh + ": the body's instance does not animate";
    return false;
  }
  const u32 skeleton = loaded < scene.instance_clip.size() &&
                               scene.instance_clip[loaded] < scene.library.clip_count()
                           ? scene.library.clip(scene.instance_clip[loaded]).skeleton
                           : 0u;
  if (skeleton >= scene.library.skeleton_count()) return true;
  const animation::SkeletonAsset& asset = scene.library.skeleton(skeleton);
  const char* from = "none";
  const i32 head = player_head_joint(asset.skeleton, asset.mapping, from);
  if (head < 0) {
    std::fprintf(stderr,
                 "engine-view: player: %s maps no Head joint (and no neck with one child): the "
                 "head is not collapsed, and a first-person camera may see inside it\n",
                 rig_.mesh.c_str());
    return true;
  }
  player_collapse_mask(asset.skeleton, head, mask_);
  set_joint_collapse(scene, loaded, asset.skeleton, static_cast<u32>(head),
                     std::span<const u32>(mask_.data(), mask_.size()));
  collapse_ = true;
  set_joint_collapse_on(scene, player_collapse_head.get());
  return true;
}

void PlayerBody::observe(void* user, const FlySession& session, const input::InputState& input) {
  auto* self = static_cast<PlayerBody*>(user);
  self->tick(session);
  if (self->next_ != nullptr) self->next_(self->next_user_, session, input);
}

// **After the walker's step, before the module's tick and the world's step**: the body at the
// walker's feet, turned to its heading (the look's yaw, never its pitch), level; the walk clip
// while it moves over the ground, the rest pose (or `idle`) while it stands. Flying, nothing: the
// body stays where the walker last stood. Everything here is a function of the walker's state at
// the tick, so a recorded walk replays with the body where it was.
void PlayerBody::tick(const FlySession& session) {
  if (actors_ == nullptr) return;
  const Walker* walker = session.walker();
  if (walker == nullptr || !session.walking()) {
    ++flying_ticks_;
    if (walker != nullptr) walked_ = walker->stats().distance_m;
    return;
  }
  const WorldPos feet = walker->feet();
  const f64 at[3] = {feet.x, feet.y + static_cast<f64>(rig_.origin_above_feet_m), feet.z};
  const f32 yaw = session.state().yaw + rig_.facing_yaw;
  if (actors_->place(k_player_actor, at, yaw) == hotload::Result::Ok) {
    ++places_;
  } else {
    ++refused_;
  }
  const f64 walked = walker->stats().distance_m;
  const bool moving = walked - walked_ > 0.0;
  walked_ = walked;
  const u8 gait = moving ? u8{1} : u8{2};
  if (gait == gait_) {
    if (moving) ++walking_ticks_;
    return;
  }
  // The first tick cuts; a change after it fades.
  const f32 fade = gait_ == 0 ? 0.0f : k_gait_fade_s;
  if (gait_ != 0) ++gait_changes_;
  gait_ = gait;
  if (moving) {
    ++walking_ticks_;
    // No walk clip (`view.player.walk_clip=none`): the body is at rest while it walks, so what it
    // played before (the first clip it started on, or the idle) is stopped, as standing does.
    if (!rig_.walk_clip.empty()) {
      (void)actors_->play(k_player_actor, rig_.walk_clip, fade, true);
    } else {
      (void)actors_->stop(k_player_actor, fade);
    }
  } else if (!rig_.idle_clip.empty()) {
    (void)actors_->play(k_player_actor, rig_.idle_clip, fade, true);
  } else {
    (void)actors_->stop(k_player_actor, fade);
  }
}

JsonValue PlayerBody::summary_json(const GameActors* actors) const {
  JsonValue out = JsonValue::object();
  const auto vec = [](Vec3 v) {
    JsonValue a = JsonValue::array();
    a.push_back(static_cast<f64>(v.x));
    a.push_back(static_cast<f64>(v.y));
    a.push_back(static_cast<f64>(v.z));
    return a;
  };
  out.set("mesh", rig_.mesh);
  out.set("instance", static_cast<u64>(read_));
  out.set("joints", static_cast<u64>(rig_.joints));
  out.set("scale", static_cast<f64>(rig_.scale));
  out.set("height_m", static_cast<f64>(rig_.target_height_m));
  out.set("bind_height", static_cast<f64>(rig_.bind_height));
  out.set("origin_above_feet_m", static_cast<f64>(rig_.origin_above_feet_m));
  out.set("facing_yaw", static_cast<f64>(rig_.facing_yaw));
  out.set("head_joint", rig_.head_name);
  out.set("head_from", rig_.head_from);
  out.set("eye_height_m", static_cast<f64>(rig_.eye_height_m));
  // The eye less the head joint, in the body's frame: x its right, y up, z ahead.
  if (rig_.head >= 0) out.set("eye_from_head_m", vec(rig_.eye_from_head));
  out.set("collapse_head", collapse_ && scene_ != nullptr && scene_->collapse_on);
  out.set("collapsed_joints", static_cast<u64>(mask_.size()));
  out.set("collapse_steps", scene_ != nullptr ? scene_->collapse_steps : u64{0});
  // `none` where no clip plays: the tunable said so, or the rig has no idle (the rest pose).
  out.set("walk_clip", rig_.walk_clip.empty() ? std::string("none") : rig_.walk_clip);
  out.set("idle_clip", rig_.idle_clip.empty() ? std::string("none") : rig_.idle_clip);
  out.set("places", places_);
  out.set("refused", refused_);
  out.set("walking_ticks", walking_ticks_);
  out.set("gait_changes", gait_changes_);
  // A flying session is a developer's view: the body stays where the walker last stood.
  out.set("flying_ticks", flying_ticks_);
  if (actors != nullptr) {
    const JsonValue all = actors->summary_json();
    for (u32 i = 0; all.is_array() && i < all.size(); ++i) {
      const JsonValue* name = all[i].find("name");
      if (name != nullptr && name->is_string() && name->as_string() == k_player_name)
        out.set("actor", all[i]);
    }
  }
  return out;
}

}  // namespace engine::view

#endif  // ENGINE_VIEW_ANIMATION && ENGINE_VIEW_GAME
