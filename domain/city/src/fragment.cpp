// The proxy fragment (docs/subsystems/city.md, "The proxy fragment"): a dozen unit boxes, each
// instanced per element of the plan and its buildings and scaled to it.
//
// **Every proxy has one anchor, and a tile holds the proxies anchored in it.** A building's element
// (a slab, a wall, a window, a core) is anchored at its bottom face's centre; the ground (a
// street's segment, a park) is cut along the tile grid first, so each piece lies in one tile and a
// park that spans six tiles is six pieces, one per tile. So the union of every tile's proxies is
// the whole plan's, piece for piece, which is the identity the tests assert, and a tile
// materialized alone is the same as the same tile cut from the whole.
#include "grid.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/city/fragment.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace engine::city {

namespace {

constexpr u64 k_purpose_tree = 0x74ee;
constexpr i32 k_slab_cm = 25;
constexpr i32 k_ground_cm = 10;
constexpr i32 k_tree_spacing_cm = 1400;

constexpr const char* k_mesh_names[k_proxy_meshes] = {
    "massing", "civic", "slab", "exterior_wall", "interior_wall", "core",
    "window",  "road",  "lane", "park",          "plaza",         "tree"};

// Linear-light base colours, chosen to read apart in a grey fly-through.
constexpr f32 k_mesh_colours[k_proxy_meshes][3] = {
    {0.55f, 0.55f, 0.58f}, {0.62f, 0.45f, 0.40f}, {0.35f, 0.35f, 0.37f}, {0.70f, 0.68f, 0.62f},
    {0.80f, 0.80f, 0.78f}, {0.45f, 0.40f, 0.55f}, {0.20f, 0.35f, 0.55f}, {0.10f, 0.10f, 0.11f},
    {0.30f, 0.27f, 0.24f}, {0.18f, 0.40f, 0.14f}, {0.60f, 0.55f, 0.45f}, {0.10f, 0.28f, 0.08f}};

Rect plan_rect(const Building& b, const Rect& local) {
  i64 ax, az, bx, bz;
  to_plan(b, local.x0, local.z0, ax, az);
  to_plan(b, local.x1, local.z1, bx, bz);
  return Rect{static_cast<i32>(std::min(ax, bx)), static_cast<i32>(std::min(az, bz)),
              static_cast<i32>(std::max(ax, bx)), static_cast<i32>(std::max(az, bz))};
}

Proxy box(const Rect& r, i32 y, i32 height, ProxyMesh mesh, u32 lot, u16 floor) {
  Proxy p;
  p.x = static_cast<i32>((i64{r.x0} + r.x1) / 2);
  p.z = static_cast<i32>((i64{r.z0} + r.z1) / 2);
  p.y = y;
  p.sx = r.width();
  p.sz = r.depth();
  p.sy = height;
  p.mesh = mesh;
  p.lot = lot;
  p.floor = floor;
  return p;
}

// A wall's box in the building's frame: its centre line thickened.
Rect wall_rect(i32 x0, i32 z0, i32 x1, i32 z1, i32 thickness) {
  const i32 h = thickness / 2;
  if (z0 == z1) return Rect{x0, z0 - h, x1, z0 - h + thickness};
  return Rect{x0 - h, z0, x0 - h + thickness, z1};
}

// Cuts `r` along the tile grid and hands each piece to `fn`.
template <class F>
void tile_pieces(const Plan& plan, const Rect& r, F&& fn) {
  if (r.width() <= 0 || r.depth() <= 0) return;
  const TileCoord lo = tile_of(plan, r.x0, r.z0);
  const TileCoord hi = tile_of(plan, i64{r.x1} - 1, i64{r.z1} - 1);
  for (i32 z = lo.z; z <= hi.z; ++z) {
    for (i32 x = lo.x; x <= hi.x; ++x) {
      const Rect t = tile_rect(plan, TileCoord{x, z});
      const Rect piece{std::max(r.x0, t.x0), std::max(r.z0, t.z0), std::min(r.x1, t.x1),
                       std::min(r.z1, t.z1)};
      if (piece.width() > 0 && piece.depth() > 0) fn(piece);
    }
  }
}

Rect segment_rect(const Plan& plan, const Segment& s) {
  const Node& a = plan.nodes[s.a];
  const Node& b = plan.nodes[s.b];
  const i32 h = plan.streets[s.street].width_cm / 2;
  if (a.z == b.z) return Rect{std::min(a.x, b.x), a.z - h, std::max(a.x, b.x), a.z + h};
  return Rect{a.x - h, std::min(a.z, b.z), a.x + h, std::max(a.z, b.z)};
}

void segment_proxies(const Plan& plan, const Segment& s, const Rect* area, Vector<Proxy>& out) {
  const StreetClass cls = plan.streets[s.street].cls;
  const ProxyMesh mesh = cls == StreetClass::Alley || cls == StreetClass::Pedestrian
                             ? ProxyMesh::lane
                             : ProxyMesh::road;
  tile_pieces(plan, segment_rect(plan, s), [&](const Rect& piece) {
    const Proxy p = box(piece, 0, k_ground_cm, mesh, k_no_id, 0);
    if (area == nullptr || proxy_in(p, *area)) out.push_back(p);
  });
}

void park_proxies(const Plan& plan, u32 index, const Rect* area, Vector<Proxy>& out) {
  const Park& park = plan.parks[index];
  const ProxyMesh mesh = park.kind == ParkKind::Square ? ProxyMesh::plaza : ProxyMesh::park;
  tile_pieces(plan, park.rect, [&](const Rect& piece) {
    const Proxy p = box(piece, 0, k_ground_cm, mesh, k_no_id, 0);
    if (area == nullptr || proxy_in(p, *area)) out.push_back(p);
  });
  if (park.kind == ParkKind::Square) return;
  // Trees on a jittered grid, drawn from the park's own seed, so a tree is where it is whichever
  // tile asks.
  const u64 seed =
      grid::draw(plan.params.seed, k_purpose_tree,
                 park.lot != k_no_id ? plan.lots[park.lot].id : plan.blocks[park.block].id);
  u32 k = 0;
  for (i32 z = park.rect.z0 + k_tree_spacing_cm / 2; z < park.rect.z1; z += k_tree_spacing_cm) {
    for (i32 x = park.rect.x0 + k_tree_spacing_cm / 2; x < park.rect.x1; x += k_tree_spacing_cm) {
      const u64 d = grid::draw(seed, k_purpose_tree, k++);
      if (!grid::chance(d, 45000)) continue;
      const i32 jx = grid::between(d >> 16, -400, 400);
      const i32 jz = grid::between(d >> 24, -400, 400);
      const i32 tx = std::clamp(x + jx, park.rect.x0 + 100, park.rect.x1 - 100);
      const i32 tz = std::clamp(z + jz, park.rect.z0 + 100, park.rect.z1 - 100);
      const i32 height = grid::between(d >> 40, 500, 1100);
      Proxy p =
          box(Rect{tx - 60, tz - 60, tx + 60, tz + 60}, 0, height, ProxyMesh::tree, k_no_id, 0);
      if (area == nullptr || proxy_in(p, *area)) out.push_back(p);
    }
  }
}

// One unit box: a metre on a side, its bottom face centred on the origin, counter-clockwise faces
// seen from outside, one metallic-roughness material of the mesh's colour.
Vector<u8> box_glb(const f32 colour[3]) {
  static constexpr f32 k_faces[6][4][3] = {
      {{-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 1, 0.5f}, {-0.5f, 1, 0.5f}},      // +z
      {{0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}, {-0.5f, 1, -0.5f}, {0.5f, 1, -0.5f}},  // -z
      {{0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {0.5f, 1, -0.5f}, {0.5f, 1, 0.5f}},      // +x
      {{-0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {-0.5f, 1, 0.5f}, {-0.5f, 1, -0.5f}},  // -x
      {{-0.5f, 1, 0.5f}, {0.5f, 1, 0.5f}, {0.5f, 1, -0.5f}, {-0.5f, 1, -0.5f}},    // +y
      {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}}};   // -y
  static constexpr f32 k_normals[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                          {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
  Vector<u8> bin;
  auto put_f32 = [](Vector<u8>& v, f32 x) {
    u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(x));
    std::memcpy(&bits, &x, 4);
    for (u32 i = 0; i < 4; ++i)
      v.push_back(static_cast<u8>(bits >> (8 * i)));
  };
  auto put_u32 = [](Vector<u8>& v, u32 x) {
    for (u32 i = 0; i < 4; ++i)
      v.push_back(static_cast<u8>(x >> (8 * i)));
  };
  for (const auto& face : k_faces) {
    for (const auto& vtx : face) {
      for (const f32 c : vtx)
        put_f32(bin, c);
    }
  }
  const u32 normal_offset = bin.size();
  for (const auto& n : k_normals) {
    for (u32 v = 0; v < 4; ++v) {
      for (const f32 c : n)
        put_f32(bin, c);
    }
  }
  const u32 index_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    const u32 base = f * 4;
    for (const u32 i : {base, base + 1, base + 2, base, base + 2, base + 3})
      put_u32(bin, i);
  }
  const u32 index_bytes = bin.size() - index_offset;
  auto num = [](f32 v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", static_cast<f64>(v));
    return std::string(text);
  };
  const std::string n = std::to_string(normal_offset);
  std::string json =
      "{\"asset\":{\"version\":\"2.0\",\"generator\":\"engine city proxies\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
      "\"indices\":2,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
      num(colour[0]) + "," + num(colour[1]) + "," + num(colour[2]) +
      ",1],\"metallicFactor\":0,\"roughnessFactor\":0.9}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,0,-0.5],\"max\":[0.5,1,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5125,\"count\":36,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n + "},{\"buffer\":0,\"byteOffset\":" + n +
      ",\"byteLength\":" + std::to_string(index_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + std::to_string(index_offset) +
      ",\"byteLength\":" + std::to_string(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  Vector<u8> glb;
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

std::string mesh_path(const std::string& mesh, std::string_view fragment_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path absolute = fs::absolute(fs::path(mesh), ec);
  if (ec) return mesh;
  const fs::path dir =
      fs::absolute(fs::path(std::string(fragment_dir.empty() ? "." : fragment_dir)), ec);
  if (ec) return absolute.generic_string();
  const fs::path relative = fs::relative(absolute, dir, ec);
  if (ec || relative.empty()) return absolute.generic_string();
  return relative.generic_string();
}

}  // namespace

const char* proxy_mesh_name(ProxyMesh mesh) noexcept {
  const u32 i = static_cast<u32>(mesh);
  return i < k_proxy_meshes ? k_mesh_names[i] : "unknown";
}

void append_building_proxies(const Plan& plan, const Building& b, Stage detail,
                             Vector<Proxy>& out) {
  (void)plan;
  const u32 lot = b.lot_id;
  const Rect foot = plan_rect(b, b.footprint);
  if (detail == Stage::Massing || b.stage == Stage::Massing) {
    out.push_back(box(foot, 0, b.height_cm(),
                      b.archetype == Archetype::CivicShell ? ProxyMesh::civic : ProxyMesh::massing,
                      lot, 0));
    return;
  }
  const i32 ext = plan.params.rules.exterior_cm;
  for (u32 f = 0; f < b.floors.size(); ++f) {
    const Floor& fl = b.floors[f];
    out.push_back(box(foot, fl.elevation_cm, k_slab_cm, ProxyMesh::slab, lot, static_cast<u16>(f)));
    if (detail == Stage::Floors || b.stage == Stage::Floors) {
      // The facades as four walls a floor, and a band of glass along each one with daylight.
      const Rect& fp = b.footprint;
      const Rect sides[4] = {
          wall_rect(fp.x0, fp.z0, fp.x1, fp.z0, ext), wall_rect(fp.x1, fp.z0, fp.x1, fp.z1, ext),
          wall_rect(fp.x0, fp.z1, fp.x1, fp.z1, ext), wall_rect(fp.x0, fp.z0, fp.x0, fp.z1, ext)};
      for (u32 k = 0; k < 4; ++k) {
        out.push_back(box(plan_rect(b, sides[k]), fl.elevation_cm + k_slab_cm,
                          fl.height_cm - k_slab_cm, ProxyMesh::exterior_wall, lot,
                          static_cast<u16>(f)));
        if (b.exposed[k] == 0) continue;
        Rect glass = sides[k];
        const i32 inset = (k % 2 == 0 ? glass.width() : glass.depth()) / 10;
        if (k % 2 == 0) {
          glass.x0 += inset;
          glass.x1 -= inset;
          glass.z0 -= 2;
          glass.z1 += 2;
        } else {
          glass.z0 += inset;
          glass.z1 -= inset;
          glass.x0 -= 2;
          glass.x1 += 2;
        }
        const i32 sill = plan.params.rules.window_sill_cm;
        const i32 head = std::min(plan.params.rules.window_head_cm, fl.height_cm - k_slab_cm);
        if (head > sill)
          out.push_back(box(plan_rect(b, glass), fl.elevation_cm + k_slab_cm + sill, head - sill,
                            ProxyMesh::window, lot, static_cast<u16>(f)));
      }
    }
  }
  out.push_back(
      box(foot, b.height_cm(), k_slab_cm, ProxyMesh::slab, lot, static_cast<u16>(b.floors.size())));
  for (const Core& c : b.cores) {
    const i32 y = b.floors[c.floor_from].elevation_cm;
    const Floor& top = b.floors[c.floor_to];
    out.push_back(box(plan_rect(b, c.rect), y, top.elevation_cm + top.height_cm - y,
                      ProxyMesh::core, lot, c.floor_from));
  }
  if (detail != Stage::Rooms || b.stage != Stage::Rooms) return;
  for (u32 w = 0; w < b.walls.size(); ++w) {
    const Wall& wall = b.walls[w];
    const Floor& fl = b.floors[wall.floor];
    const bool exterior = wall.kind == WallKind::Exterior;
    out.push_back(
        box(plan_rect(b, wall_rect(wall.x0, wall.z0, wall.x1, wall.z1, wall.thickness_cm)),
            fl.elevation_cm + k_slab_cm, fl.height_cm - k_slab_cm,
            exterior ? ProxyMesh::exterior_wall : ProxyMesh::interior_wall, lot, wall.floor));
  }
  for (const Opening& o : b.openings) {
    if (o.kind != OpeningKind::Window) continue;
    const Wall& wall = b.walls[o.wall];
    const Floor& fl = b.floors[wall.floor];
    const i32 t = wall.thickness_cm + 4;
    Rect r;
    if (wall.z0 == wall.z1) {
      const i32 cx = wall.x0 + o.at_cm;
      r = wall_rect(cx - o.width_cm / 2, wall.z0, cx - o.width_cm / 2 + o.width_cm, wall.z0, t);
    } else {
      const i32 cz = wall.z0 + o.at_cm;
      r = wall_rect(wall.x0, cz - o.width_cm / 2, wall.x0, cz - o.width_cm / 2 + o.width_cm, t);
    }
    out.push_back(box(plan_rect(b, r), fl.elevation_cm + k_slab_cm + o.sill_cm,
                      o.head_cm - o.sill_cm, ProxyMesh::window, lot, wall.floor));
  }
}

void append_ground_proxies(const Plan& plan, const Rect& area, Vector<Proxy>& out) {
  for (const Segment& s : plan.segments)
    segment_proxies(plan, s, &area, out);
  for (u32 k = 0; k < plan.parks.size(); ++k)
    park_proxies(plan, k, &area, out);
}

void append_district_ground_proxies(const Plan& plan, u32 district, Vector<Proxy>& out) {
  // A segment is the district's when it lies on a side of one of the district's blocks.
  for (const Segment& s : plan.segments) {
    const Node& a = plan.nodes[s.a];
    const Node& b = plan.nodes[s.b];
    const i32 mx = static_cast<i32>((i64{a.x} + b.x) / 2);
    const i32 mz = static_cast<i32>((i64{a.z} + b.z) / 2);
    bool mine = false;
    for (const Block& blk : plan.blocks) {
      if (blk.district != district) continue;
      const Rect& r = blk.rect;
      const bool on_x = (mz == r.z0 || mz == r.z1) && mx >= r.x0 && mx <= r.x1;
      const bool on_z = (mx == r.x0 || mx == r.x1) && mz >= r.z0 && mz <= r.z1;
      const bool inside = mx > r.x0 && mx < r.x1 && mz > r.z0 && mz < r.z1;
      if (on_x || on_z || inside) {
        mine = true;
        break;
      }
    }
    if (mine) segment_proxies(plan, s, nullptr, out);
  }
  for (u32 k = 0; k < plan.parks.size(); ++k) {
    if (plan.parks[k].district == district) park_proxies(plan, k, nullptr, out);
  }
}

bool tile_proxies(const Plan& plan, TileCoord tile, Stage detail, Vector<Proxy>& out,
                  std::string* error) {
  out.clear();
  const Rect area = tile_rect(plan, tile);
  Vector<u32> lots;
  lots_in_tile(plan, tile, lots);
  Grammar grammar(plan);
  Building b;
  Vector<Proxy> scratch;
  for (const u32 l : lots) {
    if (plan.lots[l].use == LotUse::Park) continue;
    if (!grammar.generate(l, detail, b, error)) return false;
    scratch.clear();
    append_building_proxies(plan, b, detail, scratch);
    for (const Proxy& p : scratch) {
      if (proxy_in(p, area)) out.push_back(p);
    }
  }
  append_ground_proxies(plan, area, out);
  return true;
}

void sort_proxies(Vector<Proxy>& proxies) {
  std::sort(proxies.begin(), proxies.end(), [](const Proxy& a, const Proxy& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.z != b.z) return a.z < b.z;
    if (a.y != b.y) return a.y < b.y;
    if (a.sx != b.sx) return a.sx < b.sx;
    if (a.sz != b.sz) return a.sz < b.sz;
    if (a.sy != b.sy) return a.sy < b.sy;
    if (a.mesh != b.mesh) return a.mesh < b.mesh;
    if (a.lot != b.lot) return a.lot < b.lot;
    return a.floor < b.floor;
  });
}

u64 hash_proxies(const Vector<Proxy>& proxies) noexcept {
  u64 h = hash_combine(k_hash_seed, proxies.size());
  for (const Proxy& p : proxies) {
    h = hash_combine(h, u64{static_cast<u32>(p.x)} << 32 | static_cast<u32>(p.z));
    h = hash_combine(h, u64{static_cast<u32>(p.y)} << 32 | static_cast<u32>(p.sy));
    h = hash_combine(h, u64{static_cast<u32>(p.sx)} << 32 | static_cast<u32>(p.sz));
    h = hash_combine(h, u64{p.lot} << 32 | u64{static_cast<u8>(p.mesh)} << 16 | p.floor);
  }
  return h;
}

bool write_proxy_meshes(const std::string& dir, std::string* error) {
  if (io::make_directories(dir) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot create " + dir;
    return false;
  }
  for (u32 m = 0; m < k_proxy_meshes; ++m) {
    const Vector<u8> glb = box_glb(k_mesh_colours[m]);
    const std::string path = io::join_path(dir, std::string(k_mesh_names[m]) + ".glb");
    const io::Status status = io::write_file(
        path, std::string_view(reinterpret_cast<const char*>(glb.data()), glb.size()));
    if (status != io::Status::Ok) {
      if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
      return false;
    }
  }
  return true;
}

WorldPos proxy_position(const Proxy& p) noexcept {
  // Centimetres to metres in f64, one correctly rounded division a coordinate: the integer's place
  // to a nanometre at any distance an i32 of centimetres reaches (21,474 km).
  return WorldPos{static_cast<f64>(p.x) / 100.0, static_cast<f64>(p.y) / 100.0,
                  static_cast<f64>(p.z) / 100.0};
}

Vec3 proxy_scale(const Proxy& p) noexcept {
  return Vec3{static_cast<f32>(p.sx) * 0.01f, static_cast<f32>(p.sy) * 0.01f,
              static_cast<f32>(p.sz) * 0.01f};
}

std::string proxy_mesh_path(std::string_view dir, ProxyMesh mesh) {
  return io::join_path(dir, std::string(k_mesh_names[static_cast<u32>(mesh)]) + ".glb");
}

void make_fragment(const Vector<Proxy>& proxies, std::string_view mesh_dir,
                   std::string_view fragment_dir, std::string_view name, scene::Scene& scene) {
  scene = scene::Scene{};
  scene.format = "engine.scene.v1";
  scene.name = std::string(name);
  scene.description = "Island City proxies: " + std::to_string(proxies.size()) + " instances of " +
                      std::to_string(k_proxy_meshes) +
                      " box meshes, written by engine-content city fragment";
  for (u32 m = 0; m < k_proxy_meshes; ++m) {
    scene::Mesh mesh;
    mesh.name = k_mesh_names[m];
    mesh.path =
        mesh_path(io::join_path(mesh_dir, std::string(k_mesh_names[m]) + ".glb"), fragment_dir);
    scene.meshes.push_back(std::move(mesh));
  }
  scene.instances.reserve(proxies.size());
  for (const Proxy& p : proxies) {
    scene::Instance instance;
    instance.mesh = static_cast<u32>(p.mesh);
    if (p.lot != k_no_id)
      instance.name = "lot " + std::to_string(p.lot) + " floor " + std::to_string(p.floor);
    instance.translation = proxy_position(p);
    instance.scale = proxy_scale(p);
    scene.instances.push_back(std::move(instance));
  }
}

bool write_fragment(const std::string& path, const Vector<Proxy>& proxies,
                    std::string_view mesh_dir, std::string_view name, std::string* error) {
  scene::Scene scene;
  make_fragment(proxies, mesh_dir, io::parent_path(path), name, scene);
  // Compact: a district is tens of thousands of instances, and nobody diffs a generated fragment.
  const std::string text =
      write_json(schema::to_json(scene), JsonWriteOptions{.pretty = false}) + "\n";
  const std::string_view dir = io::parent_path(path);
  if (!dir.empty() && io::make_directories(dir) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot create " + std::string(dir);
    return false;
  }
  const io::Status status = io::write_file(path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
    return false;
  }
  return true;
}

}  // namespace engine::city
