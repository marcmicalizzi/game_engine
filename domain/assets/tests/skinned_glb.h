#pragma once

// A two-bone skinned GLB written at test time, the way the engine-view tests write their cube:
// a binary in the tree is something no one can review, and the numbers here are the tests'
// expectations as much as their input. Kept as a header so more than one test file can build the
// same fixture and agree on what it contains.
//
// The asset, in one picture. A flat "bar" of two quads standing along +y, six vertices in three
// rows, skinned to a two-joint chain:
//
//     y = 2   v4 --- v5      weights (0, 1)      joint_tip   at (0, 1, 0) in joint_root's frame
//     y = 1   v2 --- v3      weights (.5, .5)      |
//     y = 0   v0 --- v1      weights (1, 0)      joint_root  at the origin
//
// The mesh node sits at x = +5 and the skin's joints do not, which is what makes the fixture a
// test of the bind-space rule: glTF places a skinned mesh through its joints alone, so the
// importer must leave the positions where the accessor put them and record the +5 on the skin.
// The node array is deliberately written so that the file's order is *not* parent-before-child
// (the mesh node comes before the deepest joint), which is what the importer's reordering has to
// fix.
//
// One animation, "wave", exercises all three glTF sampler modes and all three animated paths:
// LINEAR rotation on the tip, STEP translation on the root, CUBICSPLINE scale on the root.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace engine::assets::test_fixture {

// How the fixture writes the two skinning attributes, and the one way it can be written wrong.
struct SkinnedGlbOptions {
  bool joints_u16 = false;          // JOINTS_0 as UNSIGNED_SHORT rather than UNSIGNED_BYTE
  bool weights_u8 = false;          // WEIGHTS_0 as normalized UNSIGNED_BYTE rather than float
  bool joint_out_of_range = false;  // one vertex names joint 9 of a two-joint skin
  u32 extra_joints = 0;             // pad the skin out to more joints than a u8 index holds
};

inline void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

inline void put_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xffu));
  out.push_back(static_cast<u8>(v >> 8));
}

inline void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

inline void pad4(std::vector<u8>& out) {
  while (out.size() % 4 != 0)
    out.push_back(0);
}

inline std::string n(usize v) { return std::to_string(v); }

// The bind-space positions the importer must produce, in the file's vertex order.
inline const Vec3* bar_positions() noexcept {
  static const Vec3 k_positions[6] = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f},  {-0.5f, 1.0f, 0.0f},
                                      {0.5f, 1.0f, 0.0f},  {-0.5f, 2.0f, 0.0f}, {0.5f, 2.0f, 0.0f}};
  return k_positions;
}

// The two influences per vertex, before quantization: row 0 is all root, row 1 is half each, row
// 2 is all tip.
inline const f32* bar_weights() noexcept {
  static const f32 k_weights[6][2] = {{1.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 0.5f},
                                      {0.5f, 0.5f}, {0.0f, 1.0f}, {0.0f, 1.0f}};
  return &k_weights[0][0];
}

