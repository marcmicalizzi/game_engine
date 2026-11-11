// A footbridge as geometry, and the mesh generator `bridge` (docs/subsystems/bridges.md, "The
// mesh"): every member of the plan a closed, outward-wound convex solid — a stone a block with a
// rough outline, a sawn member a box, a log a twelve-sided prism hewn flat where it is walked on or
// bears planks — the stone and the timber each clustered as a part and the two DAGs merged into one
// mesh of two materials.

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/gltf.h>
#include <domain/bridges/bridges.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

#include <cmath>

namespace engine::bridges {
namespace {

f32 metres(i32 mm) noexcept { return static_cast<f32>(static_cast<f64>(mm) / 1000.0); }

Vec3 sub(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// A convex face's points, oriented so its normal points away from the solid's centre, appended as
// a fan. `smooth` (nullable) gives each point its own normal (a log's round side); otherwise the
// face's. UVs are metres of surface on the plane the normal is nearest to square to.
void face(MeshPart& part, Vec3 centre, const Vec3* pts, u32 n, const Vec3* smooth) {
  // Newell's normal: exact for a planar polygon whatever its collinear points.
  f64 nx = 0, ny = 0, nz = 0, cx = 0, cy = 0, cz = 0;
  for (u32 i = 0; i < n; ++i) {
    const Vec3 a = pts[i], b = pts[(i + 1) % n];
    nx += static_cast<f64>(a.y - b.y) * static_cast<f64>(a.z + b.z);
    ny += static_cast<f64>(a.z - b.z) * static_cast<f64>(a.x + b.x);
    nz += static_cast<f64>(a.x - b.x) * static_cast<f64>(a.y + b.y);
    cx += static_cast<f64>(a.x);
    cy += static_cast<f64>(a.y);
    cz += static_cast<f64>(a.z);
  }
  const f64 inv = 1.0 / static_cast<f64>(n);
  const f64 out = nx * (cx * inv - static_cast<f64>(centre.x)) +
                  ny * (cy * inv - static_cast<f64>(centre.y)) +
                  nz * (cz * inv - static_cast<f64>(centre.z));
  const bool flip = out < 0;
  const f64 len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (!(len > 0)) return;
  const f64 s = (flip ? -1.0 : 1.0) / len;
  const Vec3 normal{static_cast<f32>(nx * s), static_cast<f32>(ny * s), static_cast<f32>(nz * s)};
  const f32 ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
  const u32 base = part.positions.size();
  for (u32 k = 0; k < n; ++k) {
    const u32 i = flip ? n - 1 - k : k;
    const Vec3 p = pts[i];
    part.positions.push_back(p);
    part.normals.push_back(smooth != nullptr ? smooth[i] : normal);
    if (ay >= ax && ay >= az)
      part.uvs.push_back({p.x, p.z});
    else if (ax >= az)
      part.uvs.push_back({p.z, p.y});
    else
      part.uvs.push_back({p.x, p.y});
  }
  for (u32 k = 1; k + 1 < n; ++k) {
    part.indices.push_back(base);
    part.indices.push_back(base + k);
    part.indices.push_back(base + k + 1);
  }
}

// A prism: the convex outline `ring` (n points of the section) at the section plane's two ends.
// `at(a, b, t)` maps a section point and the axis coordinate to a position.
template <typename At>
void prism(MeshPart& part, const f32* sa, const f32* sb, const Vec3* ring_normals, u32 n, f32 t0,
           f32 t1, At at) {
  Vec3 lo[16], hi[16];
  f64 ca = 0, cb = 0;
  for (u32 i = 0; i < n; ++i) {
    lo[i] = at(sa[i], sb[i], t0);
    hi[i] = at(sa[i], sb[i], t1);
    ca += static_cast<f64>(sa[i]);
    cb += static_cast<f64>(sb[i]);
  }
  const Vec3 centre = at(static_cast<f32>(ca / n), static_cast<f32>(cb / n), (t0 + t1) * 0.5f);
  face(part, centre, lo, n, nullptr);
  face(part, centre, hi, n, nullptr);
  for (u32 i = 0; i < n; ++i) {
    const u32 j = (i + 1) % n;
    const Vec3 q[4] = {lo[i], lo[j], hi[j], hi[i]};
    if (ring_normals != nullptr) {
      const Vec3 qn[4] = {ring_normals[i], ring_normals[j], ring_normals[j], ring_normals[i]};
      face(part, centre, q, 4, qn);
    } else {
      face(part, centre, q, 4, nullptr);
    }
  }
}

// The twelve directions of a log's section, cos and sin of 0, 30, ... 330 degrees, as constants
// (never the C library's sine).
constexpr f64 k_c30 = 0.86602540378443864676;
constexpr f64 k_ring_cos[12] = {1, k_c30, 0.5, 0, -0.5, -k_c30, -1, -k_c30, -0.5, 0, 0.5, k_c30};
constexpr f64 k_ring_sin[12] = {0, 0.5, k_c30, 1, k_c30, 0.5, 0, -0.5, -k_c30, -1, -k_c30, -0.5};

void emit(MeshPart& part, const Member& m) {
  if (m.shape == Shape::log_x || m.shape == Shape::log_z) {
    // The section: a circle of the member's smaller cross extent about its middle, its top cut
    // flat at `hewn` (a log hewn where it is walked on or bears planks).
    const bool along_x = m.shape == Shape::log_x;
    const i32 w0 = along_x ? m.z0 : m.x0, w1 = along_x ? m.z1 : m.x1;
    const f64 r = 0.5 * static_cast<f64>(std::min(w1 - w0, m.y1 - m.y0)) / 1000.0;
    const f64 cw = 0.5 * static_cast<f64>(w0 + w1) / 1000.0;
    const f64 cy = 0.5 * static_cast<f64>(m.y0 + m.y1) / 1000.0;
    const f64 top = m.hewn > 0 ? static_cast<f64>(m.hewn) / 1000.0 : 1e30;
    f32 sa[12], sb[12];
    Vec3 rn[12];
    for (u32 i = 0; i < 12; ++i) {
      const f64 y = cy + r * k_ring_sin[i];
      const bool cut = y > top;
      sa[i] = static_cast<f32>(cw + r * k_ring_cos[i]);
      sb[i] = static_cast<f32>(cut ? top : y);
      const f32 nc = static_cast<f32>(k_ring_cos[i]), ns = static_cast<f32>(k_ring_sin[i]);
      rn[i] = cut ? Vec3{0, 1, 0} : (along_x ? Vec3{0, ns, nc} : Vec3{nc, ns, 0});
    }
    if (along_x)
      prism(part, sa, sb, rn, 12, metres(m.x0), metres(m.x1),
            [](f32 a, f32 b, f32 t) { return Vec3{t, b, a}; });
    else
      prism(part, sa, sb, rn, 12, metres(m.z0), metres(m.z1),
            [](f32 a, f32 b, f32 t) { return Vec3{a, b, t}; });
    return;
  }
  // A block: a sawn box, or a stone whose plan is the box with each corner knocked off by its own
  // draw (an eight-sided convex outline), its bed and top flat (a slab split along its bedding).
  f32 sa[8], sb[8];
  u32 n = 4;
  if (m.shape == Shape::box) {
    sa[0] = metres(m.x0), sb[0] = metres(m.z0);
    sa[1] = metres(m.x1), sb[1] = metres(m.z0);
    sa[2] = metres(m.x1), sb[2] = metres(m.z1);
    sa[3] = metres(m.x0), sb[3] = metres(m.z1);
  } else {
    n = 8;
    const i32 side = std::min(m.x1 - m.x0, m.z1 - m.z0);
    i32 c[4];
    u64 s = m.seed;
    for (i32& k : c) {
      s = mix64(s + 0x9E3779B97F4A7C15ull);
      k = side * (8 + static_cast<i32>(s % 18)) / 100;  // 8 to 25 % of the shorter side
    }
    const i32 px[8] = {m.x0 + c[0], m.x1 - c[1], m.x1, m.x1, m.x1 - c[2], m.x0 + c[3], m.x0, m.x0};
    const i32 pz[8] = {m.z0, m.z0, m.z0 + c[1], m.z1 - c[2], m.z1, m.z1, m.z1 - c[3], m.z0 + c[0]};
    for (u32 i = 0; i < 8; ++i) {
      sa[i] = metres(px[i]);
      sb[i] = metres(pz[i]);
    }
  }
  prism(part, sa, sb, nullptr, n, metres(m.y0), metres(m.y1),
        [](f32 a, f32 b, f32 t) { return Vec3{a, t, b}; });
}

const char* form_name(BridgeForm f) noexcept {
  switch (f) {
    case BridgeForm::stones: return "stepping stones";
    case BridgeForm::clapper: return "clapper bridge";
    case BridgeForm::log: return "foot log";
    case BridgeForm::stringers: return "log stringers";
    case BridgeForm::none: break;
  }
  return "ford";
}

}  // namespace

bool solve_bridge(const Bridge& b, MeshPart (&parts)[2], BridgePlan& plan, BridgeReport& report,
                  std::string& error) {
  parts[0] = {};
  parts[1] = {};
  report = {};
  if (!plan_bridge(b.crossing, b.seed, plan, error)) return false;
  for (const Member& m : plan.members) {
    MeshPart& part = parts[m.material == k_timber ? 1 : 0];
    const u32 before = part.indices.size() / 3;
    emit(part, m);
    const u32 made = part.indices.size() / 3 - before;
    report.kind_triangles[static_cast<u32>(m.kind)] += made;
    report.triangles += made;
  }
  report.stone_triangles = parts[0].indices.size() / 3;
  report.timber_triangles = parts[1].indices.size() / 3;
  return true;
}

bool read_file(std::string_view text, Bridge& out, std::string& error) {
  JsonValue value;
  const auto parsed = parse_json(text, value);
  if (!parsed.ok) {
    error = "bridge recipe: " + std::string(parsed.message);
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx) || !ctx.ok()) {
    error = ctx.diagnostics.empty()
                ? std::string("bridge recipe: invalid engine.bridges.Bridge")
                : "bridge recipe: " + ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message;
    return false;
  }
  std::string why;
  if (!crossing_in_range(out.crossing, why)) {
    error = "bridge recipe: crossing: " + why;
    return false;
  }
  return true;
}

std::string canonical_json(const Bridge& bridge) {
  return write_json(schema::to_json(bridge), {.pretty = false});
}

u64 source_hash(const Bridge& bridge) {
  const std::string text = canonical_json(bridge);
  return hash_combine(hash_bytes(text.data(), text.size()), k_bridge_version);
}

bool build_container(const Bridge& bridge, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, BridgeReport& report, std::string& error) {
  out = {};
  MeshPart parts[2];
  BridgePlan plan;
  if (!solve_bridge(bridge, parts, plan, report, error)) return false;
  if (plan.members.empty()) {
    error = "the crossing is left a ford: there is no bridge to build";
    return false;
  }
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;  // every member is closed and one-sided
  Vector<geometry::ClusterLodMesh> built;
  Vector<u32> material_of;
  for (u32 m = 0; m < 2; ++m) {
    if (parts[m].indices.empty()) continue;
    geometry::ClusterLodMesh one;
    std::string build_error;
    geometry::AttributeSource attributes;
    attributes.normals = {parts[m].normals.data(), parts[m].normals.size()};
    attributes.uvs = {parts[m].uvs.data(), parts[m].uvs.size()};
    if (!geometry::build_cluster_lod({parts[m].positions.data(), parts[m].positions.size()},
                                     {parts[m].indices.data(), parts[m].indices.size()}, options,
                                     one, &build_error, attributes)) {
      error = "the cluster build refused the bridge: " + build_error;
      return false;
    }
    built.push_back(std::move(one));
    material_of.push_back(m);
  }
  Vector<u32> part_of_cluster;
  std::string merge_error;
  if (!geometry::merge_cluster_lod({built.data(), built.size()}, out.mesh, &part_of_cluster,
                                   &merge_error)) {
    error = "the bridge's parts did not merge: " + merge_error;
    return false;
  }
  if (page_bytes > 0) {
    Vector<u32> source_of_cluster;
    geometry::ClusterPagesOptions paging;
    paging.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, paging, out.pages, &merge_error,
                                       &source_of_cluster)) {
      error = "the bridge's pages were not laid out: " + merge_error;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }
  out.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster)
    out.cluster_material.push_back(material_of[part]);
  // The two materials, a constant each where the pack has no set for the layer's role: a grey
  // stone, a weathered timber.
  const Vec4 colour[2] = {{0.46f, 0.45f, 0.42f, 1.0f}, {0.40f, 0.35f, 0.29f, 1.0f}};
  const f32 roughness[2] = {0.85f, 0.80f};
  const std::string_view role[2] = {bridge.stone_role, bridge.wood_role};
  for (u32 m = 0; m < 2; ++m) {
    geometry::ClusterFileMaterial material;
    material.base_color = colour[m];
    material.metallic = 0;
    material.roughness = roughness[m];
    material.alpha = geometry::encode_alpha_word(assets::k_alpha_opaque, false, 0.5f);
    out.materials.push_back(material);
    const assets::Material defaults;
    out.material_sampling.push_back(geometry::encode_material_sampling(defaults.sampling, 1.0f));
  }
  for (u32 m = 0; m < 2; ++m) {
    if (role[m].empty()) continue;
    const materials::MaterialSet* set = materials::resolve_set(nullptr, inputs.materials, role[m]);
    if (set != nullptr) materials::apply_set(*set, materials::UvLayout::metres, out, m);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  for (u32 c = 0; c < out.mesh.lod.size(); ++c)
    if (out.mesh.lod[c].parent_error == geometry::k_lod_terminal_error)
      report.root_triangles += out.mesh.mesh.clusters[c].triangle_count;
  out.source_hash = source_hash(bridge);
  out.build_key = assets::generated_mesh_build_key(out.source_hash, inputs, lod, page_bytes);
  out.generator = "bridge";
  out.generator_kind = form_name(plan.form);
  out.generator_params = canonical_json(bridge);
  return true;
}

namespace {
std::string_view text_of(std::span<const u8> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool hash_recipe(std::span<const u8> bytes, u64& out, std::string& error) {
  Bridge bridge;
  if (!read_file(text_of(bytes), bridge, error)) return false;
  out = source_hash(bridge);
  return true;
}
bool build_recipe(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                  const geometry::ClusterLodOptions& lod, u32 page_bytes,
                  geometry::ClusterFileData& out, std::string& error) {
  Bridge bridge;
  BridgeReport report;
  return read_file(text_of(bytes), bridge, error) &&
         build_container(bridge, inputs, lod, page_bytes, out, report, error);
}
constexpr assets::MeshGeneratorDesc k_mesh_generator{
    .name = "bridge", .source_hash = hash_recipe, .build = build_recipe};
const assets::MeshGeneratorRegistrar k_mesh_registrar{k_mesh_generator};
}  // namespace

}  // namespace engine::bridges
