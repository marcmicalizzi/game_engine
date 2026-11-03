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
// `auto` collapses the head under the walker's eye (inside the skull) and draws it whole under the
// face's (P58); `true` and `false` force it either way.
enum class CollapseHead : u32 { Auto, True, False };
constexpr const char* k_collapse_choices[] = {"auto", "true", "false"};
tunables::Enum<CollapseHead> player_collapse_head{
    "view.player.collapse_head", CollapseHead::Auto, k_collapse_choices,
    "First person: collapse the player's head joint and the joints below it to the head's "
    "position (--player); auto: under view.player.eye=walker only"};
enum class EyeRule : u32 { Face, Walker };
constexpr const char* k_eye_choices[] = {"face", "walker"};
tunables::Enum<EyeRule> player_eye{
    "view.player.eye", EyeRule::Face, k_eye_choices,
    "First person (--player): face puts the camera just past the face's furthest forward point at "
    "the eye line, carried by the posed head; walker keeps the walker's eye and collapses the "
    "head"};
tunables::Float player_eye_step{
    "view.player.eye_step_m", 0.02, 0.0, 0.5,
    "view.player.eye=face: metres past the face's furthest forward point the eye stands"};
tunables::Float player_eye_line{
    "view.player.eye_line", 0.48, -1.0, 1.0,
    "view.player.eye=face: the eye line's depth below the top of the head's vertices, as a "
    "fraction of their height (Farkas's canon: about half); negative keeps the furthest point's "
    "own height. A rig with eye joints uses their mean height"};
tunables::Bool player_head_look{
    "view.player.head_look", true,
    "view.player.eye=face: the look's pitch turns the neck and the head within the cervical "
    "ranges, the torso past them (--player)"};
tunables::Float player_neck_flexion{
    "view.player.neck_flexion_deg", 57.7, 0.0, 120.0,
    "The cervical spine's flexion (looking down) the head-look layer holds the neck and head to, "
    "degrees (Apti et al. 2023: 57.7 in young adults)"};
tunables::Float player_neck_extension{
    "view.player.neck_extension_deg", 59.1, 0.0, 120.0,
    "The cervical spine's extension (looking up), degrees (Apti et al. 2023: 59.1)"};
tunables::Float player_neck_rotation{
    "view.player.neck_rotation_deg", 70.7, 0.0, 120.0,
    "The cervical spine's axial rotation each way, degrees (Apti et al. 2023: 71.1 right, 70.2 "
    "left)"};
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

constexpr f32 k_deg = 0.017453292519943295f;

bool names_an_eye(const std::string& name) noexcept {
  for (usize i = 0; i + 3 <= name.size(); ++i) {
    if (lower(name[i]) == 'e' && lower(name[i + 1]) == 'y' && lower(name[i + 2]) == 'e')
      return true;
  }
  return false;
}

bool is_ancestor(const anim::Skeleton& skeleton, i32 ancestor, i32 joint) noexcept {
  if (ancestor < 0) return false;
  for (i32 j = joint; j >= 0 && static_cast<u32>(j) < skeleton.joint_count();
       j = skeleton.parents[static_cast<u32>(j)]) {
    if (j == ancestor) return true;
  }
  return false;
}

// The look's pivots under `head`: the neck (the standard skeleton's `Neck`, when it is above the
// head), else none; the torso (the parent of the neck, else of the head).
void look_pivots(const anim::Skeleton& skeleton, const anim::JointMapping& mapping, i32 head,
                 i32& torso, i32& neck) noexcept {
  torso = -1;
  neck = -1;
  if (head < 0) return;
  const i32 mapped = mapping[anim::StandardJoint::Neck];
  if (mapped != anim::k_no_joint && mapped != head && is_ancestor(skeleton, mapped, head))
    neck = mapped;
  const i32 below = neck >= 0 ? neck : head;
  torso = skeleton.parents[static_cast<u32>(below)];
  if (torso < 0) torso = -1;
}

