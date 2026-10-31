// The water capability's bodies, meshes and depths (water.h; docs/subsystems/water.md).
#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <systems/water/water.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::water {

namespace {

constexpr f64 k_two_pi = 6.283185307179586476925286766559;
constexpr f64 k_half_pi = 1.5707963267948966192313216916398;

f32 cross2(Vec2 a, Vec2 b) noexcept { return a.x * b.y - a.y * b.x; }
f32 length2(Vec2 a) noexcept { return std::sqrt(a.x * a.x + a.y * a.y); }

bool finite(f32 v) noexcept { return std::isfinite(v); }

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

// The edge's samples, metres from the centre in (x, z), in the outline's turning order.
void edge_samples(const Body& body, f32 spacing, Vector<Vec2>& out) {
  out.clear();
  if (body.circle()) {
    const f64 r = static_cast<f64>(body.radius_m);
    const u32 n =
        std::max<u32>(8u, static_cast<u32>(std::ceil(k_two_pi * r / static_cast<f64>(spacing))));
    out.reserve(n);
    for (u32 j = 0; j < n; ++j) {
      f64 s = 0.0;
      f64 c = 0.0;
      sin_cos(k_two_pi * static_cast<f64>(j) / static_cast<f64>(n), s, c);
      out.push_back(Vec2{static_cast<f32>(r * c), static_cast<f32>(r * s)});
    }
    return;
  }
  const u32 points = body.outline.size();
  for (u32 i = 0; i < points; ++i) {
    const Vec2 a = body.outline[i];
    const Vec2 b = body.outline[(i + 1) % points];
    const Vec2 d{b.x - a.x, b.y - a.y};
    const u32 n = std::max<u32>(1u, static_cast<u32>(std::ceil(length2(d) / spacing)));
    for (u32 k = 0; k < n; ++k) {
      const f32 t = static_cast<f32>(k) / static_cast<f32>(n);
      out.push_back(Vec2{a.x + d.x * t, a.y + d.y * t});
    }
  }
}

u32 ring_count(const Vector<Vec2>& edge, f32 spacing) noexcept {
  f32 reach = 0.0f;
  for (const Vec2& b : edge)
    reach = std::max(reach, length2(b));
  return std::max<u32>(1u, static_cast<u32>(std::ceil(reach / spacing)));
}

}  // namespace

void sin_cos(f64 radians, f64& s, f64& c) noexcept {
  // To [-pi/4, pi/4] by quarter turns, then the Taylor series to the 15th order, whose error there
  // is below 1e-15.
  const f64 q = std::floor(radians / k_half_pi + 0.5);
  const f64 r = radians - q * k_half_pi;
  const f64 r2 = r * r;
  const f64 sr =
      r *
      (1.0 -
       r2 / 6.0 *
           (1.0 - r2 / 20.0 *
                      (1.0 - r2 / 42.0 *
                                 (1.0 - r2 / 72.0 *
                                            (1.0 - r2 / 110.0 *
                                                       (1.0 - r2 / 156.0 * (1.0 - r2 / 210.0)))))));
  const f64 cr =
      1.0 -
      r2 / 2.0 *
          (1.0 -
           r2 / 12.0 *
               (1.0 - r2 / 30.0 *
                          (1.0 - r2 / 56.0 *
                                     (1.0 - r2 / 90.0 * (1.0 - r2 / 132.0 * (1.0 - r2 / 182.0))))));
  const i64 quadrant = static_cast<i64>(q) & 3;
  switch (quadrant) {
    case 0:
      s = sr;
      c = cr;
      break;
    case 1:
      s = cr;
      c = -sr;
      break;
    case 2:
      s = -sr;
      c = -cr;
      break;
    default:
      s = -cr;
      c = sr;
      break;
  }
}

