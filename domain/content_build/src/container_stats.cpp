// The content-build metrics of a container (docs/subsystems/content_build.md). Moved here from
// apps/engine_content/main.cpp's `stats` unchanged but for where the words go: the JSON object is
// returned rather than printed, and the tables a person reads are appended to a string, in the
// order engine-content prints them on stderr, rather than printed as they are computed.
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <domain/content_build/container_stats.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace engine::content_build {
// Reads a container and hands back its bytes as well, so a caller can walk the section table
// straight from the file rather than from what this build understands of it.
bool load_container(const std::string& path, std::string& file, geometry::ClusterFileHeader& header,
                    Vector<geometry::ClusterFileSection>& sections, geometry::ClusterFileData& data,
                    std::string& error) {
  const io::Status status = io::read_file(path, file);
  if (status != io::Status::Ok) {
    error = "cannot read '" + path + "': " + io::status_name(status);
    return false;
  }
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(file.data()), file.size());
  if (!geometry::read_cluster_file_memory(bytes, data, &error)) return false;
  std::memcpy(&header, file.data(), sizeof(header));
  sections.resize(header.section_count);
  for (u32 i = 0; i < header.section_count; ++i) {
    std::memcpy(&sections[i], file.data() + sizeof(header) + sizeof(sections[i]) * i,
                sizeof(sections[i]));
  }
  return true;
}

// The page table at a glance, for `info` and as the head of `stats`'s page section: how many
// pages, how full they are, how much of the mesh is pinned (the pages holding a group with no
// coarser version, which the residency manager never evicts), and how connected they are.
JsonValue page_summary(const geometry::ClusterPages& pages) {
  JsonValue out = JsonValue::object();
  out.set("count", JsonValue(pages.pages.size()));
  out.set("bytes_target", JsonValue(pages.page_bytes_target));
  u64 total = 0;
  u32 largest = 0;
  u32 oversized = 0;
  u32 root = 0;
  for (const geometry::ClusterPageDesc& page : pages.pages) {
    total += page.bytes;
    largest = page.bytes > largest ? page.bytes : largest;
    if ((page.flags & geometry::k_page_oversized) != 0) ++oversized;
    if ((page.flags & geometry::k_page_root) != 0) ++root;
  }
  out.set("bytes", JsonValue(total));
  out.set("largest_bytes", JsonValue(largest));
  const f64 capacity =
      static_cast<f64>(pages.pages.size()) * static_cast<f64>(pages.page_bytes_target);
  out.set("mean_fill", JsonValue(capacity > 0.0 ? static_cast<f64>(total) / capacity : 0.0));
  out.set("oversized", JsonValue(oversized));
  out.set("root_pages", JsonValue(root));
  out.set("child_page_entries", JsonValue(pages.child_pages.size()));
  return out;
}

namespace {

// printf into a string: the tables below are report output, formatted exactly as engine-content
// has always printed them, so the text a person reads did not change when it stopped going
// straight to stderr.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (written <= 0) return;
  if (static_cast<usize>(written) < sizeof(buffer)) {
    out.append(buffer, static_cast<usize>(written));
    return;
  }
  // Longer than the stack buffer: format again into the string itself.
  const usize at = out.size();
  out.resize(at + static_cast<usize>(written) + 1);
  va_start(args, format);
  std::vsnprintf(out.data() + at, static_cast<usize>(written) + 1, format, args);
  va_end(args);
  out.resize(at + static_cast<usize>(written));
}
// ---- the streaming sweep ----------------------------------------------------------------------
//
// What the page layout costs a viewer that has to fetch it: a camera flies in from 50 mesh radii
// to half a radius, and at every step the sweep reports what the ideal cut needs and what a
// budgeted residency manager (docs/plan/04-renderer.md §4.9) actually asks for. It is a content
// metric, not a benchmark: it answers "does the way this mesh is paged make a fly-in cheap?"
// without a GPU, a window, or a frame loop.

constexpr u32 k_sweep_steps = 32;
constexpr f32 k_sweep_far = 50.0f;    // in mesh radii
constexpr f32 k_sweep_near = 0.5f;    // inside the mesh's own sphere: full detail
constexpr f64 k_sweep_budget = 0.25;  // of the mesh's total page bytes

struct SweepStep {
  f32 distance = 0.0f;  // in mesh radii
  u32 clusters = 0;     // the cut with everything resident
  u32 triangles = 0;
  u32 pages_needed = 0;  // distinct pages that cut draws from
  u32 requested = 0;     // pages this step asked for, under the budget
  u32 drawn = 0;         // clusters the budgeted viewer actually drew
  u32 evicted = 0;       // pages the budget took back this step
  u32 resident = 0;      // pages held at the end of the step
  u64 resident_bytes = 0;
};

// The bounding sphere of the mesh's original geometry, which is what "radii" measures.
void leaf_bounds(const geometry::ClusterLodMesh& lod, Vec3& center, f32& radius) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (u32 i = 0; i < lod.mesh.clusters.size(); ++i) {
    if (lod.lod[i].level != 0) continue;
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
              std::min(lo.z, c.center.z - c.radius)};
    hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
              std::max(hi.z, c.center.z + c.radius)};
  }
  center = (lo + hi) * 0.5f;
  radius = 1e-6f;
  for (u32 i = 0; i < lod.mesh.clusters.size(); ++i) {
    if (lod.lod[i].level != 0) continue;
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    radius = std::max(radius, length(c.center - center) + c.radius);
  }
}