f32 clamp_to(f32 v, f32 lo, f32 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

LookShares player_look_shares(const PlayerRig& rig, f32 pitch, f32 yaw_relative) noexcept {
  LookShares out;
  const f32 down = -pitch;  // the session's pitch is up positive; a flexion turns the face down
  const f32 cervical = clamp_to(down, -rig.extension, rig.flexion);
  const f32 turned = clamp_to(yaw_relative, -rig.rotation, rig.rotation);
  // The head (the craniocervical junction) takes its part; the neck the rest of the range, or
  // the head all of it where the rig has no neck joint; the torso what is past the range.
  if (rig.neck >= 0) {
    out.down[2] = cervical * k_head_flexion_share;
    out.down[1] = cervical - out.down[2];
    out.turn[2] = turned * k_head_rotation_share;
    out.turn[1] = turned - out.turn[2];
  } else {
    out.down[2] = cervical;
    out.turn[2] = turned;
  }
  if (rig.torso >= 0) {
    out.down[0] = down - cervical;
    out.turn[0] = yaw_relative - turned;
  }
  return out;
}

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
  // **The eye at the face** (P58; apps.md "The eye at the face"), from the rig alone: over the
  // vertices weighted to the head's subtree above the neck joint, in the skinned bind pose (the
  // body faces +z there), the furthest forward point, `eye_step_m` past it, raised to the eye line
  // — the eye joints' mean height where the head's subtree names one, else `eye_line` of the way
  // down the head's vertices' box, else (negative) the point's own height.
  out.eye_step_m = static_cast<f32>(player_eye_step.get());
  out.eye_line = static_cast<f32>(player_eye_line.get());
  out.flexion = static_cast<f32>(player_neck_flexion.get()) * k_deg;
  out.extension = static_cast<f32>(player_neck_extension.get()) * k_deg;
  out.rotation = static_cast<f32>(player_neck_rotation.get()) * k_deg;
  out.head_rest = out.head >= 0 ? matrices[static_cast<u32>(out.head)] : anim::JointMatrix{};
  look_pivots(skeleton, asset.mapping, out.head, out.torso, out.neck);
  if (out.torso >= 0 && static_cast<u32>(out.torso) < skeleton.names.size())
    out.torso_name = skeleton.names[static_cast<u32>(out.torso)];
  if (out.neck >= 0 && static_cast<u32>(out.neck) < skeleton.names.size())
    out.neck_name = skeleton.names[static_cast<u32>(out.neck)];
  // A joint where it stands at rest, in the body's frame (x its right, y from the soles, z ahead).
  const auto body_point = [&](i32 joint) {
    if (joint < 0) return Vec3{};
    const Vec3 p = transform_point(model[static_cast<u32>(joint)], Vec3{}) * out.scale;
    return Vec3{-p.x, p.y + out.origin_above_feet_m, p.z};
  };
  out.torso_point = body_point(out.torso);
  out.neck_point = body_point(out.neck);
  out.head_point = body_point(out.head);
  if (out.head >= 0 && player_eye.get() == EyeRule::Face) {
    Vector<u32> subtree;
    player_collapse_mask(skeleton, out.head, subtree);
    Vector<u8> in_head(out.joints, u8{0});
    for (const u32 j : subtree)
      in_head[j] = 1;
    Vec3 box_lo{1e30f, 1e30f, 1e30f};
    Vec3 box_hi{-1e30f, -1e30f, -1e30f};
    Vec3 tip{};
    // Any weight on the head's subtree, at or above the neck joint: a rig may blend the whole head
    // between the neck and the head (RiggedFigure: only its crown is weighted mostly to
    // `neck_joint_2`), and one may weight its neck and upper chest to the head (the owner's
    // authored bodies, P57), which the neck joint's height leaves out — the chest stands further
    // forward than the nose and would take the eye down to it.
    const f32 floor_y =
        out.neck >= 0 ? transform_point(model[static_cast<u32>(out.neck)], Vec3{}).y : -1e30f;
    for (u32 v = 0; v < skinned.size(); ++v) {
      const geometry::SkinBinding& b = mesh.skin_bindings[v];
      u32 weight = 0;
      for (u32 k = 0; k < 4; ++k) {
        if (b.joints[k] < out.joints && in_head[b.joints[k]] != 0) weight += b.weights[k];
      }
      const Vec3 p = skinned[v];
      if (weight == 0 || p.y < floor_y) continue;
      if (out.head_vertices == 0 || p.z > tip.z) tip = p;
      ++out.head_vertices;
      box_lo = Vec3{std::min(box_lo.x, p.x), std::min(box_lo.y, p.y), std::min(box_lo.z, p.z)};
      box_hi = Vec3{std::max(box_hi.x, p.x), std::max(box_hi.y, p.y), std::max(box_hi.z, p.z)};
    }
    if (out.head_vertices > 0) {
      // The box in the body's frame (x its right: the rig's -x), for the summary.
      out.head_lo = Vec3{-box_hi.x * out.scale, box_lo.y * out.scale + out.origin_above_feet_m,
                         box_lo.z * out.scale};
      out.head_hi = Vec3{-box_lo.x * out.scale, box_hi.y * out.scale + out.origin_above_feet_m,
                         box_hi.z * out.scale};
      f32 eyes = 0.0f;
      u32 eye_joints = 0;
      for (const u32 j : subtree) {
        if (j < skeleton.names.size() && names_an_eye(skeleton.names[j])) {
          eyes += transform_point(model[j], Vec3{}).y;
          ++eye_joints;
        }
      }
      f32 height = tip.y;
      if (eye_joints > 0) {
        height = eyes / static_cast<f32>(eye_joints);
        out.eye_from = "eyes";
      } else if (out.eye_line >= 0.0f) {
        height = box_hi.y - out.eye_line * (box_hi.y - box_lo.y);
        out.eye_from = "head";
      } else {
        out.eye_from = "face";
      }
      // On the head's midline (the box's centre: a nose is on it, and a blocky head's furthest
      // points are a whole face wide), at the furthest point's depth plus the step.
      const Vec3 at{0.5f * (box_lo.x + box_hi.x), height, tip.z + out.eye_step_m / out.scale};
      out.eye_bind = transform_point(inverse(anim::mat4_from_joint(out.head_rest)), at);
      out.eye_point =
          Vec3{-at.x * out.scale, at.y * out.scale + out.origin_above_feet_m, at.z * out.scale};
      out.eye_face = true;
      out.head_look = player_head_look.get();
    }
  }
  const CollapseHead collapse = player_collapse_head.get();
  out.collapse_head =
      collapse == CollapseHead::Auto ? !out.eye_face : collapse == CollapseHead::True;
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
  entry_ = static_cast<u32>(scene.collapse.size() - 1);
  collapse_ = rig_.collapse_head;
  // The head-look layer under the face's eye (P58): the torso, the neck and the head, outermost
  // first, the joints `measure_player_rig` found in the same file. Under the walker's eye, none.
  Vector<u32> pivots;
  if (rig_.eye_face && rig_.head_look) {
    if (rig_.torso >= 0) pivots.push_back(static_cast<u32>(rig_.torso));
    if (rig_.neck >= 0) pivots.push_back(static_cast<u32>(rig_.neck));
    pivots.push_back(static_cast<u32>(head));
  }
  look_ = !pivots.empty();
  set_joint_look(scene, entry_, asset.skeleton, collapse_,
                 std::span<const u32>(pivots.data(), pivots.size()));
  // The copy is handed when it carries something: the collapse, or the look.
  set_joint_collapse_on(scene, collapse_ || look_);
  return true;
}

