// The procedural rock (docs/subsystems/scatter.md, "The procedural rock"): what a kind that names
// no mesh places, so a layer can be looked at with nothing outside the repository. An icosahedron
// subdivided by midpoints, every vertex pushed out along its direction by a seeded value noise,
// stretched to its half-extents and lowered so `buried` of its height is under the origin; welded
// by exact position and given area-weighted normals; one primitive with one material, as a GLB.
//
// Its file is written once under a name that is the hash of its fields and `k_rock_version`, the
// way the city writes its proxy boxes beside its plan: a file already there under the name is the
// right one, and the renderer's derived-data cache clusters it like any mesh. f32 + - * / and sqrt
// only, so the bytes are the same from every toolchain.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>
#include <systems/scatter/scatter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace engine::scatter {

namespace {

constexpr u64 mix(u64 x) noexcept {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

f64 lattice_value(u64 seed, i64 i, i64 j, i64 k) noexcept {
  const u64 h =
      mix(seed ^ mix(static_cast<u64>(i) + mix(static_cast<u64>(j) + mix(static_cast<u64>(k)))));
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}

f64 smooth(f64 t) noexcept { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise in three dimensions, in [0, 1].
f64 noise3(u64 seed, f64 x, f64 y, f64 z) noexcept {
  const f64 fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
  const i64 i = static_cast<i64>(fx), j = static_cast<i64>(fy), k = static_cast<i64>(fz);
  const f64 tx = smooth(x - fx), ty = smooth(y - fy), tz = smooth(z - fz);
  f64 c[2][2];
  for (i64 b = 0; b < 2; ++b) {
    for (i64 a = 0; a < 2; ++a) {
      const f64 v0 = lattice_value(seed, i, j + a, k + b);
      const f64 v1 = lattice_value(seed, i + 1, j + a, k + b);
      c[b][a] = v0 + (v1 - v0) * tx;
    }
  }
  const f64 c0 = c[0][0] + (c[0][1] - c[0][0]) * ty;
  const f64 c1 = c[1][0] + (c[1][1] - c[1][0]) * ty;
  return c0 + (c1 - c0) * tz;
}

Vec3 normalized(Vec3 v) noexcept {
  const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return len > 0.0f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0.0f, 1.0f, 0.0f};
}

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

u32 bits_of(f32 v) noexcept {
  u32 b = 0;
  std::memcpy(&b, &v, 4);
  return b;
}

std::string resolve(std::string_view dir, const std::string& path) {
  return io::is_absolute_path(path) || dir.empty() ? path : io::join_path(dir, path);
}

}  // namespace

void make_rock(const Rock& rock, RockMesh& out) {
  out = RockMesh{};
  // The icosahedron.
  const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
  const Vec3 base[12] = {{-1.0f, t, 0.0f}, {1.0f, t, 0.0f}, {-1.0f, -t, 0.0f}, {1.0f, -t, 0.0f},
                         {0.0f, -1.0f, t}, {0.0f, 1.0f, t}, {0.0f, -1.0f, -t}, {0.0f, 1.0f, -t},
                         {t, 0.0f, -1.0f}, {t, 0.0f, 1.0f}, {-t, 0.0f, -1.0f}, {-t, 0.0f, 1.0f}};
  const u32 faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                            {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                            {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                            {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
  // Triangles as corner positions on the unit sphere, subdivided by midpoints: a shared edge's
  // midpoint is the same sum in either triangle (a + b is b + a), so the weld below finds it.
  Vector<Vec3> tris;
  for (const auto& f : faces) {
    tris.push_back(normalized(base[f[0]]));
    tris.push_back(normalized(base[f[1]]));
    tris.push_back(normalized(base[f[2]]));
  }
  const u32 detail = std::min<u32>(rock.detail, 5u);
  for (u32 d = 0; d < detail; ++d) {
    Vector<Vec3> next;
    next.reserve(tris.size() * 4);
    for (u32 i = 0; i < tris.size(); i += 3) {
      const Vec3 a = tris[i], b = tris[i + 1], c = tris[i + 2];
      const Vec3 ab = normalized(Vec3{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f});
      const Vec3 bc = normalized(Vec3{(b.x + c.x) * 0.5f, (b.y + c.y) * 0.5f, (b.z + c.z) * 0.5f});
      const Vec3 ca = normalized(Vec3{(c.x + a.x) * 0.5f, (c.y + a.y) * 0.5f, (c.z + a.z) * 0.5f});
      const Vec3 quad[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
      for (const Vec3& v : quad)
        next.push_back(v);
    }
    tris = std::move(next);
  }
  // Weld by exact position: sort the corners by their bits and give equal ones one index.
  const u32 corners = tris.size();
  Vector<u32> order(corners);
  for (u32 i = 0; i < corners; ++i)
    order[i] = i;
  const auto key_less = [&](u32 a, u32 b) {
    const Vec3& p = tris[a];
    const Vec3& q = tris[b];
    if (bits_of(p.x) != bits_of(q.x)) return bits_of(p.x) < bits_of(q.x);
    if (bits_of(p.y) != bits_of(q.y)) return bits_of(p.y) < bits_of(q.y);
    if (bits_of(p.z) != bits_of(q.z)) return bits_of(p.z) < bits_of(q.z);
    return a < b;
  };
  std::sort(order.begin(), order.end(), key_less);
  Vector<u32> index_of(corners);
  Vector<Vec3> unit;
  const auto same = [&](u32 a, u32 b) {
    return bits_of(tris[a].x) == bits_of(tris[b].x) && bits_of(tris[a].y) == bits_of(tris[b].y) &&
           bits_of(tris[a].z) == bits_of(tris[b].z);
  };
  for (u32 n = 0; n < corners; ++n) {
    const u32 i = order[n];
    if (n == 0 || !same(order[n - 1], i)) unit.push_back(tris[i]);
    index_of[i] = unit.size() - 1;
  }
  // Pushed out by the noise, stretched, and lowered.
  const u64 seed = mix(0x524F434B53454544ull ^ static_cast<u64>(rock.seed));
  const f64 rough = static_cast<f64>(rock.roughness);
  out.positions.reserve(unit.size());
  for (const Vec3& u : unit) {
    const f64 x = static_cast<f64>(u.x), y = static_cast<f64>(u.y), z = static_cast<f64>(u.z);
    const f64 n = 0.65 * noise3(seed, x * 1.7 + 11.0, y * 1.7 + 23.0, z * 1.7 + 37.0) +
                  0.35 * noise3(seed ^ 1u, x * 3.9 + 5.0, y * 3.9 + 7.0, z * 3.9 + 13.0);
    const f64 r = 1.0 + rough * (2.0 * n - 1.0);
    const f32 lift = rock.size_y * (1.0f - 2.0f * rock.buried);
    out.positions.push_back(Vec3{static_cast<f32>(x * r) * rock.size_x,
                                 static_cast<f32>(y * r) * rock.size_y + lift,
                                 static_cast<f32>(z * r) * rock.size_z});
  }
  out.indices.reserve(corners);
  for (u32 i = 0; i < corners; ++i)
    out.indices.push_back(index_of[i]);
  // Area-weighted normals: the cross product's length is twice the area.
  out.normals.assign(out.positions.size(), Vec3{});
  for (u32 i = 0; i < out.indices.size(); i += 3) {
    const Vec3 a = out.positions[out.indices[i]];
    const Vec3 b = out.positions[out.indices[i + 1]];
    const Vec3 c = out.positions[out.indices[i + 2]];
    const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    for (u32 k = 0; k < 3; ++k) {
      Vec3& acc = out.normals[out.indices[i + k]];
      acc = Vec3{acc.x + n.x, acc.y + n.y, acc.z + n.z};
    }
  }
  for (Vec3& n : out.normals)
    n = normalized(n);
}

Vector<u8> rock_glb(const Rock& rock) {
  RockMesh mesh;
  make_rock(rock, mesh);
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
  const u32 index_offset = bin.size();
  for (const u32 i : mesh.indices)
    put_u32(bin, i);
  const u32 index_bytes = bin.size() - index_offset;
  auto n = [](u32 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\",\"generator\":\"engine scatter rock\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
      "\"indices\":2,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
      num(rock.red) + "," + num(rock.green) + "," + num(rock.blue) +
      ",1],\"metallicFactor\":0,\"roughnessFactor\":0.85}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      n(vertices) + ",\"type\":\"VEC3\",\"min\":[" + num(lo.x) + "," + num(lo.y) + "," + num(lo.z) +
      "],\"max\":[" + num(hi.x) + "," + num(hi.y) + "," + num(hi.z) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      n(vertices) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5125,\"count\":" +
      n(mesh.indices.size()) +
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

std::string rock_file_name(const Rock& rock) {
  JsonWriteOptions compact;
  compact.pretty = false;
  const std::string text = write_json(schema::to_json(rock), compact);
  const u64 h = hash_bytes(text.data(), text.size(), mix(0x524F434Bull + k_rock_version));
  char name[40];
  std::snprintf(name, sizeof(name), "rock-%016llx.glb", static_cast<unsigned long long>(h));
  return name;
}

bool kind_meshes(const Layer& layer, std::string_view dir, Vector<scene_gen::PlacementMesh>& out,
                 std::string* error, std::string_view host_ddc) {
  // Where the rocks go: where the layer says; else the host's derived-data root (`--ddc`, handed
  // down as `scene_gen::Context::ddc`), so a scene in a game repository writes them where its host
  // keeps derived data, as the water's meshes do; else the repository's root (`<repo>/ddc/scatter`,
  // found from the scene's directory as the renderer's cache is, so a committed scene writes
  // nothing into the tree); else `scatter` beside the scene, which is where a scene outside any
  // checkout — a test's scratch directory — keeps them.
  std::string rock_dir;
  if (!layer.meshes.empty()) {
    rock_dir = resolve(dir, layer.meshes);
  } else {
    std::string ddc(host_ddc);
    if (ddc.empty()) {
      std::error_code ec;
      const std::filesystem::path from = std::filesystem::absolute(
          std::filesystem::path(dir.empty() ? "." : std::string(dir)), ec);
      if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
    }
    rock_dir = !ddc.empty() ? io::join_path(ddc, "scatter") : resolve(dir, "scatter");
  }
  for (const Kind& kind : layer.kinds) {
    scene_gen::PlacementMesh mesh;
    if (!kind.mesh.empty()) {
      mesh.path = resolve(dir, kind.mesh);
    } else {
      mesh.path = io::join_path(rock_dir, rock_file_name(*kind.rock));
      if (!io::exists(mesh.path)) {
        if (io::make_directories(rock_dir) != io::Status::Ok) {
          if (error != nullptr) *error = "cannot create " + rock_dir;
          return false;
        }
        const Vector<u8> bytes = rock_glb(*kind.rock);
        const io::Status status = io::write_file_atomic(
            mesh.path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (status != io::Status::Ok) {
          if (error != nullptr)
            *error = "cannot write " + mesh.path + ": " + io::status_name(status);
          return false;
        }
      }
    }
    mesh.name = "scatter/" + std::string(io::file_name(mesh.path));
    out.push_back(std::move(mesh));
  }
  return true;
}

}  // namespace engine::scatter