bool read_body(const scene::WaterBody& block, Body& out, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = "water body \"" + block.name + "\": " + std::move(sentence);
    return false;
  };
  out = Body{};
  out.name = block.name;
  out.level_m = block.level_m;
  if (!std::isfinite(block.level_m)) return fail("its level_m is not a number");
  const bool circle = block.shape.circle.has_value();
  const bool polygon = !block.shape.polygon.empty();
  if (circle == polygon) return fail("its shape must be exactly one of a circle and a polygon");
  if (circle) {
    const scene::WaterCircle& c = *block.shape.circle;
    if (!(c.radius_m > 0.0f) || !finite(c.radius_m))
      return fail("its circle's radius_m must be positive");
    out.radius_m = c.radius_m;
    out.center = WorldPos{c.center.x, block.level_m, c.center.z};
  } else {
    const u32 n = block.shape.polygon.size();
    if (n < 3) return fail("its polygon has fewer than three points");
    // The area centroid, in f64 from the first point (the points may be far from the origin).
    const WorldPos p0 = block.shape.polygon[0];
    f64 area2 = 0.0;
    f64 cx = 0.0;
    f64 cz = 0.0;
    for (u32 i = 0; i < n; ++i) {
      const WorldPos& a = block.shape.polygon[i];
      const WorldPos& b = block.shape.polygon[(i + 1) % n];
      const f64 ax = a.x - p0.x, az = a.z - p0.z, bx = b.x - p0.x, bz = b.z - p0.z;
      const f64 cross = ax * bz - bx * az;
      area2 += cross;
      cx += (ax + bx) * cross;
      cz += (az + bz) * cross;
    }
    if (!(std::fabs(area2) > 1e-6)) return fail("its polygon has no area");
    out.center = WorldPos{p0.x + cx / (3.0 * area2), block.level_m, p0.z + cz / (3.0 * area2)};
    out.outline.reserve(n);
    for (u32 i = 0; i < n; ++i) {
      const WorldPos& p = block.shape.polygon[i];
      out.outline.push_back(
          Vec2{static_cast<f32>(p.x - out.center.x), static_cast<f32>(p.z - out.center.z)});
    }
    // One turning order: positive area in (x, z).
    if (area2 < 0.0) std::reverse(out.outline.begin(), out.outline.end());
    for (u32 i = 0; i < n; ++i) {
      if (!(cross2(out.outline[i], out.outline[(i + 1) % n]) > 0.0f)) {
        return fail("its polygon is not star-shaped from its centroid (edge " + std::to_string(i) +
                    " does not turn the way the others do), which the mesh's rings need");
      }
    }
  }
  if (!(block.shore_m >= 0.0f) || !finite(block.shore_m))
    return fail("its shore_m must not be negative");
  if (!(block.depth_fade_m > 0.0f) || !finite(block.depth_fade_m))
    return fail("its depth_fade_m must be positive");
  if (!(block.wave_amplitude_m >= 0.0f) || !finite(block.wave_amplitude_m))
    return fail("its wave_amplitude_m must not be negative");
  if (!(block.wave_length_m > 0.0f) || !finite(block.wave_length_m))
    return fail("its wave_length_m must be positive");
  if (!(block.chop >= 0.0f && block.chop <= 1.0f)) return fail("its chop must be within [0, 1]");
  if (!(block.roughness >= 0.02f && block.roughness <= 1.0f))
    return fail("its roughness must be within [0.02, 1]");
  if (!finite(block.direction_deg)) return fail("its direction_deg is not a number");
  // The volume tier's optics (water.md, "Translucent by volume"): rates per metre that cannot be
  // negative, a share, and a sediment load.
  const auto per_metre = [](const std::optional<Vec3>& v) {
    return !v.has_value() || (v->x >= 0.0f && v->y >= 0.0f && v->z >= 0.0f && finite(v->x) &&
                              finite(v->y) && finite(v->z));
  };
  if (!per_metre(block.absorption))
    return fail("its absorption must be three rates per metre, none negative");
  if (!per_metre(block.scattering))
    return fail("its scattering must be three rates per metre, none negative");
  if (!(block.transmission >= 0.0f && block.transmission <= 1.0f))
    return fail("its transmission must be within [0, 1]");
  if (!(block.turbidity >= 0.0f) || !finite(block.turbidity))
    return fail("its turbidity must not be negative");
  // The open sea's spectrum (water.md, "The spectrum").
  if (!(block.wind_m_s >= 0.0f) || !finite(block.wind_m_s))
    return fail("its wind_m_s must not be negative");
  if (!(block.fetch_km > 0.0f) || !finite(block.fetch_km))
    return fail("its fetch_km must be positive");
  out.shore_m = block.shore_m;
  out.open = block.open;
  return true;
}