bool PlayerBody::face_camera(const FlyState& drawn, f32 eye_height, f32 fov_y, f32 znear,
                             renderer::Camera& out) {
  if (!rig_.eye_face || scene_ == nullptr || entry_ >= scene_->collapse.size()) return false;
  // The look as the frame draws it. In first person the body turns to the heading, so the look's
  // yaw relative to the body is zero; the term is there for a camera that does not turn the body.
  const LookShares shares = player_look_shares(rig_, drawn.pitch, 0.0f);
  if (look_) {
    // The pivots set in `bind` are a prefix of torso, neck, head: the shares of the ones present.
    f32 down[3];
    f32 turn[3];
    u32 n = 0;
    for (u32 i = 0; i < 3; ++i) {
      const bool present = i == 2 || (i == 0 ? rig_.torso >= 0 : rig_.neck >= 0);
      if (!present) continue;
      down[n] = shares.down[i];
      turn[n] = shares.turn[i];
      ++n;
    }
    set_joint_look_angles(*scene_, entry_, std::span<const f32>(down, n),
                          std::span<const f32>(turn, n));
  }
  const AnimatedScene::JointCollapse& c = scene_->collapse[entry_];
  const anim::JointMatrix& head = c.head_posed_valid ? c.head_posed : rig_.head_rest;
  const Vec3 eye = anim::transform_point(head, rig_.eye_bind);  // mesh units, the body's frame
  // Scaled as the actor is (a module's `actor_scale` steps it about the feet), placed as the
  // session places it: at the walker's feet, its origin `origin_above_feet_m` up, turned to the
  // heading plus a half turn, level.
  f32 scale = rig_.scale;
  if (actors_ != nullptr) {
    hotload::ActorState state;
    if (actors_->state(k_player_actor, state) == hotload::Result::Ok) scale = scale * state.scale;
  }
  const Vec3 local = eye * scale;
  const Vec3 turned =
      rotate(quat_from_axis_angle(Vec3::unit_y(), drawn.yaw + rig_.facing_yaw), local);
  const WorldPos origin{
      drawn.position.x,
      drawn.position.y - static_cast<f64>(eye_height) + static_cast<f64>(rig_.origin_above_feet_m),
      drawn.position.z};
  FlyState at = drawn;
  at.position = origin + DVec3{turned};
  out = fly_view(at, fov_y, znear);
  last_.pitch = drawn.pitch;
  last_.shares = shares;
  last_.eye = Vec3{-local.x, local.y + rig_.origin_above_feet_m, local.z};
  last_.position = at.position;
  ++face_frames_;
  return true;
}