bool sweep_streaming(const geometry::ClusterFileData& data, Vector<SweepStep>& out, u64& budget,
                     std::string& error) {
  out.clear();
  budget = 0;
  const geometry::ClusterPages& pages = data.pages;
  if (pages.pages.empty()) return true;
  u64 total = 0;
  for (const geometry::ClusterPageDesc& page : pages.pages)
    total += page.bytes;
  budget = static_cast<u64>(static_cast<f64>(total) * k_sweep_budget);

  Vec3 center{};
  f32 radius = 1.0f;
  leaf_bounds(data.mesh, center, radius);

  geometry::PageResidency everything;
  everything.resident.assign(pages.pages.size(), u8{1});
  geometry::PageResidencyManager manager;
  if (!manager.reset(pages, budget, &error)) return false;

  geometry::LodView view;
  view.znear = 0.1f;
  // 1080p at 60 degrees, one pixel of error: the same view the LOD tests use.
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;

  Vector<u32> cut;
  Vector<u32> budgeted;
  Vector<u32> requests;
  Vector<u8> seen(pages.pages.size(), u8{0});
  for (u32 step = 0; step < k_sweep_steps; ++step) {
    const f32 t = static_cast<f32>(step) / static_cast<f32>(k_sweep_steps - 1);
    SweepStep entry;
    entry.distance = k_sweep_far + (k_sweep_near - k_sweep_far) * t;
    view.camera = center + Vec3{0.0f, 0.0f, entry.distance * radius};

    // What the mesh would draw with every page in memory, and how many pages that touches.
    cut.clear();
    requests.clear();
    entry.clusters =
        geometry::select_lod_streaming(data.mesh, view, pages, everything, cut, requests);
    for (u8& flag : seen)
      flag = 0;
    for (const u32 c : cut) {
      entry.triangles += data.mesh.mesh.clusters[c].triangle_count;
      u8& flag = seen[pages.page_of_cluster[c]];
      if (flag == 0) ++entry.pages_needed;
      flag = 1;
    }

    // And what a viewer under a quarter of the bytes asks for, one frame per step.
    manager.begin_frame();
    budgeted.clear();
    requests.clear();
    entry.drawn = geometry::select_lod_streaming(data.mesh, view, pages, manager.page_residency(),
                                                 budgeted, requests);
    for (const u32 c : budgeted)
      manager.touch(pages.page_of_cluster[c]);
    entry.requested = requests.size();
    manager.request(std::span<const u32>(requests.data(), requests.size()));
    manager.admit(~u32{0});
    entry.evicted = manager.evict_to_budget();
    entry.resident = manager.resident_pages();
    entry.resident_bytes = manager.resident_bytes();
    out.push_back(entry);
  }
  return true;
}

// The sweep as a table for a person; engine-content puts it on stderr so stdout stays one JSON
// line.
void append_sweep(std::string& out, const Vector<SweepStep>& sweep, u64 budget) {
  if (sweep.empty()) return;
  appendf(out, "  streaming sweep, 1080p at 1 px, budget %llu bytes (25%%)\n",
          static_cast<unsigned long long>(budget));
  appendf(out, "  %6s %9s %10s %7s %10s %8s %8s %9s %9s\n", "radii", "clusters", "triangles",
          "pages", "requested", "drawn", "evicted", "resident", "KB");
  for (const SweepStep& step : sweep) {
    appendf(out, "  %6.2f %9u %10u %7u %10u %8u %8u %9u %9llu\n",
            static_cast<double>(step.distance), step.clusters, step.triangles, step.pages_needed,
            step.requested, step.drawn, step.evicted, step.resident,
            static_cast<unsigned long long>(step.resident_bytes / 1024));
  }
}

// ---- the atlas, and what LOD can do with it ----------------------------------------------------
//
// How fragmented a UV atlas is decides how far a mesh can be simplified at all, because the LOD
// builder is forbidden to collapse across an island edge (docs/subsystems/geometry.md, "What the
// simplifier is given, and why"). Before that rule existed the question did not arise and the
// builder simply produced the wrong picture; now it is a **cost** the asset controls, which makes
// it something a content build has to report. These are the numbers §7.4's atlas-fragmentation
// validator will threshold.
//
// An island is a connected component of the level-0 triangles over *atlas points* — source
// vertices joined by index and, where the weld kept two apart for their normals alone, by sharing
// a position and a stored UV — because the two sides of an island edge differ in UV, and a hard
// edge inside an island does not (the reasoning is at the join below). Areas are in UV space,
// reported in texels of a 4096 atlas because that is the unit an author reasons in and the unit
// "smaller than a triangle" is decided in.
struct AtlasStats {
  u32 islands = 0;
  u32 source_vertices = 0;    // distinct source vertices the clusters reference
  u32 seam_vertices = 0;      // of those, ones sharing a position with a different-UV vertex
  f64 uv_area = 0.0;          // total, in UV units (1.0 is the whole atlas once)
  f64 smallest_texels = 0.0;  // the smallest island's area, in texels of a 4096 atlas
  f64 median_texels = 0.0;
  f64 largest_texels = 0.0;
  u32 smallest_triangles = 0;
  bool valid = false;
};

u32 find_root(Vector<u32>& parent, u32 v) {
  while (parent[v] != v) {
    parent[v] = parent[parent[v]];  // path halving; the union-find is over source vertices
    v = parent[v];
  }
  return v;
}

