#pragma once

// A two-bone skinned GLB written at test time, with two clips on it.
//
// Written rather than committed, for the same reason `domain/assets/tests/skinned_glb.h` is: a
// binary in the tree is something nobody can review, and the numbers here are the tests'
// expectations as much as their input. It is a smaller cousin of that fixture — this capability
// needs a skeleton and clips, not a test of the importer's attribute handling — and it is its own
// file rather than an include of assets' because a module does not reach into another module's
// tests directory.
//
// The asset, in one picture. A flat bar of two quads standing along +y, six vertices in three
// rows, skinned to a two-joint chain:
//
//     y = 2   v4 --- v5      weights (0, 1)      joint_tip   at (0, 1, 0) in joint_root's frame
//     y = 1   v2 --- v3      weights (.5, .5)      |
//     y = 0   v0 --- v1      weights (1, 0)      joint_root  at the origin
//
// Two clips, both LINEAR over exactly one second, chosen so that a blend between them is
// hand-checkable and so that neither is a special case of the other:
//
//   "walk"   rotation on joint_tip:  identity -> a quarter turn about +z
//   "idle"   translation on joint_root: the origin -> (0, 4, 0)
//
// One second and these keys are all exact binary fractions, which is what lets the looping test
// assert an exact wrap instead of an approximate one.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace engine::animation::test_fixture {

inline constexpr f32 k_clip_seconds = 1.0f;
inline constexpr const char* k_walk = "walk";
inline constexpr const char* k_idle = "idle";
inline constexpr const char* k_skin = "bar_skin";

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

// Writes the fixture to `path`. Returns false only when the file cannot be opened.
inline bool write_animated_glb(const std::string& path) {
  const Vec3 positions[6] = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f},  {-0.5f, 1.0f, 0.0f},
                             {0.5f, 1.0f, 0.0f},  {-0.5f, 2.0f, 0.0f}, {0.5f, 2.0f, 0.0f}};
  const f32 weights[6][2] = {{1.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 0.5f},
                             {0.5f, 0.5f}, {0.0f, 1.0f}, {0.0f, 1.0f}};

  std::vector<u8> bin;
  const u32 position_offset = static_cast<u32>(bin.size());
  for (const Vec3& p : positions) {
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 joint_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    const u8 quad[4] = {0u, 1u, 0u, 0u};
    for (const u8 j : quad)
      bin.push_back(j);
  }
  pad4(bin);
  const u32 weight_offset = static_cast<u32>(bin.size());
  for (u32 v = 0; v < 6; ++v) {
    put_f32(bin, weights[v][0]);
    put_f32(bin, weights[v][1]);
    put_f32(bin, 0.0f);
    put_f32(bin, 0.0f);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  const u16 tris[12] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
  for (const u16 i : tris)
    put_u16(bin, i);
  pad4(bin);
  // Column-major inverse binds: the root's is the identity, the tip's undoes its own (0, 1, 0).
  const u32 inverse_bind_offset = static_cast<u32>(bin.size());
  const f32 identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const f32 tip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
  for (const f32 v : identity)
    put_f32(bin, v);
  for (const f32 v : tip)
    put_f32(bin, v);

  const u32 walk_time_offset = static_cast<u32>(bin.size());
  put_f32(bin, 0.0f);
  put_f32(bin, k_clip_seconds);
  const u32 walk_value_offset = static_cast<u32>(bin.size());
  const f32 quarter = 0.70710678f;
  const f32 rotations[8] = {0, 0, 0, 1, 0, 0, quarter, quarter};
  for (const f32 v : rotations)
    put_f32(bin, v);

  const u32 idle_time_offset = static_cast<u32>(bin.size());
  put_f32(bin, 0.0f);
  put_f32(bin, k_clip_seconds);
  const u32 idle_value_offset = static_cast<u32>(bin.size());
  const f32 translations[6] = {0, 0, 0, 0, 4, 0};
  for (const f32 v : translations)
    put_f32(bin, v);
  const u32 bin_bytes = static_cast<u32>(bin.size());

  std::string json;
  json += "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
  json += "\"nodes\":[";
  json += "{\"name\":\"rig\",\"children\":[1,3]},";
  json += "{\"name\":\"joint_root\",\"children\":[2],\"translation\":[0,0,0]},";
  json += "{\"name\":\"joint_tip\",\"translation\":[0,1,0]},";
  json += "{\"name\":\"bar\",\"mesh\":0,\"skin\":0,\"translation\":[5,0,0]}";
  json += "],";
  json += std::string("\"skins\":[{\"name\":\"") + k_skin +
          "\",\"joints\":[1,2],\"skeleton\":1,\"inverseBindMatrices\":4}],";
  json +=
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
      "\"WEIGHTS_0\":2},\"indices\":3}]}],";
  json += std::string("\"animations\":[{\"name\":\"") + k_walk +
          "\",\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"}],"
          "\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}]},";
  json += std::string("{\"name\":\"") + k_idle +
          "\",\"samplers\":[{\"input\":7,\"output\":8,\"interpolation\":\"LINEAR\"}],"
          "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}],";
  json += "\"accessors\":[";
  json +=
      "{\"bufferView\":0,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\","
      "\"min\":[-0.5,0,0],\"max\":[0.5,2,0]},";
  json += "{\"bufferView\":1,\"componentType\":5121,\"count\":6,\"type\":\"VEC4\"},";
  json += "{\"bufferView\":2,\"componentType\":5126,\"count\":6,\"type\":\"VEC4\"},";
  json += "{\"bufferView\":3,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"},";
  json += "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},";
  json +=
      "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
      "\"min\":[0],\"max\":[1]},";
  json += "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},";
  json +=
      "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
      "\"min\":[0],\"max\":[1]},";
  json += "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}],";

  auto view = [](u32 offset, u32 bytes) {
    return "{\"buffer\":0,\"byteOffset\":" + n(offset) + ",\"byteLength\":" + n(bytes) + "}";
  };
  json += "\"bufferViews\":[";
  json += view(position_offset, 6 * 12) + ",";
  json += view(joint_offset, 6 * 4) + ",";
  json += view(weight_offset, 6 * 16) + ",";
  json += view(index_offset, 12 * 2) + ",";
  json += view(inverse_bind_offset, 2 * 64) + ",";
  json += view(walk_time_offset, 2 * 4) + ",";
  json += view(walk_value_offset, 2 * 16) + ",";
  json += view(idle_time_offset, 2 * 4) + ",";
  json += view(idle_value_offset, 2 * 12);
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

}  // namespace engine::animation::test_fixture
