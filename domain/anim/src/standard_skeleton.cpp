#include <core/log/log.h>
#include <domain/anim/standard_skeleton.h>

#include <cmath>

ENGINE_LOG_CATEGORY_DEFINE(log_anim, "anim");

namespace engine::anim {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// The canonical spelling of each role, in enum order.
const char* const k_names[k_standard_joint_count] = {
    "Root",          "Hips",          "Spine",         "Chest",         "UpperChest",
    "Neck",          "Head",          "LeftShoulder",  "LeftUpperArm",  "LeftLowerArm",
    "LeftHand",      "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",
    "LeftUpperLeg",  "LeftLowerLeg",  "LeftFoot",      "LeftToes",      "RightUpperLeg",
    "RightLowerLeg", "RightFoot",     "RightToes"};

// The normalized spellings each role answers to, NUL-terminated per role by an empty string.
// These are the names the rigs that actually arrive use: Mixamo, the Khronos samples, Blender's
// Rigify, and the Bip01 lineage. Normalization drops case and everything that is not a letter or
// a digit, so `mixamorig:LeftForeArm`, `Left_Fore_Arm`, and `leftforearm` are one name.
const char* const* role_aliases(u32 role) noexcept {
  static const char* const k_root[] = {"root", "reference", "armature", "rootnode", nullptr};
  static const char* const k_hips[] = {"hips", "hip", "pelvis", "bip01pelvis", nullptr};
  static const char* const k_spine[] = {"spine", "spine1", "abdomen", "bip01spine", nullptr};
  static const char* const k_chest[] = {"chest", "spine2", "spine02", "bip01spine1", nullptr};
  static const char* const k_upper_chest[] = {"upperchest", "spine3", "spine03", nullptr};
  static const char* const k_neck[] = {"neck", "neck1", nullptr};
  static const char* const k_head[] = {"head", nullptr};
  static const char* const k_l_shoulder[] = {
      "leftshoulder", "lshoulder", "shoulderl", "leftclavicle", "lclavicle", "claviclel", nullptr};
  static const char* const k_l_upper_arm[] = {"leftupperarm", "leftarm",   "lupperarm",
                                              "larm",         "upperarml", nullptr};
  static const char* const k_l_lower_arm[] = {
      "leftlowerarm", "leftforearm", "lforearm", "llowerarm", "lowerarml", "forearml", nullptr};
  static const char* const k_l_hand[] = {"lefthand", "lhand", "handl", "leftwrist", nullptr};
  static const char* const k_r_shoulder[] = {"rightshoulder", "rshoulder", "shoulderr",
                                             "rightclavicle", "rclavicle", "clavicler",
                                             nullptr};
  static const char* const k_r_upper_arm[] = {"rightupperarm", "rightarm",  "rupperarm",
                                              "rarm",          "upperarmr", nullptr};
  static const char* const k_r_lower_arm[] = {
      "rightlowerarm", "rightforearm", "rforearm", "rlowerarm", "lowerarmr", "forearmr", nullptr};
  static const char* const k_r_hand[] = {"righthand", "rhand", "handr", "rightwrist", nullptr};
  static const char* const k_l_upper_leg[] = {"leftupleg", "leftupperleg", "leftthigh", "lthigh",
                                              "thighl",    "upperlegl",    nullptr};
  static const char* const k_l_lower_leg[] = {"leftleg", "leftlowerleg", "leftcalf",
                                              "lcalf",   "calfl",        "leftshin",
                                              "shinl",   "lowerlegl",    nullptr};
  static const char* const k_l_foot[] = {"leftfoot",  "lfoot",  "footl",
                                         "leftankle", "anklel", nullptr};
  static const char* const k_l_toes[] = {"lefttoebase", "lefttoes", "lefttoe", "ltoe",
                                         "toel",        "balll",    nullptr};
  static const char* const k_r_upper_leg[] = {"rightupleg", "rightupperleg", "rightthigh", "rthigh",
                                              "thighr",     "upperlegr",     nullptr};
  static const char* const k_r_lower_leg[] = {"rightleg", "rightlowerleg", "rightcalf",
                                              "rcalf",    "calfr",         "rightshin",
                                              "shinr",    "lowerlegr",     nullptr};
  static const char* const k_r_foot[] = {"rightfoot",  "rfoot",  "footr",
                                         "rightankle", "ankler", nullptr};
  static const char* const k_r_toes[] = {"righttoebase", "righttoes", "righttoe",
                                         "rtoe",         "toer",      nullptr};
  static const char* const* const k_table[k_standard_joint_count] = {
      k_root,        k_hips,        k_spine,       k_chest,       k_upper_chest, k_neck,
      k_head,        k_l_shoulder,  k_l_upper_arm, k_l_lower_arm, k_l_hand,      k_r_shoulder,
      k_r_upper_arm, k_r_lower_arm, k_r_hand,      k_l_upper_leg, k_l_lower_leg, k_l_foot,
      k_l_toes,      k_r_upper_leg, k_r_lower_leg, k_r_foot,      k_r_toes};
  return k_table[role];
}

std::string normalized(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out.push_back(c);
    }
  }
  return out;
}

bool ends_with(const std::string& text, std::string_view suffix) noexcept {
  return text.size() >= suffix.size() &&
         std::string_view(text).substr(text.size() - suffix.size()) == suffix;
}