void PlayerBody::note_marker(u64 tick) {
  if (face_frames_ == 0 || markers_.size() >= 16) return;
  FaceEye marker = last_;
  marker.tick = tick;
  markers_.push_back(marker);
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
  // The eye's rule (P58): `face` or `walker`, and where its height came from.
  out.set("eye", rig_.eye_face ? "face" : "walker");
  out.set("eye_from", rig_.eye_from);
  // The eye less the head joint, in the body's frame: x its right, y up, z ahead — the rule in
  // effect's (under `face`, the eye at rest).
  if (rig_.head >= 0) {
    Vec3 from = rig_.eye_from_head;
    if (rig_.eye_face) {
      const Vec3 head = rig_.head_bind * rig_.scale;
      from = rig_.eye_point - Vec3{-head.x, head.y + rig_.origin_above_feet_m, head.z};
    }
    out.set("eye_from_head_m", vec(from));
  }
  if (rig_.eye_face) {
    // The eye at rest in the body's frame, metres from the soles; the face's vertices it came from.
    out.set("eye_point_m", vec(rig_.eye_point));
    out.set("eye_step_m", static_cast<f64>(rig_.eye_step_m));
    out.set("eye_line", static_cast<f64>(rig_.eye_line));
    out.set("head_vertices", static_cast<u64>(rig_.head_vertices));
    out.set("head_lo_m", vec(rig_.head_lo));
    out.set("head_hi_m", vec(rig_.head_hi));
    // The head-look layer: its pivots, the ranges (degrees), and the last face camera — the look's
    // shares, the eye in the body's frame and in the world — and each marker capture's.
    JsonValue look = JsonValue::object();
    look.set("on", look_);
    look.set("torso", rig_.torso_name);
    look.set("neck", rig_.neck_name);
    look.set("head", rig_.head_name);
    constexpr f64 k_to_deg = 57.29577951308232;
    look.set("flexion_deg", static_cast<f64>(rig_.flexion) * k_to_deg);
    look.set("extension_deg", static_cast<f64>(rig_.extension) * k_to_deg);
    look.set("rotation_deg", static_cast<f64>(rig_.rotation) * k_to_deg);
    look.set("head_flexion_share", static_cast<f64>(k_head_flexion_share));
    look.set("head_rotation_share", static_cast<f64>(k_head_rotation_share));
    // The pivots at rest in the body's frame (metres from the soles), for the arithmetic.
    look.set("torso_m", vec(rig_.torso_point));
    look.set("neck_m", vec(rig_.neck_point));
    look.set("head_m", vec(rig_.head_point));
    look.set("frames", face_frames_);
    const auto eye_json = [&](const FaceEye& e) {
      JsonValue o = JsonValue::object();
      if (e.tick != 0) o.set("tick", e.tick);  // a marker's; the last frame's has none
      o.set("pitch_deg", static_cast<f64>(e.pitch) * k_to_deg);
      JsonValue down = JsonValue::array();
      for (const f32 d : e.shares.down)
        down.push_back(static_cast<f64>(d) * k_to_deg);
      o.set("down_deg", down);  // torso, neck, head
      o.set("eye_m", vec(e.eye));
      JsonValue p = JsonValue::array();
      p.push_back(e.position.x);
      p.push_back(e.position.y);
      p.push_back(e.position.z);
      o.set("position", p);
      return o;
    };
    if (face_frames_ > 0) look.set("last", eye_json(last_));
    JsonValue markers = JsonValue::array();
    for (const FaceEye& e : markers_)
      markers.push_back(eye_json(e));
    look.set("markers", markers);
    out.set("look", look);
  }
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