// Writes the fixture to `path`. Returns false only when the file cannot be opened.
inline bool write_skinned_glb(const std::string& path, const SkinnedGlbOptions& options = {}) {
  const Vec3* positions = bar_positions();
  const f32* weights = bar_weights();

  std::vector<u8> bin;
  const u32 position_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    put_f32(bin, positions[v].x);
    put_f32(bin, positions[v].y);
    put_f32(bin, positions[v].z);
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    put_f32(bin, 0.0f);
    put_f32(bin, 0.0f);
    put_f32(bin, 1.0f);
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    put_f32(bin, static_cast<f32>(v & 1u));
    put_f32(bin, static_cast<f32>(v / 2) * 0.5f);
  }
  const u32 joint_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    const u32 second = (options.joint_out_of_range && v == 3) ? 9u : 1u;
    const u32 quad[4] = {0u, second, 0u, 0u};
    for (const u32 j : quad) {
      if (options.joints_u16) {
        put_u16(bin, static_cast<u16>(j));
      } else {
        bin.push_back(static_cast<u8>(j));
      }
    }
  }
  pad4(bin);
  const u32 weight_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    const f32 quad[4] = {weights[v * 2], weights[v * 2 + 1], 0.0f, 0.0f};
    for (const f32 w : quad) {
      if (options.weights_u8) {
        bin.push_back(static_cast<u8>(w * 255.0f + 0.5f));
      } else {
        put_f32(bin, w);
      }
    }
  }
  pad4(bin);
  const u32 index_offset = static_cast<u32>(bin.size());
  const u16 tris[12] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
  for (const u16 i : tris)
    put_u16(bin, i);
  pad4(bin);
  // Inverse bind matrices, column-major: the root's is the identity, the tip's undoes its own
  // (0, 1, 0), which is what takes a point from model space into the joint's space at bind.
  const u32 inverse_bind_offset = static_cast<u32>(bin.size());
  const f32 identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const f32 tip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
  for (const f32 v : identity)
    put_f32(bin, v);
  for (const f32 v : tip)
    put_f32(bin, v);

  // LINEAR rotation on the tip: identity, then a quarter turn about +z.
  const u32 rotation_time_offset = static_cast<u32>(bin.size());
  put_f32(bin, 0.0f);
  put_f32(bin, 1.0f);
  const u32 rotation_value_offset = static_cast<u32>(bin.size());
  const f32 quarter = 0.70710678f;
  const f32 rotations[8] = {0, 0, 0, 1, 0, 0, quarter, quarter};
  for (const f32 v : rotations)
    put_f32(bin, v);
  // STEP translation on the root: the origin until 0.5, then two units up.
  const u32 translation_time_offset = static_cast<u32>(bin.size());
  put_f32(bin, 0.0f);
  put_f32(bin, 0.5f);
  const u32 translation_value_offset = static_cast<u32>(bin.size());
  const f32 translations[6] = {0, 0, 0, 0, 2, 0};
  for (const f32 v : translations)
    put_f32(bin, v);
  // CUBICSPLINE scale on the root: in-tangent, value, out-tangent per key.
  const u32 scale_time_offset = static_cast<u32>(bin.size());
  put_f32(bin, 0.0f);
  put_f32(bin, 1.0f);
  const u32 scale_value_offset = static_cast<u32>(bin.size());
  const f32 scales[18] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0};
  for (const f32 v : scales)
    put_f32(bin, v);
  const u32 bin_bytes = static_cast<u32>(bin.size());

  const std::string joint_component = options.joints_u16 ? "5123" : "5121";
  const std::string weight_component = options.weights_u8 ? "5121" : "5126";
  const std::string weight_normalized = options.weights_u8 ? ",\"normalized\":true" : "";
  const u32 joint_stride = options.joints_u16 ? 8u : 4u;
  const u32 weight_stride = options.weights_u8 ? 4u : 16u;

  // Extra joints, when asked for: plain nodes hanging off the rig, enough to push the skin past
  // what a u8 per-vertex index can name. The skin then has no inverse bind matrices, which glTF
  // allows and which keeps the accessor count honest.
  std::string extra_nodes;
  std::string extra_joint_list;
  std::string extra_children;
  for (u32 i = 0; i < options.extra_joints; ++i) {
    const u32 index = 4 + i;
    extra_nodes += ",{\"name\":\"filler" + n(i) + "\",\"translation\":[0," + n(i) + ",0]}";
    extra_joint_list += "," + n(index);
    extra_children += "," + n(index);
  }
  const std::string inverse_bind =
      options.extra_joints != 0 ? std::string() : std::string(",\"inverseBindMatrices\":6");

  std::string json;
  json += "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
  json += "\"nodes\":[";
  json += "{\"name\":\"rig\",\"children\":[1,3" + extra_children + "]},";
  json += "{\"name\":\"joint_root\",\"children\":[2],\"translation\":[0,0,0]},";
  json += "{\"name\":\"joint_tip\",\"translation\":[0,1,0]},";
  json += "{\"name\":\"bar\",\"mesh\":0,\"skin\":0,\"translation\":[5,0,0]}";
  json += extra_nodes;
  json += "],";
  json += "\"skins\":[{\"name\":\"bar_skin\",\"joints\":[1,2" + extra_joint_list +
          "],\"skeleton\":1" + inverse_bind + "}],";
  json +=
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":5,\"material\":0}]}],";
  json +=
      "\"materials\":[{\"name\":\"bar_material\",\"pbrMetallicRoughness\":{"
      "\"baseColorFactor\":[0.8,0.8,0.8,1],\"metallicFactor\":0,\"roughnessFactor\":0.6}}],";
  json +=
      "\"animations\":[{\"name\":\"wave\",\"samplers\":["
      "{\"input\":7,\"output\":8,\"interpolation\":\"LINEAR\"},"
      "{\"input\":9,\"output\":10,\"interpolation\":\"STEP\"},"
      "{\"input\":11,\"output\":12,\"interpolation\":\"CUBICSPLINE\"}],"
      "\"channels\":["
      "{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}},"
      "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"translation\"}},"
      "{\"sampler\":2,\"target\":{\"node\":1,\"path\":\"scale\"}}]}],";
  json += "\"accessors\":[";
  json +=
      "{\"bufferView\":0,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\","
      "\"min\":[-0.5,0,0],\"max\":[0.5,2,0]},";
  json += "{\"bufferView\":1,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"},";
  json += "{\"bufferView\":2,\"componentType\":5126,\"count\":6,\"type\":\"VEC2\"},";
  json +=
      "{\"bufferView\":3,\"componentType\":" + joint_component + ",\"count\":6,\"type\":\"VEC4\"},";
  json += "{\"bufferView\":4,\"componentType\":" + weight_component +
          ",\"count\":6,\"type\":" + "\"VEC4\"" + weight_normalized + "},";
  json += "{\"bufferView\":5,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"},";
  json += "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},";
  json +=
      "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
      "\"min\":[0],\"max\":[1]},";
  json += "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},";
  json +=
      "{\"bufferView\":9,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
      "\"min\":[0],\"max\":[0.5]},";
  json += "{\"bufferView\":10,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},";
  json +=
      "{\"bufferView\":11,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
      "\"min\":[0],\"max\":[1]},";
  json += "{\"bufferView\":12,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"}],";

  auto view = [](u32 offset, u32 bytes) {
    return "{\"buffer\":0,\"byteOffset\":" + n(offset) + ",\"byteLength\":" + n(bytes) + "}";
  };
  json += "\"bufferViews\":[";
  json += view(position_offset, 6 * 12) + ",";
  json += view(normal_offset, 6 * 12) + ",";
  json += view(uv_offset, 6 * 8) + ",";
  json += view(joint_offset, 6 * joint_stride) + ",";
  json += view(weight_offset, 6 * weight_stride) + ",";
  json += view(index_offset, 12 * 2) + ",";
  json += view(inverse_bind_offset, 2 * 64) + ",";
  json += view(rotation_time_offset, 2 * 4) + ",";
  json += view(rotation_value_offset, 2 * 16) + ",";
  json += view(translation_time_offset, 2 * 4) + ",";
  json += view(translation_value_offset, 2 * 12) + ",";
  json += view(scale_time_offset, 2 * 4) + ",";
  json += view(scale_value_offset, 6 * 12);
  json += "],\"buffers\":[{\"byteLength\":" + n(bin_bytes) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';

  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());

  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return file.good();
}

}  // namespace engine::assets::test_fixture