// The model-space height of the hips at bind: the yardstick a root translation is scaled by.
// The hips' own origin is carried up through each parent's transform, so a rig whose chain is
// rotated gives the right number rather than a sum of local offsets.
f32 hip_height(const Skeleton& skeleton, const JointMapping& mapping) {
  const i32 hips = mapping[StandardJoint::Hips];
  if (hips == k_no_joint) return 0.0f;
  Vec3 point{};
  for (i32 joint = hips; joint != k_no_joint; joint = skeleton.parents[static_cast<u32>(joint)])
    point = ::engine::transform_point(skeleton.local_bind[static_cast<u32>(joint)], point);
  return std::fabs(point.y);
}

}  // namespace

const char* standard_joint_name(StandardJoint joint) noexcept {
  const u32 index = static_cast<u32>(joint);
  return index < k_standard_joint_count ? k_names[index] : "";
}

StandardJoint standard_joint_from_name(std::string_view name) noexcept {
  for (u32 i = 0; i < k_standard_joint_count; ++i) {
    if (name == k_names[i]) return static_cast<StandardJoint>(i);
  }
  return StandardJoint::Count;
}

JointMapping map_joints(const Skeleton& skeleton) {
  JointMapping out;
  out.to_skeleton.assign(k_standard_joint_count, k_no_joint);
  const u32 joints = skeleton.joint_count();
  Vector<std::string> flattened;
  flattened.reserve(joints);
  for (u32 j = 0; j < joints; ++j)
    flattened.push_back(normalized(skeleton.names[j]));
  Vector<u8> claimed(joints, u8{0});

  // Three passes, widening as they go, so that an exact name always beats a substring: with one
  // pass, `LeftHandIndex1` could take `LeftHand`'s role simply by coming first in the array.
  for (u32 pass = 0; pass < 3; ++pass) {
    for (u32 role = 0; role < k_standard_joint_count; ++role) {
      if (out.to_skeleton[role] != k_no_joint) continue;
      for (const char* const* alias = role_aliases(role); *alias != nullptr; ++alias) {
        const std::string_view want(*alias);
        for (u32 j = 0; j < joints && out.to_skeleton[role] == k_no_joint; ++j) {
          if (claimed[j] != 0) continue;
          const std::string& have = flattened[j];
          const bool hit = pass == 0   ? have == want
                           : pass == 1 ? ends_with(have, want)
                                       : have.find(want) != std::string::npos;
          if (!hit) continue;
          out.to_skeleton[role] = static_cast<i32>(j);
          claimed[j] = 1;
          ++out.mapped;
        }
        if (out.to_skeleton[role] != k_no_joint) break;
      }
    }
  }
  return out;
}

bool build_retarget(const Skeleton& source, const Skeleton& target, Retarget& out,
                    std::string* error) {
  out = Retarget{};
  std::string why;
  if (!source.validate(&why)) return fail(error, "retarget source " + why);
  if (!target.validate(&why)) return fail(error, "retarget target " + why);

  const JointMapping source_map = map_joints(source);
  const JointMapping target_map = map_joints(target);
  const u32 target_joints = target.joint_count();
  out.joints.resize(target_joints, RetargetJoint{});
  for (u32 j = 0; j < target_joints; ++j)
    out.joints[j].bind = target.local_bind[j];

  for (u32 role = 0; role < k_standard_joint_count; ++role) {
    const i32 from = source_map.to_skeleton[role];
    const i32 to = target_map.to_skeleton[role];
    if (from == k_no_joint || to == k_no_joint) continue;
    RetargetJoint& entry = out.joints[static_cast<u32>(to)];
    entry.source = from;
    const Transform3& source_bind = source.local_bind[static_cast<u32>(from)];
    entry.source_bind_inverse = conjugate(normalize(source_bind.rotation));
    entry.source_bind_translation = source_bind.position;
    ++out.mapped;
  }
  if (out.mapped == 0) {
    return fail(error,
                "retarget: no canonical joint is named by both skeletons, so every target joint "
                "would hold its bind pose");
  }

  const f32 source_height = hip_height(source, source_map);
  const f32 target_height = hip_height(target, target_map);
  out.translation_scale =
      source_height > 1.0e-6f && target_height > 1.0e-6f ? target_height / source_height : 1.0f;
  ENGINE_LOG_INFO(log_anim, "retarget built", log::field("roles", out.mapped),
                  log::field("source_joints", source.joint_count()),
                  log::field("target_joints", target_joints),
                  log::field("translation_scale", out.translation_scale));
  return true;
}

void retarget_pose(const Retarget& retarget, const Pose& source_pose, Pose& out) {
  const u32 joints = retarget.joints.size();
  if (out.joint_count() != joints) out.resize(joints);
  const u32 source_joints = source_pose.joint_count();
  for (u32 j = 0; j < joints; ++j) {
    const RetargetJoint& entry = retarget.joints[j];
    if (entry.source == k_no_joint || static_cast<u32>(entry.source) >= source_joints) {
      // Nothing drives this joint: its bind pose, so the result is always a complete pose.
      out.translation[j] = entry.bind.position;
      out.rotation[j] = entry.bind.rotation;
      out.scale[j] = entry.bind.scale;
      continue;
    }
    const u32 from = static_cast<u32>(entry.source);
    // What transfers is how far the joint has turned from *its own* rest, not where it points in
    // its parent's frame: the two rests differ by exactly the modelling decisions a retarget is
    // there to absorb.
    const Quat deviation = entry.source_bind_inverse * source_pose.rotation[from];
    out.rotation[j] = normalize(entry.bind.rotation * deviation);
    out.translation[j] =
        entry.bind.position + (source_pose.translation[from] - entry.source_bind_translation) *
                                  retarget.translation_scale;
    out.scale[j] = source_pose.scale[from];
  }
}

}  // namespace engine::anim
