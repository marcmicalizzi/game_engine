#include "glb.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace engine::ruins {

namespace {

// A mesh being built: positions, normals and triangles.
struct Mesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<u32> indices;
};

// ---- low: a chamfered box
// ------------------------------------------------------------------------
//
// The box [0, l] x [0, h] x [-d/2, d/2] with every edge cut by a chamfer `k`: six inset faces,
// twelve edge strips and eight corner triangles, 44 triangles on the faces' own 24 vertices, each
// with its face's normal — so the faces shade flat and a chamfer shades from one face's normal to
// the other's, which reads as a bevel. 24 vertices and 44 triangles are one cluster (at most 64 and
// 124); a vertex per polygon corner, 80 of them, was three.
void chamfered_box(f32 l, f32 h, f32 d, f32 k, Mesh& out) {
  const f32 lo[3] = {0.0f, 0.0f, -0.5f * d};
  const f32 hi[3] = {l, h, 0.5f * d};
  const Vec3 centre{0.5f * l, 0.5f * h, 0.0f};
  // Face (a, s) is the side of axis a at bound s (0 low, 1 high); its four vertices are its
  // corners pulled in by `k` along the other two axes, indexed by their bounds on them.
  auto vertex_of = [](u32 a, const u32 bound[3]) {
    const u32 b = (a + 1) % 3;
    const u32 c = (a + 2) % 3;
    return (a * 2 + bound[a]) * 4 + bound[b] + 2 * bound[c];
  };
  out.positions.resize(24);
  out.normals.resize(24);
  for (u32 a = 0; a < 3; ++a) {
    for (u32 v = 0; v < 8; ++v) {
      const u32 bound[3] = {v & 1u, (v >> 1) & 1u, (v >> 2) & 1u};
      f32 p[3];
      for (u32 x = 0; x < 3; ++x) {
        const f32 edge = bound[x] != 0 ? hi[x] : lo[x];
        p[x] = x == a ? edge : (bound[x] != 0 ? edge - k : edge + k);
      }
      f32 n[3] = {0.0f, 0.0f, 0.0f};
      n[a] = bound[a] != 0 ? 1.0f : -1.0f;
      const u32 id = vertex_of(a, bound);
      out.positions[id] = Vec3{p[0], p[1], p[2]};
      out.normals[id] = Vec3{n[0], n[1], n[2]};
    }
  }
  // A triangle, wound counter-clockwise seen from outside: swapped when its normal points at the
  // box's centre.
  auto triangle = [&](u32 i0, u32 i1, u32 i2) {
    const Vec3 p0 = out.positions[i0];
    const Vec3 p1 = out.positions[i1];
    const Vec3 p2 = out.positions[i2];
    const Vec3 middle = (p0 + p1 + p2) * (1.0f / 3.0f);
    const bool flip = dot(cross(p1 - p0, p2 - p0), middle - centre) < 0.0f;
    out.indices.push_back(i0);
    out.indices.push_back(flip ? i2 : i1);
    out.indices.push_back(flip ? i1 : i2);
  };
  auto quad = [&](u32 i0, u32 i1, u32 i2, u32 i3) {
    triangle(i0, i1, i2);
    triangle(i0, i2, i3);
  };
  // Faces: axis a at bound s.
  for (u32 a = 0; a < 3; ++a) {
    for (u32 s = 0; s < 2; ++s) {
      const u32 base = (a * 2 + s) * 4;
      quad(base + 0, base + 1, base + 3, base + 2);
    }
  }
  // Edge strips: along axis a, between face b at sb and face c at sc; their ends are the two
  // faces' vertices at either end of the edge.
  for (u32 a = 0; a < 3; ++a) {
    const u32 b = (a + 1) % 3;
    const u32 c = (a + 2) % 3;
    for (u32 sb = 0; sb < 2; ++sb) {
      for (u32 sc = 0; sc < 2; ++sc) {
        u32 at[2][3];
        for (u32 e = 0; e < 2; ++e) {
          at[e][a] = e;
          at[e][b] = sb;
          at[e][c] = sc;
        }
        quad(vertex_of(b, at[0]), vertex_of(b, at[1]), vertex_of(c, at[1]), vertex_of(c, at[0]));
      }
    }
  }
  // Corner triangles: the three faces' vertices at each corner.
  for (u32 v = 0; v < 8; ++v) {
    const u32 bound[3] = {v & 1u, (v >> 1) & 1u, (v >> 2) & 1u};
    triangle(vertex_of(0, bound), vertex_of(1, bound), vertex_of(2, bound));
  }
}

