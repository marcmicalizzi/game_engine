#pragma once

// The two-bone bar of `animation_glb.h` under a **transformed non-joint ancestor**, written at
// test time in two forms that must pose identically (S92).
//
// What Blender writes for a rigged character: the skin's joints sit under an `Armature` node that
// is not itself a joint, and that node carries a transform. The game's cat had its Armature
// translated +0.375 m on y, and the library used to parent a root joint to nothing and forget the
// Armature, so the character stood buried by exactly that much. The **normalised** form is what
// the game rewrote its files into to work around it: an identity ancestor, with the ancestor's
// translation moved into the root joint's own translation and its keyframes.
//
//     original                                  normalised
//     Armature  t = (0, 0.375, 0)   [track]     Armature  identity
//       joint_root  t = 0           [track]       joint_root  t = (0, 0.375, 0)   [track']
//         joint_tip  t = (0, 1, 0)  [walk]          joint_tip  t = (0, 1, 0)      [walk]
//       bar (mesh, skin)                          bar (mesh, skin)
//
// The inverse binds are the joints' own frames without the ancestor (identity for the root, the
// tip's height undone for the tip), so the bar's bind-space vertices run from y = 0 to y = 2 and
// the rest pose has to lift them by the ancestor's 0.375: the mesh's lowest vertex stands at
// +0.375 m, which is the number the tests read.
//
// Three clips, all LINEAR over one second with keys that are exact binary fractions, so that the
// original and the normalised forms can be compared to the bit where the fold is exact:
//
//   "rise"    the ancestor's translation 0.375 -> 0.875 on y, and the tip's quarter turn about +z.
//             Normalised: the same keys on the root joint.
//   "slide"   the ancestor still, the root joint's translation 0 -> 0.5 on y (the cat's walk: a
//             root-translation track relative to a static Armature). Normalised: 0.375 -> 0.875.
//   "both"    the ancestor rises as in "rise" and the root slides 0 -> 0.25 on x, two animated
//             inputs to one channel, which the library resamples. Normalised: the sum of the two.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace engine::animation::test_fixture {

inline constexpr f32 k_ancestor_height = 0.375f;
inline constexpr const char* k_rise = "rise";
inline constexpr const char* k_slide = "slide";
inline constexpr const char* k_both = "both";

namespace ancestor_detail {

inline void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
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

}  // namespace ancestor_detail

// The bar's bind-space vertices, which the tests skin to find where the mesh stands.
inline constexpr Vec3 k_bar_positions[6] = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f},
                                            {-0.5f, 1.0f, 0.0f}, {0.5f, 1.0f, 0.0f},
                                            {-0.5f, 2.0f, 0.0f}, {0.5f, 2.0f, 0.0f}};
inline constexpr f32 k_bar_weights[6][2] = {{1.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 0.5f},
                                            {0.5f, 0.5f}, {0.0f, 1.0f}, {0.0f, 1.0f}};