AtlasStats measure_atlas(const geometry::ClusterLodMesh& lod) {
  AtlasStats out;
  const geometry::ClusterMesh& mesh = lod.mesh;
  if (mesh.attributes.size() != mesh.vertices.size() || mesh.vertex_source.empty()) return out;
  u32 source_count = 0;
  for (const u32 source : mesh.vertex_source)
    source_count = source >= source_count ? source + 1 : source_count;
  if (source_count == 0) return out;

  // One position and one UV per source vertex, taken from any cluster that carries it: every
  // cluster's copy of a source vertex holds the same bytes, which is the invariant the weld and
  // `fill_cluster_attributes` maintain and `validate_clusters` checks.
  Vector<Vec3> position(source_count, Vec3{});
  Vector<Vec2> uv(source_count, Vec2{});
  Vector<u8> present(source_count, u8{0});
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const u32 source = mesh.vertex_source[v];
    position[source] = mesh.vertices[v];
    uv[source] = geometry::decode_half2(mesh.attributes[v].uv_half2);
    present[source] = 1u;
  }
  for (const u8 seen : present)
    out.source_vertices += seen;

  // Islands: union the three corners of every level-0 triangle.
  Vector<u32> parent(source_count);
  for (u32 i = 0; i < source_count; ++i)
    parent[i] = i;
  Vector<f64> area(source_count, 0.0);  // accumulated on the component's root at the end
  Vector<u32> triangles(source_count, 0u);
  Vector<u32> corners;
  corners.reserve(mesh.triangles.size() * 3);
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    if (lod.lod[c].level != 0) continue;
    const geometry::ClusterDesc& desc = mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u32 packed = mesh.triangles[desc.triangle_offset + t];
      for (u32 k = 0; k < 3; ++k)
        corners.push_back(
            mesh.vertex_source[desc.vertex_offset + geometry::ClusterMesh::unpack(packed, k)]);
    }
  }
  // A UV island is connected **in the atlas**: two source vertices at one position with one UV
  // (as stored) are one point of it even when the weld kept them apart for a different normal.
  // So every source vertex is first joined to the others that share its position and stored UV.
  // Without this, a hard edge inside an island — which a remesher writes along every chart border
  // it cuts (E10's second pass) — split the island by index, and `smallest_island_texels_4096`
  // reported a sliver of a sound island as an island of its own. Texture sampling and the LOD
  // builder's seam rule (`uv_seams`, with `normal_seams` off) both see the atlas the same way.
  {
    struct AtlasKey {
      u32 bits[5] = {};
      u32 vertex = 0;
    };
    Vector<AtlasKey> atlas_keys;
    atlas_keys.reserve(out.source_vertices);
    for (u32 v = 0; v < source_count; ++v) {
      if (present[v] == 0) continue;
      AtlasKey k;
      std::memcpy(&k.bits[0], &position[v], sizeof(Vec3));
      std::memcpy(&k.bits[3], &uv[v], sizeof(Vec2));
      k.vertex = v;
      atlas_keys.push_back(k);
    }
    std::sort(atlas_keys.begin(), atlas_keys.end(), [](const AtlasKey& a, const AtlasKey& b) {
      for (u32 i = 0; i < 5; ++i) {
        if (a.bits[i] != b.bits[i]) return a.bits[i] < b.bits[i];
      }
      return a.vertex < b.vertex;
    });
    for (u32 i = 1; i < atlas_keys.size(); ++i) {
      if (std::memcmp(atlas_keys[i].bits, atlas_keys[i - 1].bits, sizeof(atlas_keys[i].bits)) != 0)
        continue;
      const u32 a = find_root(parent, atlas_keys[i - 1].vertex);
      const u32 b = find_root(parent, atlas_keys[i].vertex);
      if (a != b) parent[b] = a;
    }
  }
  for (u32 i = 0; i + 2 < corners.size(); i += 3) {
    const u32 a = find_root(parent, corners[i]);
    const u32 b = find_root(parent, corners[i + 1]);
    const u32 c = find_root(parent, corners[i + 2]);
    if (a != b) parent[b] = a;
    if (a != c) parent[find_root(parent, c)] = a;
  }
  for (u32 i = 0; i + 2 < corners.size(); i += 3) {
    const u32 root = find_root(parent, corners[i]);
    const Vec2 e0 = uv[corners[i + 1]] - uv[corners[i]];
    const Vec2 e1 = uv[corners[i + 2]] - uv[corners[i]];
    const f64 triangle_area = std::fabs(static_cast<f64>(e0.x * e1.y - e0.y * e1.x)) * 0.5;
    area[root] += triangle_area;
    ++triangles[root];
    out.uv_area += triangle_area;
  }

  // Seam vertices: a source vertex that shares a position with another source vertex that
  // disagrees about the UV. Sorted by a position key rather than hashed, so the answer is a
  // function of the container alone.
  struct PositionKey {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    u32 vertex = 0;
  };
  Vector<PositionKey> keys;
  keys.reserve(out.source_vertices);
  for (u32 v = 0; v < source_count; ++v) {
    if (present[v] == 0) continue;
    keys.push_back(PositionKey{position[v].x, position[v].y, position[v].z, v});
  }
  std::sort(keys.begin(), keys.end(), [](const PositionKey& a, const PositionKey& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.vertex < b.vertex;
  });
  for (u32 i = 0; i < keys.size();) {
    u32 j = i;
    while (j < keys.size() && keys[j].x == keys[i].x && keys[j].y == keys[i].y &&
           keys[j].z == keys[i].z)
      ++j;
    if (j - i > 1) {
      for (u32 k = i; k < j; ++k) {
        bool differs = false;
        for (u32 m = i; m < j; ++m) {
          if (m == k) continue;
          if (uv[keys[m].vertex].x != uv[keys[k].vertex].x ||
              uv[keys[m].vertex].y != uv[keys[k].vertex].y) {
            differs = true;
          }
        }
        if (differs) ++out.seam_vertices;
      }
    }
    i = j;
  }

  Vector<f64> island_areas;
  f64 smallest_area = 0.0;
  for (u32 v = 0; v < source_count; ++v) {
    if (triangles[v] == 0) continue;
    if (out.islands == 0 || area[v] < smallest_area) {
      smallest_area = area[v];
      out.smallest_triangles = triangles[v];
    }
    ++out.islands;
    island_areas.push_back(area[v]);
    const f64 texels = area[v] * 4096.0 * 4096.0;
    if (texels > out.largest_texels) out.largest_texels = texels;
  }
  out.smallest_texels = smallest_area * 4096.0 * 4096.0;
  if (!island_areas.empty()) {
    std::sort(island_areas.begin(), island_areas.end());
    out.median_texels = island_areas[island_areas.size() / 2] * 4096.0 * 4096.0;
  }
  out.valid = out.islands > 0;
  return out;
}