f32 inside_distance(const Body& body, f32 x, f32 z) noexcept {
  const Vec2 q{x, z};
  const f32 r = length2(q);
  if (body.circle()) return body.radius_m - r;
  const u32 n = body.outline.size();
  // The wedge whose edge the ray from the centre through q crosses.
  const Vec2 dir = r > 1e-6f ? Vec2{x / r, z / r} : Vec2{1.0f, 0.0f};
  for (u32 i = 0; i < n; ++i) {
    const Vec2 a = body.outline[i];
    const Vec2 b = body.outline[(i + 1) % n];
    if (cross2(a, dir) < 0.0f || cross2(dir, b) < 0.0f) continue;
    const Vec2 ab{b.x - a.x, b.y - a.y};
    const f32 denom = cross2(dir, ab);
    if (!(denom > 0.0f)) continue;
    // The edge's point along the ray, at `reach` from the centre.
    const f32 reach = cross2(a, b) / denom;
    return reach - r;
  }
  return -1.0f;
}

f32 depth_at(std::span<const Body> bodies, const scene_gen::Ground& ground, WorldPos at) noexcept {
  f32 deepest = 0.0f;
  for (const Body& body : bodies) {
    const f32 x = static_cast<f32>(at.x - body.center.x);
    const f32 z = static_cast<f32>(at.z - body.center.z);
    // An open body (water.md, "To the horizon") holds every point whose ground is below its level,
    // inside its outline or past it, which is where the resolve draws it.
    if (!body.open && inside_distance(body, x, z) < 0.0f) continue;
    const f64 depth = body.level_m - ground.surface(at);
    if (depth > static_cast<f64>(deepest)) deepest = static_cast<f32>(depth);
  }
  return deepest;
}

f32 spacing_for(const Body& body) noexcept { return std::clamp(body.shore_m / 3.0f, 0.25f, 1.0f); }

void build_mesh(const Body& body, const scene_gen::Ground& ground, Mesh& out) {
  out = Mesh{};
  f32 spacing = spacing_for(body);
  Vector<Vec2> edge;
  u32 rings = 0;
  for (;;) {
    edge_samples(body, spacing, edge);
    rings = ring_count(edge, spacing);
    if (u64{edge.size()} * rings + 1 <= k_max_vertices) break;
    spacing *= 1.25f;
  }
  const u32 m = edge.size();
  out.spacing = spacing;
  out.edge = m;
  out.rings = rings;
  const u32 vertices = m * rings + 1;
  out.positions.reserve(vertices);
  out.normals.reserve(vertices);
  out.uvs.reserve(vertices);
  const auto add = [&](Vec2 p, f32 from_edge) {
    out.positions.push_back(Vec3{p.x, 0.0f, p.y});
    out.normals.push_back(Vec3{0.0f, 1.0f, 0.0f});
    const WorldPos at{body.center.x + static_cast<f64>(p.x), body.level_m,
                      body.center.z + static_cast<f64>(p.y)};
    const f64 depth = std::max(0.0, body.level_m - ground.surface(at));
    out.max_depth = std::max(out.max_depth, static_cast<f32>(depth));
    out.uvs.push_back(Vec2{from_edge, static_cast<f32>(depth)});
  };
  for (u32 k = 0; k < rings; ++k) {
    const f32 t = 1.0f - static_cast<f32>(k) / static_cast<f32>(rings);
    for (u32 j = 0; j < m; ++j) {
      const Vec2 b = edge[j];
      add(Vec2{b.x * t, b.y * t}, (1.0f - t) * length2(b));
    }
  }
  f32 centre_edge = 0.0f;
  for (const Vec2& b : edge)
    centre_edge = centre_edge == 0.0f ? length2(b) : std::min(centre_edge, length2(b));
  add(Vec2{0.0f, 0.0f}, centre_edge);
  const u32 centre = vertices - 1;
  // Facing +y: the triangle's normal is (b - a) x (c - a), whose y is (c - a).x (b - a).z less
  // (b - a).x (c - a).z; a triangle wound the other way is turned.
  const auto tri = [&](u32 a, u32 b, u32 c) {
    const Vec3 pa = out.positions[a], pb = out.positions[b], pc = out.positions[c];
    const f32 ny = (pc.x - pa.x) * (pb.z - pa.z) - (pb.x - pa.x) * (pc.z - pa.z);
    out.indices.push_back(a);
    if (ny >= 0.0f) {
      out.indices.push_back(b);
      out.indices.push_back(c);
    } else {
      out.indices.push_back(c);
      out.indices.push_back(b);
    }
  };
  out.indices.reserve((rings - 1) * m * 6 + m * 3);
  for (u32 k = 0; k + 1 < rings; ++k) {
    for (u32 j = 0; j < m; ++j) {
      const u32 j1 = (j + 1) % m;
      const u32 a = k * m + j, b = k * m + j1, c = (k + 1) * m + j1, d = (k + 1) * m + j;
      tri(a, b, c);
      tri(a, c, d);
    }
  }
  for (u32 j = 0; j < m; ++j)
    tri((rings - 1) * m + j, (rings - 1) * m + (j + 1) % m, centre);
}