// Writes the fixture to `path`, original or normalised. False only when the file cannot be opened.
inline bool write_ancestor_glb(const std::string& path, bool normalised) {
  using namespace ancestor_detail;
  std::vector<u8> bin;
  struct View {
    u32 offset;
    u32 bytes;
  };
  std::vector<View> views;
  auto begin_view = [&]() { return static_cast<u32>(bin.size()); };
  auto end_view = [&](u32 offset) {
    pad4(bin);
    views.push_back(View{offset, static_cast<u32>(bin.size()) - offset});
  };

  u32 at = begin_view();  // 0: positions
  for (const Vec3& p : k_bar_positions) {
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  end_view(at);
  at = begin_view();  // 1: joints
  for (u32 v = 0; v < 6; ++v) {
    for (const u8 j : {u8{0}, u8{1}, u8{0}, u8{0}})
      bin.push_back(j);
  }
  end_view(at);
  at = begin_view();  // 2: weights
  for (u32 v = 0; v < 6; ++v) {
    put_f32(bin, k_bar_weights[v][0]);
    put_f32(bin, k_bar_weights[v][1]);
    put_f32(bin, 0.0f);
    put_f32(bin, 0.0f);
  }
  end_view(at);
  at = begin_view();  // 3: indices
  for (const u16 i : {u16{0}, u16{1}, u16{3}, u16{0}, u16{3}, u16{2}, u16{2}, u16{3}, u16{5},
                      u16{2}, u16{5}, u16{4}}) {
    bin.push_back(static_cast<u8>(i & 0xffu));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  end_view(at);
  at = begin_view();  // 4: inverse binds, column-major
  for (const f32 v : {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 1.0f})
    put_f32(bin, v);
  for (const f32 v : {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                      -1.0f, 0.0f, 1.0f})
    put_f32(bin, v);
  end_view(at);
  at = begin_view();  // 5: the key times, shared by every sampler
  put_f32(bin, 0.0f);
  put_f32(bin, 1.0f);
  end_view(at);
  auto vec3_view = [&](Vec3 a, Vec3 b) {
    const u32 offset = begin_view();
    for (const Vec3 v : {a, b}) {
      put_f32(bin, v.x);
      put_f32(bin, v.y);
      put_f32(bin, v.z);
    }
    end_view(offset);
    return static_cast<u32>(views.size() - 1);
  };
  const f32 h = k_ancestor_height;
  const u32 rise = vec3_view(Vec3{0.0f, h, 0.0f}, Vec3{0.0f, 0.875f, 0.0f});  // 6
  const u32 slide = normalised ? vec3_view(Vec3{0.0f, h, 0.0f}, Vec3{0.0f, 0.875f, 0.0f})
                               : vec3_view(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.5f, 0.0f});
  const u32 both_root = normalised ? vec3_view(Vec3{0.0f, h, 0.0f}, Vec3{0.25f, 0.875f, 0.0f})
                                   : vec3_view(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.25f, 0.0f, 0.0f});
  at = begin_view();
  const f32 quarter = 0.70710678f;
  for (const f32 v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, quarter, quarter})
    put_f32(bin, v);
  end_view(at);
  const u32 turn = static_cast<u32>(views.size() - 1);
  const u32 bin_bytes = static_cast<u32>(bin.size());

  // Accessors: 0..4 the mesh and the skin, 5 the times, then one per value view.
  std::string json;
  json += "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[";
  json += normalised ? "{\"name\":\"Armature\",\"children\":[1,3]},"
                     : "{\"name\":\"Armature\",\"children\":[1,3],\"translation\":[0,0.375,0]},";
  json += normalised ? "{\"name\":\"joint_root\",\"children\":[2],\"translation\":[0,0.375,0]},"
                     : "{\"name\":\"joint_root\",\"children\":[2]},";
  json += "{\"name\":\"joint_tip\",\"translation\":[0,1,0]},";
  json += "{\"name\":\"bar\",\"mesh\":0,\"skin\":0}],";
  json += "\"skins\":[{\"name\":\"bar_skin\",\"joints\":[1,2],\"inverseBindMatrices\":4}],";
  json +=
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
      "\"WEIGHTS_0\":2},\"indices\":3}]}],";
  auto sampler = [&](u32 view) {
    return "{\"input\":5,\"output\":" + n(view) + ",\"interpolation\":\"LINEAR\"}";
  };
  auto channel = [&](u32 s, u32 node, const char* p) {
    return "{\"sampler\":" + n(s) + ",\"target\":{\"node\":" + n(node) + ",\"path\":\"" + p +
           "\"}}";
  };
  // Where the ancestor's track lands: on the Armature in the original, on the root joint once the
  // file is normalised.
  const u32 lifted = normalised ? 1u : 0u;
  json += "\"animations\":[";
  json += std::string("{\"name\":\"") + k_rise + "\",\"samplers\":[" + sampler(rise) + "," +
          sampler(turn) + "],\"channels\":[" + channel(0, lifted, "translation") + "," +
          channel(1, 2, "rotation") + "]},";
  json += std::string("{\"name\":\"") + k_slide + "\",\"samplers\":[" + sampler(slide) +
          "],\"channels\":[" + channel(0, 1, "translation") + "]},";
  json += std::string("{\"name\":\"") + k_both + "\",\"samplers\":[" + sampler(both_root) +
          (normalised ? "" : "," + sampler(rise)) + "],\"channels\":[" +
          channel(0, 1, "translation") + (normalised ? "" : "," + channel(1, 0, "translation")) +
          "]}],";
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
      "\"min\":[0],\"max\":[1]}";
  for (u32 v = 6; v < views.size(); ++v) {
    json += ",{\"bufferView\":" + n(v) + ",\"componentType\":5126,\"count\":2,\"type\":\"" +
            (v == turn ? "VEC4" : "VEC3") + "\"}";
  }
  json += "],\"bufferViews\":[";
  for (u32 v = 0; v < views.size(); ++v) {
    if (v != 0) json += ",";
    json += "{\"buffer\":0,\"byteOffset\":" + n(views[v].offset) +
            ",\"byteLength\":" + n(views[v].bytes) + "}";
  }
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