// The canonical vertex ids over the source vertices the clusters reference (docs/subsystems/
// geometry.md, "Canonical vertex identity"). A source vertex is a distinct `vertex_source` entry,
// as `referenced_source_vertices` counts them — so a vertex two primitives share counts once per
// primitive, which is how the builders see it. What an author or an atlas tool wants to know:
//
//   distinct_ids      how many points of the base the mesh names;
//   sharing           source vertices whose id another source vertex also carries — the seam
//                     duplicates (a UV seam, a hard edge, a skin or morph split) when the ids are
//                     derived, since those are exactly the vertices a position weld folds together;
//   duplicates        source vertices minus distinct ids, among those that have one: what the
//                     seams cost in vertices;
//   largest_share     the most source vertices any one id has;
//   without_id        vertices of a primitive that carried no authored id (`k_no_vertex_id`);
//   ids_at_several_positions  ids whose vertices do not all sit at one position. Always zero for
//                     derived ids; for authored ones it is the thing to look at first, because a
//                     point of the base is one point.
struct IdentityStats {
  u32 source_vertices = 0;
  u32 distinct_ids = 0;
  u32 sharing = 0;
  u32 duplicates = 0;
  u32 largest_share = 0;
  u32 without_id = 0;
  u32 ids_at_several_positions = 0;
  bool valid = false;
};

IdentityStats measure_identity(const geometry::ClusterMesh& mesh) {
  IdentityStats out;
  if (mesh.vertex_ids.empty() || mesh.vertex_ids.size() != mesh.vertices.size() ||
      mesh.vertex_source.size() != mesh.vertices.size()) {
    return out;
  }
  // One (id, position, source) per source vertex, sorted by id then position, so every question
  // above is a walk over runs — and the answer is a function of the container, not of a hash order.
  struct Entry {
    u32 id = 0;
    u32 position[3] = {};
    u32 source = 0;
  };
  u32 source_count = 0;
  for (const u32 source : mesh.vertex_source)
    source_count = source >= source_count ? source + 1 : source_count;
  Vector<u8> seen(source_count, u8{0});
  Vector<Entry> entries;
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const u32 source = mesh.vertex_source[v];
    if (seen[source] != 0) continue;
    seen[source] = 1;
    Entry entry;
    entry.id = mesh.vertex_ids[v];
    std::memcpy(entry.position, &mesh.vertices[v], sizeof(entry.position));
    entry.source = source;
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.id != b.id) return a.id < b.id;
    const int order = std::memcmp(a.position, b.position, sizeof(a.position));
    if (order != 0) return order < 0;
    return a.source < b.source;
  });
  out.source_vertices = entries.size();
  for (u32 i = 0; i < entries.size();) {
    u32 j = i;
    bool one_position = true;
    while (j < entries.size() && entries[j].id == entries[i].id) {
      if (std::memcmp(entries[j].position, entries[i].position, sizeof(entries[i].position)) != 0)
        one_position = false;
      ++j;
    }
    const u32 run = j - i;
    if (entries[i].id == geometry::k_no_vertex_id) {
      out.without_id = run;
    } else {
      ++out.distinct_ids;
      if (run > 1) out.sharing += run;
      out.largest_share = run > out.largest_share ? run : out.largest_share;
      if (!one_position) ++out.ids_at_several_positions;
    }
    i = j;
  }
  out.duplicates = out.source_vertices - out.without_id - out.distinct_ids;
  out.valid = true;
  return out;
}

// What a coarse cut does to the texture, on the CPU (`geometry::measure_lod_attribute_error`).
// Two budgets, because one number cannot say whether the damage grows with the coarseness.
struct AttributeErrorRow {
  f64 fraction = 0.0;  // of the leaf triangles
  u32 clusters = 0;
  u32 triangles = 0;
  geometry::AttributeError error;
};