// ---- mid and high: a rounded, subdivided, eroded block ------------------------------------------

// A lattice value noise in [-1, 1], trilinear between lattice points with a smoothstep: seeded
// integers through the engine's hash and float arithmetic only, so it is the same on every
// toolchain.
f32 lattice(u64 seed, i32 x, i32 y, i32 z) noexcept {
  const u64 h = hash_combine(hash_combine(hash_combine(seed, static_cast<u64>(static_cast<u32>(x))),
                                          static_cast<u64>(static_cast<u32>(y))),
                             static_cast<u64>(static_cast<u32>(z)));
  return static_cast<f32>(h >> 40) / static_cast<f32>(1u << 23) - 1.0f;
}

f32 value_noise(u64 seed, Vec3 p) noexcept {
  const f32 fx = std::floor(p.x);
  const f32 fy = std::floor(p.y);
  const f32 fz = std::floor(p.z);
  const i32 ix = static_cast<i32>(fx);
  const i32 iy = static_cast<i32>(fy);
  const i32 iz = static_cast<i32>(fz);
  auto smooth = [](f32 t) { return t * t * (3.0f - 2.0f * t); };
  const f32 tx = smooth(p.x - fx);
  const f32 ty = smooth(p.y - fy);
  const f32 tz = smooth(p.z - fz);
  auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
  const f32 x00 = lerp(lattice(seed, ix, iy, iz), lattice(seed, ix + 1, iy, iz), tx);
  const f32 x10 = lerp(lattice(seed, ix, iy + 1, iz), lattice(seed, ix + 1, iy + 1, iz), tx);
  const f32 x01 = lerp(lattice(seed, ix, iy, iz + 1), lattice(seed, ix + 1, iy, iz + 1), tx);
  const f32 x11 =
      lerp(lattice(seed, ix, iy + 1, iz + 1), lattice(seed, ix + 1, iy + 1, iz + 1), tx);
  return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

struct Weathering {
  f32 radius = 0.004f;     // how far the arrises are rounded, metres
  f32 amplitude = 0.001f;  // how deep the surface is eaten, metres
  u64 seed = 1;
};

// The box [0, l] x [0, h] x [-d/2, d/2] as a grid of about `triangles` triangles, welded, its
// edges rounded to `w.radius` (every surface point is pushed onto the inner box swept by a sphere
// of that radius) and its surface eaten inward by up to `w.amplitude` of two octaves of noise —
// about 8 cm features and 2 cm pits — along the rounded surface's normal. Smooth normals.
void eroded_block(f32 l, f32 h, f32 d, u32 triangles, const Weathering& w, Mesh& out) {
  const f32 lo[3] = {0.0f, 0.0f, -0.5f * d};
  const f32 hi[3] = {l, h, 0.5f * d};
  const f32 area = 2.0f * (l * h + h * d + l * d);
  const f32 step = std::sqrt(area / std::max(0.5f * static_cast<f32>(triangles), 1.0f));
  u32 n[3];
  for (u32 a = 0; a < 3; ++a) {
    const f32 size = hi[a] - lo[a];
    n[a] = std::max(2u, static_cast<u32>(std::floor(size / step + 0.5f)));
  }
  const u32 sx = n[0] + 1;
  const u32 sy = n[1] + 1;
  const u32 sz = n[2] + 1;
  Vector<u32> id(sx * sy * sz, ~0u);
  const f32 r = std::min({w.radius, 0.45f * l, 0.45f * h, 0.45f * d});
  auto vertex = [&](const u32 g[3]) {
    u32& slot = id[(g[2] * sy + g[1]) * sx + g[0]];
    if (slot != ~0u) return slot;
    f32 p[3];
    f32 q[3];
    f32 dv[3];
    f32 len2 = 0.0f;
    for (u32 a = 0; a < 3; ++a) {
      p[a] = lo[a] + (hi[a] - lo[a]) * (static_cast<f32>(g[a]) / static_cast<f32>(n[a]));
      q[a] = std::clamp(p[a], lo[a] + r, hi[a] - r);
      dv[a] = p[a] - q[a];
      len2 += dv[a] * dv[a];
    }
    const f32 len = std::sqrt(len2);
    const f32 inv = len > 0.0f ? 1.0f / len : 0.0f;
    const Vec3 normal{dv[0] * inv, dv[1] * inv, dv[2] * inv};
    const Vec3 surface{q[0] + normal.x * r, q[1] + normal.y * r, q[2] + normal.z * r};
    const f32 coarse = value_noise(w.seed, surface * 12.0f);
    const f32 fine = value_noise(w.seed ^ 0x9e3779b97f4a7c15ull, surface * 45.0f);
    const f32 eaten = std::clamp(0.5f + 0.35f * coarse + 0.25f * fine, 0.0f, 1.0f) * w.amplitude;
    slot = out.positions.size();
    out.positions.push_back(surface - normal * eaten);
    return slot;
  };
  // Each face: axis a at bound s, a grid over the other two axes b and c (b x c = +a), wound
  // counter-clockwise seen from outside.
  for (u32 a = 0; a < 3; ++a) {
    const u32 b = (a + 1) % 3;
    const u32 c = (a + 2) % 3;
    for (u32 s = 0; s < 2; ++s) {
      for (u32 v = 0; v < n[c]; ++v) {
        for (u32 u = 0; u < n[b]; ++u) {
          u32 g00[3], g10[3], g11[3], g01[3];
          g00[a] = g10[a] = g11[a] = g01[a] = s != 0 ? n[a] : 0;
          g00[b] = u;
          g00[c] = v;
          g10[b] = u + 1;
          g10[c] = v;
          g11[b] = u + 1;
          g11[c] = v + 1;
          g01[b] = u;
          g01[c] = v + 1;
          const u32 i00 = vertex(g00);
          const u32 i10 = vertex(g10);
          const u32 i11 = vertex(g11);
          const u32 i01 = vertex(g01);
          if (s != 0) {
            for (const u32 i : {i00, i10, i11, i00, i11, i01})
              out.indices.push_back(i);
          } else {
            for (const u32 i : {i00, i11, i10, i00, i01, i11})
              out.indices.push_back(i);
          }
        }
      }
    }
  }
  // Smooth normals: the area-weighted sum of the faces around each vertex.
  out.normals.assign(out.positions.size(), Vec3{0.0f, 0.0f, 0.0f});
  for (u32 t = 0; t + 2 < out.indices.size(); t += 3) {
    const u32 i0 = out.indices[t];
    const u32 i1 = out.indices[t + 1];
    const u32 i2 = out.indices[t + 2];
    const Vec3 face =
        cross(out.positions[i1] - out.positions[i0], out.positions[i2] - out.positions[i0]);
    out.normals[i0] = out.normals[i0] + face;
    out.normals[i1] = out.normals[i1] + face;
    out.normals[i2] = out.normals[i2] + face;
  }
  for (Vec3& normal : out.normals) {
    const f32 len = length(normal);
    normal = len > 0.0f ? normal * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
  }
}

std::string centimetres(f32 metres) { return std::to_string(to_cm(metres)); }

}  // namespace

