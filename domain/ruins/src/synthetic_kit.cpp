#include "glb.h"

#include <core/json/json.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::ruins {

namespace {

struct Box {
  Vec3 lo;
  Vec3 hi;
};

void put_u32(Vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_f32(Vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

std::string num(f32 v) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.9g", static_cast<f64>(v));
  return text;
}

}  // namespace

Vector<u8> mesh_glb(const Vector<Vec3>& positions, const Vector<Vec3>& normals,
                    const Vector<u32>& indices, const Vec3& colour, const char* generator) {
  Vector<u8> bin;
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (const Vec3& p : positions) {
    lo = min(lo, p);
    hi = max(hi, p);
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 vertices = positions.size();
  const u32 normal_offset = bin.size();
  for (const Vec3& n : normals) {
    put_f32(bin, n.x);
    put_f32(bin, n.y);
    put_f32(bin, n.z);
  }
  const u32 index_offset = bin.size();
  for (const u32 i : indices)
    put_u32(bin, i);
  const u32 index_bytes = bin.size() - index_offset;
  auto n = [](u32 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\",\"generator\":\"" + std::string(generator) +
      "\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
      "\"indices\":2,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
      num(colour.x) + "," + num(colour.y) + "," + num(colour.z) +
      ",1],\"metallicFactor\":0,\"roughnessFactor\":0.9}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      n(vertices) + ",\"type\":\"VEC3\",\"min\":[" + num(lo.x) + "," + num(lo.y) + "," + num(lo.z) +
      "],\"max\":[" + num(hi.x) + "," + num(hi.y) + "," + num(hi.z) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      n(vertices) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5125,\"count\":" +
      n(indices.size()) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(index_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(bin.size()) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  Vector<u8> glb;
  glb.reserve(static_cast<u32>(28 + json.size() + bin.size()));
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  for (const char c : json)
    glb.push_back(static_cast<u8>(c));
  put_u32(glb, bin.size());
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  for (const u8 byte : bin)
    glb.push_back(byte);
  return glb;
}

bool write_kit_files(const std::string& dir, const Vector<std::string>& names,
                     const Vector<Vector<u8>>& glbs, std::string* error) {
  if (io::make_directories(dir) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot create " + dir;
    return false;
  }
  for (u32 i = 0; i < names.size(); ++i) {
    const std::string path = io::join_path(dir, names[i]);
    const Vector<u8>& bytes = glbs[i];
    const io::Status status = io::write_file(
        path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (status != io::Status::Ok) {
      if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
      return false;
    }
  }
  return true;
}

namespace {

// Axis-aligned boxes as one GLB: 24 vertices a box with their face normals, 12 triangles wound
// counter-clockwise seen from outside, and one metallic-roughness material.
Vector<u8> boxes_glb(const Vector<Box>& boxes, const Vec3& colour) {
  constexpr Vec3 k_normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                 Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  constexpr Vec3 k_tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                                  Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<u32> indices;
  for (const Box& box : boxes) {
    const Vec3 centre = (box.lo + box.hi) * 0.5f;
    const Vec3 size = box.hi - box.lo;
    for (u32 f = 0; f < 6; ++f) {
      const Vec3 n = k_normals[f];
      const Vec3 t = k_tangents[f];
      const Vec3 b = cross(n, t);
      const Vec3 corners[4] = {n * 0.5f - t * 0.5f - b * 0.5f, n * 0.5f + t * 0.5f - b * 0.5f,
                               n * 0.5f + t * 0.5f + b * 0.5f, n * 0.5f - t * 0.5f + b * 0.5f};
      for (const Vec3& c : corners) {
        positions.push_back(
            Vec3{centre.x + c.x * size.x, centre.y + c.y * size.y, centre.z + c.z * size.z});
      }
    }
  }
  for (u32 i = 0; i < boxes.size(); ++i) {
    for (u32 f = 0; f < 6; ++f) {
      for (u32 c = 0; c < 4; ++c)
        normals.push_back(k_normals[f]);
    }
  }
  for (u32 face = 0; face < boxes.size() * 6; ++face) {
    const u32 base = face * 4;
    for (const u32 i : {base, base + 1, base + 2, base, base + 2, base + 3})
      indices.push_back(i);
  }
  return mesh_glb(positions, normals, indices, colour, "engine ruins synthetic kit");
}

scene::RuinSocket socket(Vec3 position, Vec3 direction, Vec3 outside) {
  scene::RuinSocket s;
  s.position = position;
  s.direction = direction;
  s.outside = outside;
  return s;
}

void straight_sockets(scene::RuinMember& m, f32 length) {
  m.sockets.push_back(socket(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}));
  m.sockets.push_back(socket(Vec3{length, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}));
}

// A length in metres, for a member's name: "2", "10", "1.5".
std::string metres_name(f32 metres) {
  char text[16];
  std::snprintf(text, sizeof(text), "%g", static_cast<f64>(metres));
  return text;
}

// A percentage, for a member's name.
std::string pct(f32 fraction) {
  return std::to_string(static_cast<u32>(std::floor(fraction * 100.0f + 0.5f)));
}

}  // namespace

SyntheticKitOptions e33_sized_options() {
  SyntheticKitOptions o;
  o.module = 2.0f;
  o.thickness = 0.64f;
  o.wall_height = 2.4f;
  o.short_section = 1;
  o.long_section = 5;
  o.corner_arm_in = 2.68f;
  o.corner_arm_out = 3.68f;
  o.doorway = false;
  o.window = false;
  o.name = "e33-sized-boxes";
  return o;
}

void make_synthetic_kit(const SyntheticKitOptions& o, SyntheticKit& out) {
  out = SyntheticKit{};
  scene::RuinKit& kit = out.kit;
  kit.format = "engine.ruin-kit.v1";
  kit.name = o.name;
  kit.profile = "boxes";
  kit.module = o.module;
  kit.thickness = o.thickness;
  kit.wall_height = o.wall_height;
  const f32 t = o.thickness;
  const f32 half = 0.5f * t;
  const f32 h = o.wall_height;
  constexpr f32 k_fractions[3] = {1.0f, 0.55f, 0.3f};
  const u32 variants = std::clamp<u32>(o.height_variants, 1u, 3u);
  const Vec3 k_section{0.78f, 0.64f, 0.45f};
  const Vec3 k_corner{0.62f, 0.50f, 0.36f};
  const Vec3 k_opening{0.74f, 0.58f, 0.40f};
  const Vec3 k_debris{0.55f, 0.46f, 0.36f};
  auto add = [&](scene::RuinMember member, const Vector<Box>& boxes, const Vec3& colour) {
    const std::string file = member.name + ".glb";
    member.mesh = file;
    out.mesh_names.push_back(file);
    out.glb.push_back(boxes_glb(boxes, colour));
    kit.members.push_back(std::move(member));
  };

  // Sections: each length at each height.
  for (const u32 modules : {o.short_section, o.long_section}) {
    if (modules == 0) continue;
    const f32 length = static_cast<f32>(modules) * o.module;
    for (u32 v = 0; v < variants; ++v) {
      scene::RuinMember m;
      m.name = "section-" + metres_name(length) + "m-h" + pct(k_fractions[v]);
      m.kind = scene::RuinPieceKind::Section;
      m.height = h * k_fractions[v];
      straight_sockets(m, length);
      const Vector<Box> boxes{Box{Vec3{0, 0, -half}, Vec3{length, m.height, half}}};
      add(std::move(m), boxes, k_section);
    }
  }
  // Outside corners: an L of two boxes, the corner square in the outgoing arm's.
  const f32 a = std::max(o.corner_arm_in, half);
  const f32 b = std::max(o.corner_arm_out, half);
  for (u32 v = 0; v < variants; ++v) {
    scene::RuinMember m;
    m.name = "corner-h" + pct(k_fractions[v]);
    m.kind = scene::RuinPieceKind::Corner;
    m.height = h * k_fractions[v];
    m.sockets.push_back(socket(Vec3{0, 0, -a}, Vec3{0, 0, 1}, Vec3{-1, 0, 0}));
    m.sockets.push_back(socket(Vec3{b, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}));
    Vector<Box> boxes;
    boxes.push_back(Box{Vec3{-half, 0, -half}, Vec3{b, m.height, half}});
    if (a > half) boxes.push_back(Box{Vec3{-half, 0, -a}, Vec3{half, m.height, -half}});
    add(std::move(m), boxes, k_corner);
  }
  if (o.inside_corner) {
    scene::RuinMember m;
    m.name = "inside-corner";
    m.kind = scene::RuinPieceKind::InsideCorner;
    m.height = h;
    m.sockets.push_back(socket(Vec3{0, 0, a}, Vec3{0, 0, -1}, Vec3{1, 0, 0}));
    m.sockets.push_back(socket(Vec3{b, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}));
    Vector<Box> boxes;
    boxes.push_back(Box{Vec3{-half, 0, -half}, Vec3{b, h, half}});
    if (a > half) boxes.push_back(Box{Vec3{-half, 0, half}, Vec3{half, h, a}});
    add(std::move(m), boxes, k_corner);
  }
  // Openings, one module wide: two jambs and a lintel over them, and a sill under a window.
  const f32 length = o.module;
  auto opening = [&](const char* name, scene::RuinPieceKind kind, f32 width, f32 sill, f32 head) {
    width = std::min(width, length - 0.2f);
    head = std::min(head, h - 0.2f);
    const f32 o0 = 0.5f * (length - width);
    const f32 o1 = o0 + width;
    scene::RuinMember m;
    m.name = name;
    m.kind = kind;
    m.height = h;
    m.opening_start = o0;
    m.opening_end = o1;
    m.sill = sill;
    m.head = head;
    straight_sockets(m, length);
    Vector<Box> boxes;
    boxes.push_back(Box{Vec3{0, 0, -half}, Vec3{o0, h, half}});
    boxes.push_back(Box{Vec3{o1, 0, -half}, Vec3{length, h, half}});
    boxes.push_back(Box{Vec3{o0, head, -half}, Vec3{o1, h, half}});
    if (sill > 0.0f) boxes.push_back(Box{Vec3{o0, 0, -half}, Vec3{o1, sill, half}});
    add(std::move(m), boxes, k_opening);
  };
  if (o.doorway) opening("doorway", scene::RuinPieceKind::Doorway, 1.1f, 0.0f, 2.1f);
  if (o.window) opening("window", scene::RuinPieceKind::Window, 1.0f, 0.9f, 1.9f);
  // Debris: a block and a slab, standing on the ground about their own centre.
  constexpr f32 k_debris_sizes[3][3] = {
      {0.5f, 0.3f, 0.35f}, {0.7f, 0.18f, 0.45f}, {0.4f, 0.25f, 0.3f}};
  for (u32 d = 0; d < std::min<u32>(o.debris_variants, 3u); ++d) {
    scene::RuinMember m;
    m.name = "debris-" + std::to_string(d);
    m.kind = scene::RuinPieceKind::Debris;
    const f32 sx = 0.5f * k_debris_sizes[d][0];
    const f32 sz = 0.5f * k_debris_sizes[d][2];
    m.height = k_debris_sizes[d][1];
    m.radius = std::ceil(std::sqrt(sx * sx + sz * sz) * 100.0f) / 100.0f;
    const Vector<Box> boxes{Box{Vec3{-sx, 0, -sz}, Vec3{sx, m.height, sz}}};
    add(std::move(m), boxes, k_debris);
  }
}

bool write_synthetic_kit(const std::string& dir, const SyntheticKitOptions& options,
                         std::string* error, std::string* kit_path) {
  SyntheticKit kit;
  make_synthetic_kit(options, kit);
  if (!write_kit_files(dir, kit.mesh_names, kit.glb, error)) return false;
  const std::string path = io::join_path(dir, "kit.json");
  const std::string text = write_json(schema::to_json(kit.kit)) + "\n";
  const io::Status status = io::write_file(path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
    return false;
  }
  if (kit_path != nullptr) *kit_path = path;
  return true;
}

}  // namespace engine::ruins