Vector<AttributeErrorRow> measure_attribute_error(const geometry::ClusterLodMesh& lod) {
  Vector<AttributeErrorRow> rows;
  if (lod.mesh.attributes.size() != lod.mesh.vertices.size() || lod.leaf_triangle_count == 0) {
    return rows;
  }
  for (const f64 fraction : {0.25, 0.10}) {
    const u32 target = static_cast<u32>(static_cast<f64>(lod.leaf_triangle_count) * fraction);
    // The cut nearest the budget: the raw cut's triangle count is monotone in the threshold, so a
    // geometric sweep is enough and needs no bracketing.
    f32 best_threshold = 0.0f;
    u32 best_distance = ~u32{0};
    for (u32 step = 0; step <= 64; ++step) {
      const f32 threshold = std::pow(10.0f, -6.0f + 8.0f * static_cast<f32>(step) / 64.0f);
      Vector<u32> cut;
      geometry::select_lod_raw(lod, threshold, cut);
      u32 triangles = 0;
      for (const u32 c : cut)
        triangles += lod.mesh.clusters[c].triangle_count;
      const u32 distance = triangles > target ? triangles - target : target - triangles;
      if (distance < best_distance) {
        best_distance = distance;
        best_threshold = threshold;
      }
    }
    Vector<u32> cut;
    geometry::select_lod_raw(lod, best_threshold, cut);
    if (cut.empty()) continue;
    AttributeErrorRow row;
    row.fraction = fraction;
    row.clusters = cut.size();
    for (const u32 c : cut)
      row.triangles += lod.mesh.clusters[c].triangle_count;
    std::string error;
    if (!geometry::measure_lod_attribute_error(lod, cut, geometry::AttributeErrorOptions{},
                                               row.error, &error)) {
      continue;
    }
    rows.push_back(row);
  }
  return rows;
}

}  // namespace
// The metrics docs/plan/07-content-pipeline.md §7.3 wants a content build to report about what it
// produced: how the clusters are spread over the levels, how full they are, how much the cluster
// layout duplicates the source vertices, where the bytes went, how coarse the position grid is,
// how fragmented the UV atlas is, and how much of the texture a coarse LOD cut moves. One JSON
// line, so a script can watch them move between builds.
bool container_stats(const std::string& path, JsonValue& out_summary, std::string& human,
                     std::string& out_error) {
  std::string file;
  geometry::ClusterFileHeader header;
  Vector<geometry::ClusterFileSection> records;
  geometry::ClusterFileData data;
  std::string error;
  if (!load_container(path, file, header, records, data, error)) {
    out_error = std::move(error);
    return false;
  }

  const geometry::ClusterMesh& mesh = data.mesh.mesh;
  const u32 cluster_count = mesh.clusters.size();

  // Triangles per cluster: the three order statistics and the whole distribution, one entry per
  // distinct count, whose cluster counts add up to the clusters in the file.
  Vector<u32> per_cluster;
  per_cluster.reserve(cluster_count);
  for (const geometry::ClusterDesc& cluster : mesh.clusters)
    per_cluster.push_back(cluster.triangle_count);
  std::sort(per_cluster.begin(), per_cluster.end());
  JsonValue histogram = JsonValue::array();
  for (u32 i = 0; i < per_cluster.size();) {
    u32 j = i;
    while (j < per_cluster.size() && per_cluster[j] == per_cluster[i])
      ++j;
    JsonValue bucket = JsonValue::object();
    bucket.set("triangles", JsonValue(per_cluster[i]));
    bucket.set("clusters", JsonValue(j - i));
    histogram.push_back(std::move(bucket));
    i = j;
  }
  JsonValue triangles_per_cluster = JsonValue::object();
  triangles_per_cluster.set("min", JsonValue(per_cluster.empty() ? 0u : per_cluster[0]));
  // The upper middle for an even count, so the median is always a count some cluster has.
  triangles_per_cluster.set(
      "median", JsonValue(per_cluster.empty() ? 0u : per_cluster[per_cluster.size() / 2]));
  triangles_per_cluster.set(
      "max", JsonValue(per_cluster.empty() ? 0u : per_cluster[per_cluster.size() - 1]));
  triangles_per_cluster.set("total", JsonValue(mesh.triangles.size()));
  triangles_per_cluster.set("histogram", std::move(histogram));

  JsonValue levels = JsonValue::array();
  for (const u32 count : data.mesh.level_cluster_counts)
    levels.push_back(JsonValue(count));

  // Where the bytes went. The table is walked from the file, so a section this build does not
  // know is still accounted for, and the header and the table itself are named too.
  JsonValue sections = JsonValue::array();
  u64 payload_bytes = 0;
  for (const geometry::ClusterFileSection& section : records) {
    const u64 bytes = u64{section.element_size} * section.element_count;
    payload_bytes += bytes;
    JsonValue entry = JsonValue::object();
    entry.set("kind", JsonValue(section.kind));
    entry.set("name", JsonValue(geometry::cluster_section_name(section.kind)));
    entry.set("bytes", JsonValue(bytes));
    sections.push_back(std::move(entry));
  }
  const u64 header_bytes = static_cast<u64>(sizeof(header));
  const u64 table_bytes =
      static_cast<u64>(sizeof(geometry::ClusterFileSection)) * header.section_count;
  JsonValue bytes = JsonValue::object();
  bytes.set("total", JsonValue(header.total_bytes));
  bytes.set("header", JsonValue(header_bytes));
  bytes.set("section_table", JsonValue(table_bytes));
  bytes.set("payloads", JsonValue(payload_bytes));
  // What alignment cost: every payload starts on a 16-byte boundary.
  bytes.set("padding", JsonValue(header.total_bytes - header_bytes - table_bytes - payload_bytes));
  bytes.set("sections", std::move(sections));

  JsonValue origin = JsonValue::array();
  origin.push_back(JsonValue(mesh.quant_origin.x));
  origin.push_back(JsonValue(mesh.quant_origin.y));
  origin.push_back(JsonValue(mesh.quant_origin.z));
  JsonValue quantization = JsonValue::object();
  quantization.set("step", JsonValue(mesh.quant_scale));
  quantization.set("origin", std::move(origin));

  // Vertex duplication: the cluster-ordered vertices against the source vertices they came from.
  // The denominator is the number of distinct `vertex_source` entries, not the container's
  // `source_vertex_count`, because for a mesh merged from several primitives the latter counts the
  // whole source vertex space once per primitive and no ratio against it means anything.
  Vector<u32> referenced;
  referenced.reserve(mesh.vertex_source.size());
  for (const u32 source : mesh.vertex_source)
    referenced.push_back(source);
  std::sort(referenced.begin(), referenced.end());
  const u32 distinct =
      static_cast<u32>(std::unique(referenced.begin(), referenced.end()) - referenced.begin());
  const f64 duplication =
      distinct == 0 ? 0.0 : static_cast<f64>(mesh.vertices.size()) / static_cast<f64>(distinct);

  // The page table, the spread of child pages per page — which is how much of the DAG a page
  // depends on and therefore how wide a prefetch is — and the fly-in sweep.
  JsonValue pages = page_summary(data.pages);
  Vector<u32> child_counts;
  child_counts.reserve(data.pages.pages.size());
  for (const geometry::ClusterPageDesc& page : data.pages.pages)
    child_counts.push_back(page.child_page_count);
  std::sort(child_counts.begin(), child_counts.end());
  JsonValue child_histogram = JsonValue::array();
  for (u32 i = 0; i < child_counts.size();) {
    u32 j = i;
    while (j < child_counts.size() && child_counts[j] == child_counts[i])
      ++j;
    JsonValue bucket = JsonValue::object();
    bucket.set("child_pages", JsonValue(child_counts[i]));
    bucket.set("pages", JsonValue(j - i));
    child_histogram.push_back(std::move(bucket));
    i = j;
  }
  pages.set("child_pages_histogram", std::move(child_histogram));
  // The page table's own invariants, checked against the mesh it came with: a container is read
  // by the renderer, and this is the one place that says out loud whether its table is sound.
  if (!data.pages.pages.empty()) {
    std::string page_error;
    const bool valid = geometry::validate_cluster_pages(data.mesh, data.pages, &page_error);
    pages.set("valid", JsonValue(valid));
    if (!valid) pages.set("invalid_reason", JsonValue(page_error));
  }

  Vector<SweepStep> sweep;
  u64 budget = 0;
  std::string sweep_error;
  if (!sweep_streaming(data, sweep, budget, sweep_error)) {
    out_error = std::move(sweep_error);
    return false;
  }
  JsonValue streaming = JsonValue::object();
  streaming.set("budget_bytes", JsonValue(budget));
  streaming.set("budget_fraction", JsonValue(k_sweep_budget));
  streaming.set("steps", JsonValue(k_sweep_steps));
  streaming.set("from_radii", JsonValue(k_sweep_far));
  streaming.set("to_radii", JsonValue(k_sweep_near));
  JsonValue steps = JsonValue::array();
  u64 requested_total = 0;
  u32 most_needed = 0;
  for (const SweepStep& step : sweep) {
    requested_total += step.requested;
    most_needed = step.pages_needed > most_needed ? step.pages_needed : most_needed;
    JsonValue entry = JsonValue::object();
    entry.set("radii", JsonValue(step.distance));
    entry.set("clusters", JsonValue(step.clusters));
    entry.set("triangles", JsonValue(step.triangles));
    entry.set("pages_needed", JsonValue(step.pages_needed));
    entry.set("requested", JsonValue(step.requested));
    entry.set("drawn", JsonValue(step.drawn));
    entry.set("evicted", JsonValue(step.evicted));
    entry.set("resident", JsonValue(step.resident));
    entry.set("resident_bytes", JsonValue(step.resident_bytes));
    steps.push_back(std::move(entry));
  }
  streaming.set("requested_total", JsonValue(requested_total));
  streaming.set("pages_needed_max", JsonValue(most_needed));
  streaming.set("steps_detail", std::move(steps));
  pages.set("streaming", std::move(streaming));
  append_sweep(human, sweep, budget);

  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("clusters", JsonValue(cluster_count));
  summary.set("pages", std::move(pages));
  summary.set("lod_levels", JsonValue(data.mesh.level_cluster_counts.size()));
  summary.set("level_clusters", std::move(levels));
  summary.set("groups", JsonValue(data.mesh.group_count));
  summary.set("leaf_triangles", JsonValue(data.mesh.leaf_triangle_count));
  summary.set("triangles_per_cluster", std::move(triangles_per_cluster));
  summary.set("source_vertices", JsonValue(mesh.source_vertex_count));
  summary.set("referenced_source_vertices", JsonValue(distinct));
  summary.set("cluster_vertices", JsonValue(mesh.vertices.size()));
  summary.set("vertex_duplication", JsonValue(duplication));
  summary.set("bytes", std::move(bytes));
  summary.set("quantization", std::move(quantization));
  summary.set("materials", JsonValue(data.materials.size()));
  // The images, slot by slot: a container is only drawable on its own when every image either
  // names a file or carries its bytes, so the per-image line says which it is and what it costs.
  const geometry::ClusterImageSummary image_summary = geometry::summarize_cluster_images(data);
  JsonValue image_detail = JsonValue::array();
  for (u32 i = 0; i < image_summary.count; ++i) {
    JsonValue entry = JsonValue::object();
    entry.set("path", JsonValue(i < data.image_paths.size() ? data.image_paths[i] : std::string()));
    const u64 carried = i < data.images.size() ? data.images[i].bytes.size() : 0;
    entry.set("bytes", JsonValue(carried));
    entry.set("mime", JsonValue(i < data.images.size() ? data.images[i].mime_type : std::string()));
    image_detail.push_back(std::move(entry));
  }
  JsonValue images = JsonValue::object();
  images.set("count", JsonValue(image_summary.count));
  images.set("embedded", JsonValue(image_summary.embedded));
  images.set("deduplicated", JsonValue(image_summary.deduplicated));
  images.set("bytes", JsonValue(image_summary.bytes));
  images.set("detail", std::move(image_detail));
  summary.set("images", JsonValue(image_summary.count));
  summary.set("image_detail", std::move(images));

  // The morph stream, channel by channel. The two numbers that decide whether the sparse layout
  // was worth building are `touched_fraction` — the share of the mesh's cluster vertices a channel
  // moves — and `bytes`, which is what a page has to carry for it (geometry.md, "Morph channels").
  if (!mesh.morph_channels.empty()) {
    Vector<u32> touched(mesh.morph_channels.size(), 0u);
    for (const geometry::MorphSlice& slice : mesh.morph_slices)
      touched[slice.channel] += slice.delta_count;
    const bool normals = !mesh.morph_normal_deltas.empty();
    const u32 per_delta = 1u + 6u + (normals ? 6u : 0u);
    JsonValue channels = JsonValue::array();
    u64 total_bytes = u64{mesh.morph_channels.size()} * sizeof(geometry::MorphChannel) +
                      u64{mesh.morph_cluster_slices.size()} * sizeof(u32) +
                      u64{mesh.morph_slices.size()} * sizeof(geometry::MorphSlice) +
                      u64{mesh.morph_delta_count} * per_delta;
    for (u32 c = 0; c < mesh.morph_channels.size(); ++c) {
      JsonValue entry = JsonValue::object();
      entry.set("name",
                JsonValue(c < mesh.morph_names.size() ? mesh.morph_names[c] : std::string()));
      entry.set("deltas", JsonValue(touched[c]));
      entry.set("touched_fraction",
                JsonValue(mesh.vertices.empty() ? 0.0
                                                : static_cast<f64>(touched[c]) /
                                                      static_cast<f64>(mesh.vertices.size())));
      entry.set("max_displacement",
                JsonValue(static_cast<f64>(mesh.morph_channels[c].max_displacement)));
      entry.set("position_scale",
                JsonValue(static_cast<f64>(mesh.morph_channels[c].position_scale)));
      entry.set("default_weight",
                JsonValue(static_cast<f64>(mesh.morph_channels[c].default_weight)));
      entry.set("bytes", JsonValue(u64{touched[c]} * per_delta));
      channels.push_back(std::move(entry));
    }
    JsonValue morph = JsonValue::object();
    morph.set("channels", JsonValue(mesh.morph_channels.size()));
    morph.set("deltas", JsonValue(mesh.morph_delta_count));
    morph.set("slices", JsonValue(mesh.morph_slices.size()));
    morph.set("normal_deltas", JsonValue(normals));
    morph.set("bytes", JsonValue(total_bytes));
    morph.set("bytes_per_cluster_vertex",
              JsonValue(mesh.vertices.empty() ? 0.0
                                              : static_cast<f64>(total_bytes) /
                                                    static_cast<f64>(mesh.vertices.size())));
    morph.set("detail", std::move(channels));
    summary.set("morph", std::move(morph));
    appendf(human,
            "  morph: %u channels, %u deltas over %u slices, %llu bytes (%.2f a cluster "
            "vertex)%s\n",
            mesh.morph_channels.size(), mesh.morph_delta_count, mesh.morph_slices.size(),
            static_cast<unsigned long long>(total_bytes),
            mesh.vertices.empty()
                ? 0.0
                : static_cast<double>(total_bytes) / static_cast<double>(mesh.vertices.size()),
            normals ? "" : ", positions only");
  }

  // The canonical vertex ids: which id space, and how the mesh's source vertices fall into it.
  // `sharing` is also an atlas number in its own right — with derived ids it counts the vertices a
  // seam of any kind duplicated, which is what an atlas repack or a remesh moves.
  const IdentityStats identity_stats = measure_identity(mesh);
  {
    JsonValue identity = JsonValue::object();
    identity.set("source", JsonValue(geometry::vertex_id_source_name(mesh.vertex_id_source)));
    identity.set("ids", JsonValue(mesh.vertex_ids.size()));
    identity.set("bytes", JsonValue(u64{mesh.vertex_ids.size()} * sizeof(u32)));
    if (identity_stats.valid) {
      identity.set("source_vertices", JsonValue(identity_stats.source_vertices));
      identity.set("distinct_ids", JsonValue(identity_stats.distinct_ids));
      identity.set("sharing", JsonValue(identity_stats.sharing));
      identity.set("duplicates", JsonValue(identity_stats.duplicates));
      identity.set("largest_share", JsonValue(identity_stats.largest_share));
      identity.set("without_id", JsonValue(identity_stats.without_id));
      identity.set("ids_at_several_positions", JsonValue(identity_stats.ids_at_several_positions));
      appendf(human,
              "  identity: %s ids, %u source vertices name %u points of the base; %u share an "
              "id (%u duplicates, at most %u on one), %u without one, %u ids at more than one "
              "position\n",
              geometry::vertex_id_source_name(mesh.vertex_id_source),
              identity_stats.source_vertices, identity_stats.distinct_ids, identity_stats.sharing,
              identity_stats.duplicates, identity_stats.largest_share, identity_stats.without_id,
              identity_stats.ids_at_several_positions);
    }
    summary.set("identity", std::move(identity));
  }

  // The atlas, and how much of the texture the LOD cut moves. Both are about the *picture* rather
  // than the bytes, and both are here because the seam rule of geometry.md made atlas
  // fragmentation a cost the asset controls rather than a defect the builder hides.
  const AtlasStats atlas_stats = measure_atlas(data.mesh);
  if (atlas_stats.valid) {
    JsonValue atlas = JsonValue::object();
    atlas.set("islands", JsonValue(atlas_stats.islands));
    atlas.set("source_vertices", JsonValue(atlas_stats.source_vertices));
    atlas.set("seam_vertices", JsonValue(atlas_stats.seam_vertices));
    atlas.set("seam_fraction", JsonValue(atlas_stats.source_vertices == 0
                                             ? 0.0
                                             : static_cast<f64>(atlas_stats.seam_vertices) /
                                                   static_cast<f64>(atlas_stats.source_vertices)));
    atlas.set("uv_area", JsonValue(atlas_stats.uv_area));
    atlas.set("smallest_island_texels_4096", JsonValue(atlas_stats.smallest_texels));
    atlas.set("smallest_island_triangles", JsonValue(atlas_stats.smallest_triangles));
    atlas.set("median_island_texels_4096", JsonValue(atlas_stats.median_texels));
    atlas.set("largest_island_texels_4096", JsonValue(atlas_stats.largest_texels));
    atlas.set("triangles_per_island",
              JsonValue(atlas_stats.islands == 0 ? 0.0
                                                 : static_cast<f64>(data.mesh.leaf_triangle_count) /
                                                       static_cast<f64>(atlas_stats.islands)));
    summary.set("atlas", std::move(atlas));
    appendf(human,
            "  atlas: %u islands, %u of %u vertices on a seam (%.1f%%), smallest island "
            "%.0f texels of 4096 over %u triangles\n",
            atlas_stats.islands, atlas_stats.seam_vertices, atlas_stats.source_vertices,
            atlas_stats.source_vertices == 0
                ? 0.0
                : 100.0 * static_cast<double>(atlas_stats.seam_vertices) /
                      static_cast<double>(atlas_stats.source_vertices),
            static_cast<double>(atlas_stats.smallest_texels), atlas_stats.smallest_triangles);
  }
  const Vector<AttributeErrorRow> attribute_rows = measure_attribute_error(data.mesh);
  if (!attribute_rows.empty()) {
    JsonValue rows = JsonValue::array();
    appendf(human, "  LOD attribute error (closest point, texels of a 4096 atlas)\n");
    appendf(human, "  %8s %9s %10s %9s %9s %9s %9s %9s\n", "budget", "clusters", "triangles",
            "uv mean", "uv p99", "uv max", "over 8px", "normal");
    for (const AttributeErrorRow& row : attribute_rows) {
      JsonValue entry = JsonValue::object();
      entry.set("triangle_fraction", JsonValue(row.fraction));
      entry.set("clusters", JsonValue(row.clusters));
      entry.set("triangles", JsonValue(row.triangles));
      entry.set("samples", JsonValue(row.error.samples));
      entry.set("uv_mean_texels", JsonValue(row.error.uv_mean_texels));
      entry.set("uv_p99_texels", JsonValue(row.error.uv_p99_texels));
      entry.set("uv_max_texels", JsonValue(row.error.uv_max_texels));
      entry.set("uv_outlier_fraction", JsonValue(row.error.uv_outlier_fraction()));
      entry.set("normal_mean_deg", JsonValue(row.error.normal_mean_deg));
      entry.set("normal_max_deg", JsonValue(row.error.normal_max_deg));
      rows.push_back(std::move(entry));
      appendf(human, "  %7.0f%% %9u %10u %9.1f %9.1f %9.1f %8.2f%% %8.2f\n", row.fraction * 100.0,
              row.clusters, row.triangles, static_cast<double>(row.error.uv_mean_texels),
              static_cast<double>(row.error.uv_p99_texels),
              static_cast<double>(row.error.uv_max_texels),
              static_cast<double>(row.error.uv_outlier_fraction()) * 100.0,
              static_cast<double>(row.error.normal_mean_deg));
    }
    summary.set("lod_attribute_error", std::move(rows));
  }

  summary.set("source_path", JsonValue(data.source_path));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  out_summary = std::move(summary);
  return true;
}

}  // namespace engine::content_build