Vector<u8> mesh_glb(const Mesh& mesh, Vec3 shallow, f32 roughness) {
  Vector<u8> bin;
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (const Vec3& p : mesh.positions) {
    lo = min(lo, p);
    hi = max(hi, p);
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 vertices = mesh.positions.size();
  const u32 normal_offset = bin.size();
  for (const Vec3& n : mesh.normals) {
    put_f32(bin, n.x);
    put_f32(bin, n.y);
    put_f32(bin, n.z);
  }
  const u32 uv_offset = bin.size();
  for (const Vec2& uv : mesh.uvs) {
    put_f32(bin, uv.x);
    put_f32(bin, uv.y);
  }
  const u32 index_offset = bin.size();
  for (const u32 i : mesh.indices)
    put_u32(bin, i);
  const u32 index_bytes = bin.size() - index_offset;
  auto n = [](u32 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\",\"generator\":\"engine water " + n(k_mesh_version) +
      "\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"name\":\"water\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
      num(shallow.x) + "," + num(shallow.y) + "," + num(shallow.z) +
      ",1],\"metallicFactor\":0,\"roughnessFactor\":" + num(roughness) +
      "}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      n(vertices) + ",\"type\":\"VEC3\",\"min\":[" + num(lo.x) + "," + num(lo.y) + "," + num(lo.z) +
      "],\"max\":[" + num(hi.x) + "," + num(hi.y) + "," + num(hi.z) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      n(vertices) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      n(vertices) +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5125,\"count\":" +
      n(mesh.indices.size()) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
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

std::string mesh_file_name(std::span<const u8> glb) {
  const u64 h = hash_bytes(glb.data(), glb.size(), 0x5741544552ull + k_mesh_version);
  char name[40];
  std::snprintf(name, sizeof(name), "water-%016llx.glb", static_cast<unsigned long long>(h));
  return name;
}

void build_depth_map(const Body& body, const Mesh& mesh, const scene_gen::Ground& ground,
                     DepthMap& out) {
  out = DepthMap{};
  if (mesh.positions.empty() || mesh.edge == 0) return;
  // The bounding square of the outline: the edge ring is the first `edge` vertices.
  f32 lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, hi_z = -1e30f;
  for (u32 i = 0; i < mesh.edge; ++i) {
    lo_x = std::min(lo_x, mesh.positions[i].x);
    hi_x = std::max(hi_x, mesh.positions[i].x);
    lo_z = std::min(lo_z, mesh.positions[i].z);
    hi_z = std::max(hi_z, mesh.positions[i].z);
  }
  const f64 side = static_cast<f64>(std::max(hi_x - lo_x, hi_z - lo_z));
  const f64 target = static_cast<f64>(mesh.spacing) * 0.25;
  const f64 wanted = target > 0.0 ? std::ceil(side / target) + 1.0 : 0.0;
  const u32 size = static_cast<u32>(
      std::clamp(wanted, static_cast<f64>(k_depth_map_min), static_cast<f64>(k_depth_map_max)));
  const f64 texel = side / static_cast<f64>(size - 1);
  const f64 x0 = 0.5 * (static_cast<f64>(lo_x) + static_cast<f64>(hi_x)) - 0.5 * side;
  const f64 z0 = 0.5 * (static_cast<f64>(lo_z) + static_cast<f64>(hi_z)) - 0.5 * side;
  out.size = size;
  out.origin = Vec2{static_cast<f32>(x0), static_cast<f32>(z0)};
  out.texel_m = static_cast<f32>(texel);
  // The depths in f64 first, then the range they are quantized over.
  Vector<f64> depth(size * size);
  f64 deepest = 0.0;
  for (u32 j = 0; j < size; ++j) {
    for (u32 i = 0; i < size; ++i) {
      // The texel's centre as the reader computes it, from the stored f32 frame.
      const f64 x =
          static_cast<f64>(out.origin.x) + static_cast<f64>(i) * static_cast<f64>(out.texel_m);
      const f64 z =
          static_cast<f64>(out.origin.y) + static_cast<f64>(j) * static_cast<f64>(out.texel_m);
      const WorldPos at{body.center.x + x, body.level_m, body.center.z + z};
      const f64 d = std::max(0.0, body.level_m - ground.surface(at));
      depth[j * size + i] = d;
      deepest = std::max(deepest, d);
    }
  }
  out.depth_max = static_cast<f32>(deepest);
  out.texels.resize(size * size);
  const f64 scale = out.depth_max > 0.0f ? 65535.0 / static_cast<f64>(out.depth_max) : 0.0;
  for (u32 t = 0; t < size * size; ++t)
    out.texels[t] = static_cast<u16>(std::clamp(std::floor(depth[t] * scale + 0.5), 0.0, 65535.0));
}

Vector<u8> depth_map_bytes(const DepthMap& map) {
  Vector<u8> out;
  out.reserve(32 + map.texels.size() * 2);
  put_u32(out, 0x4d445745u);  // "EWDM"
  put_u32(out, k_depth_map_version);
  put_u32(out, map.size);
  put_u32(out, 0u);
  put_f32(out, map.origin.x);
  put_f32(out, map.origin.y);
  put_f32(out, map.texel_m);
  put_f32(out, map.depth_max);
  for (const u16 t : map.texels) {
    out.push_back(static_cast<u8>(t & 0xffu));
    out.push_back(static_cast<u8>(t >> 8));
  }
  return out;
}

bool read_depth_map(std::span<const u8> bytes, DepthMap& out, std::string* error) {
  out = DepthMap{};
  auto u32_at = [&](usize at) {
    return u32{bytes[at]} | (u32{bytes[at + 1]} << 8) | (u32{bytes[at + 2]} << 16) |
           (u32{bytes[at + 3]} << 24);
  };
  auto f32_at = [&](usize at) {
    const u32 bits = u32_at(at);
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  };
  if (bytes.size() < 32 || u32_at(0) != 0x4d445745u) {
    if (error != nullptr) *error = "not a water depth map";
    return false;
  }
  if (u32_at(4) != k_depth_map_version) {
    if (error != nullptr) *error = "a water depth map of version " + std::to_string(u32_at(4));
    return false;
  }
  const u32 size = u32_at(8);
  if (size < 2 || size > k_depth_map_max || bytes.size() != 32 + usize{size} * size * 2) {
    if (error != nullptr) *error = "a water depth map whose size does not match its bytes";
    return false;
  }
  out.size = size;
  out.origin = Vec2{f32_at(16), f32_at(20)};
  out.texel_m = f32_at(24);
  out.depth_max = f32_at(28);
  out.texels.resize(size * size);
  for (u32 t = 0; t < size * size; ++t)
    out.texels[t] =
        static_cast<u16>(u32{bytes[32 + usize{t} * 2]} | (u32{bytes[33 + usize{t} * 2]} << 8));
  return true;
}

std::string body_file_stem(std::span<const u8> glb, std::span<const u8> map) {
  u64 h = hash_bytes(glb.data(), glb.size(), 0x5741544552ull + k_mesh_version);
  h = hash_bytes(map.data(), map.size(), h ^ (0x4445505448ull + k_depth_map_version));
  char name[40];
  std::snprintf(name, sizeof(name), "water-%016llx", static_cast<unsigned long long>(h));
  return name;
}

JsonValue generator_params(std::span<const scene::WaterBody> bodies) {
  JsonValue list = JsonValue::array();
  for (const scene::WaterBody& body : bodies)
    list.push_back(schema::to_json(body));
  JsonValue params = JsonValue::object();
  params.set("bodies", std::move(list));
  return params;
}

}  // namespace engine::water