const char* block_fidelity_name(BlockFidelity fidelity) noexcept {
  switch (fidelity) {
    case BlockFidelity::low: return "low";
    case BlockFidelity::mid: return "mid";
    case BlockFidelity::high: return "high";
  }
  return "unknown";
}

bool parse_block_fidelity(std::string_view text, BlockFidelity& out) noexcept {
  if (text == "low") {
    out = BlockFidelity::low;
  } else if (text == "mid") {
    out = BlockFidelity::mid;
  } else if (text == "high") {
    out = BlockFidelity::high;
  } else {
    return false;
  }
  return true;
}

void make_synthetic_block_kit(const SyntheticBlockOptions& o, SyntheticBlockKit& out) {
  out = SyntheticBlockKit{};
  scene::RuinBlockKit& kit = out.kit;
  kit.format = "engine.ruin-block-kit.v1";
  kit.name =
      o.name.empty() ? std::string("synthetic-blocks-") + block_fidelity_name(o.fidelity) : o.name;
  kit.profile = "ashlar";
  // Every size on the centimetre grid the layer decides on, so the kit says exactly what the
  // meshes are.
  auto snapped = [](f32 metres) { return static_cast<f32>(to_cm(metres)) * 0.01f; };
  const f32 t = snapped(o.thickness);
  const f32 course = snapped(o.course);
  const f32 stretcher = snapped(o.stretcher);
  kit.course_height = course;
  kit.thickness = t;
  kit.bond = "running";
  struct Spec {
    const char* role_name;
    scene::RuinBlockRole role;
    f32 length;
    bool eroded;
    u32 variant;
    u32 weight;
  };
  const Spec specs[] = {
      {"stretcher", scene::RuinBlockRole::Stretcher, stretcher, false, 0, 2},
      {"stretcher", scene::RuinBlockRole::Stretcher, stretcher, true, 1, 1},
      {"stretcher", scene::RuinBlockRole::Stretcher, stretcher, true, 2, 1},
      {"stretcher", scene::RuinBlockRole::Stretcher, snapped(0.75f * stretcher), false, 0, 1},
      {"stretcher", scene::RuinBlockRole::Stretcher, snapped(0.75f * stretcher), true, 1, 1},
      {"stretcher", scene::RuinBlockRole::Stretcher, snapped(1.25f * stretcher), false, 0, 1},
      {"stretcher", scene::RuinBlockRole::Stretcher, snapped(1.25f * stretcher), true, 1, 1},
      {"half", scene::RuinBlockRole::Half, snapped(0.5f * stretcher), false, 0, 1},
      {"half", scene::RuinBlockRole::Half, snapped(0.5f * stretcher), true, 1, 1},
      {"quoin", scene::RuinBlockRole::Quoin, snapped(t + 0.5f * stretcher), false, 0, 1},
      {"quoin", scene::RuinBlockRole::Quoin, snapped(t + 0.5f * stretcher), true, 1, 1},
      {"lintel", scene::RuinBlockRole::Lintel, snapped(o.lintel), false, 0, 1},
      {"lintel", scene::RuinBlockRole::Lintel, snapped(o.lintel), true, 1, 1},
      {"sill", scene::RuinBlockRole::Sill, snapped(o.sill), false, 0, 1},
  };
  const u32 target = o.fidelity == BlockFidelity::high ? o.high_triangles : o.mid_triangles;
  u32 index = 0;
  for (const Spec& spec : specs) {
    scene::RuinBlock block;
    block.name = std::string(spec.role_name) + "-" + centimetres(spec.length) +
                 (spec.eroded ? "-eroded-" + std::to_string(spec.variant) : std::string("-crisp"));
    block.role = spec.role;
    block.length = spec.length;
    block.height = course;
    block.depth = t;
    block.eroded = spec.eroded;
    block.weight = spec.weight;
    const std::string file = block.name + ".glb";
    block.mesh = file;
    Mesh mesh;
    if (o.fidelity == BlockFidelity::low) {
      chamfered_box(spec.length, course, t, spec.eroded ? 0.03f : 0.01f, mesh);
    } else {
      Weathering w;
      w.seed = hash_combine(hash_combine(o.seed, index), spec.variant);
      w.radius = spec.eroded ? (spec.variant == 2 ? 0.035f : 0.025f) : 0.004f;
      w.amplitude = spec.eroded ? (spec.variant == 2 ? 0.012f : 0.009f) : 0.0008f;
      eroded_block(spec.length, course, t, target, w, mesh);
    }
    // Sandstone, the weathered blocks a shade darker and the dressed pieces (quoin, lintel, sill)
    // a shade cooler, so a capture tells them apart.
    const bool dressed =
        spec.role != scene::RuinBlockRole::Stretcher && spec.role != scene::RuinBlockRole::Half;
    const Vec3 colour = dressed
                            ? (spec.eroded ? Vec3{0.70f, 0.58f, 0.43f} : Vec3{0.74f, 0.62f, 0.47f})
                            : (spec.eroded ? Vec3{0.76f, 0.61f, 0.43f} : Vec3{0.81f, 0.67f, 0.48f});
    out.mesh_names.push_back(file);
    out.triangles.push_back(mesh.indices.size() / 3);
    out.glb.push_back(mesh_glb(mesh.positions, mesh.normals, mesh.indices, colour,
                               "engine ruins synthetic blocks"));
    kit.blocks.push_back(std::move(block));
    ++index;
  }
}

bool write_synthetic_block_kit(const std::string& dir, const SyntheticBlockOptions& options,
                               std::string* error, std::string* kit_path) {
  SyntheticBlockKit kit;
  make_synthetic_block_kit(options, kit);
  if (!write_kit_files(dir, kit.mesh_names, kit.glb, error)) return false;
  const std::string path = io::join_path(dir, "block-kit.json");
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
