#include <domain/geometry/cluster.h>
#include <domain/geometry/uv_repair.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::geometry {

namespace {

constexpr f64 k_atlas_texels = 4096.0 * 4096.0;
constexpr u32 k_none = ~u32{0};
constexpr u32 k_shared = ~u32{0} - 1;  // `run_of` for a vertex more than one run references

// What happened to one triangle.
enum class TriangleState : u8 { sound, pending, dropped, refolded, unrepaired };

// A position and a UV by their bits, which is how the weld compares them: two vertices with the
// same key are the same point of the atlas, whatever their index.
struct AtlasKey {
  u32 px = 0;
  u32 py = 0;
  u32 pz = 0;
  u32 u = 0;
  u32 v = 0;
};

AtlasKey key_of(Vec3 p, Vec2 t) noexcept {
  return AtlasKey{std::bit_cast<u32>(p.x), std::bit_cast<u32>(p.y), std::bit_cast<u32>(p.z),
                  std::bit_cast<u32>(t.x), std::bit_cast<u32>(t.y)};
}

bool same_position(const AtlasKey& a, const AtlasKey& b) noexcept {
  return a.px == b.px && a.py == b.py && a.pz == b.pz;
}

bool same_key(const AtlasKey& a, const AtlasKey& b) noexcept {
  return same_position(a, b) && a.u == b.u && a.v == b.v;
}

bool key_less(const AtlasKey& a, const AtlasKey& b) noexcept {
  if (a.px != b.px) return a.px < b.px;
  if (a.py != b.py) return a.py < b.py;
  if (a.pz != b.pz) return a.pz < b.pz;
  if (a.u != b.u) return a.u < b.u;
  return a.v < b.v;
}

u32 find_root(Vector<u32>& parent, u32 x) noexcept {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

void unite(Vector<u32>& parent, u32 a, u32 b) noexcept {
  a = find_root(parent, a);
  b = find_root(parent, b);
  if (a == b) return;
  // The smaller index is the root, so a root is a function of the component and not of the
  // order the unions came in.
  if (b < a) std::swap(a, b);
  parent[b] = a;
}

Vec2 stored(Vec2 uv) noexcept { return decode_half2(encode_half2(uv)); }

// Twice the area over the longest edge: the triangle's smallest height, in the positions' units.
f64 thinness(Vec3 a, Vec3 b, Vec3 c) noexcept {
  const f64 e0[3] = {static_cast<f64>(b.x) - static_cast<f64>(a.x),
                     static_cast<f64>(b.y) - static_cast<f64>(a.y),
                     static_cast<f64>(b.z) - static_cast<f64>(a.z)};
  const f64 e1[3] = {static_cast<f64>(c.x) - static_cast<f64>(a.x),
                     static_cast<f64>(c.y) - static_cast<f64>(a.y),
                     static_cast<f64>(c.z) - static_cast<f64>(a.z)};
  const f64 e2[3] = {static_cast<f64>(c.x) - static_cast<f64>(b.x),
                     static_cast<f64>(c.y) - static_cast<f64>(b.y),
                     static_cast<f64>(c.z) - static_cast<f64>(b.z)};
  const f64 n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                    e0[0] * e1[1] - e0[1] * e1[0]};
  const f64 twice_area = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const f64 l0 = e0[0] * e0[0] + e0[1] * e0[1] + e0[2] * e0[2];
  const f64 l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
  const f64 l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
  const f64 longest = std::sqrt(std::max(l0, std::max(l1, l2)));
  return longest > 0.0 ? twice_area / longest : 0.0;
}

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// One run's atlas vertices and islands. Built only for a run that has an island under the
// threshold, so its cost is that run's and not the mesh's.
struct RunAtlas {
  Vector<u32> vertices;      // the run's distinct vertex indices, ascending
  Vector<u32> atlas_of;      // parallel to `vertices`: its atlas vertex
  Vector<Vec3> position;     // per atlas vertex
  Vector<Vec2> uv;           // per atlas vertex; a refold writes here first
  Vector<u32> group_begin;   // per atlas vertex: the first atlas vertex at the same position
  Vector<u32> group_end;     // and one past the last (atlas vertices are sorted by position first)
  Vector<u32> label;         // the sound island an atlas vertex belongs to, k_none while none
  Vector<u8> locked;         // a vertex of it is referenced by another run: never written
  Vector<u8> written;        // a refold gave it a new UV
  Vector<u32> normal_from;   // a vertex whose normal a refold gave it too, k_none for its own
  Vector<u32> first_vertex;  // per atlas vertex: its lowest vertex index, whose normal a twin lends
  Vector<f64> island_area;   // per island root, in stored texels
};

u32 atlas_vertex(const RunAtlas& atlas, u32 vertex) noexcept {
  const auto it = std::lower_bound(atlas.vertices.begin(), atlas.vertices.end(), vertex);
  return atlas.atlas_of[static_cast<u32>(it - atlas.vertices.begin())];
}

// The first atlas vertex at `a`'s position (other than `a`) that belongs to `island`, or k_none.
u32 twin_in(const RunAtlas& atlas, u32 a, u32 island) noexcept {
  for (u32 t = atlas.group_begin[a]; t < atlas.group_end[a]; ++t) {
    if (t != a && atlas.label[t] == island) return t;
  }
  return k_none;
}

struct Choice {
  u32 island = k_none;
  u32 support = 0;  // how many of the triangle's corners the island reaches
  bool blocked = false;
};

// Which island a UV-degenerate triangle folds into. A corner an earlier fold already gave to an
// island decides it (the triangle is joined to that island through that vertex, and a second
// island would make its UVs span the atlas); otherwise it is the island that reaches the most of
// its corners, then the larger island, then the lower id, so the choice is a function of the mesh.
Choice choose_island(const RunAtlas& atlas, const u32 corner[3]) noexcept {
  Choice out;
  u32 forced = k_none;
  for (u32 k = 0; k < 3; ++k) {
    const u32 a = corner[k];
    if (atlas.label[a] != k_none) {
      if (forced != k_none && forced != atlas.label[a]) {
        out.blocked = true;  // would bridge two islands
        return out;
      }
      forced = atlas.label[a];
    } else if (atlas.locked[a] != 0u) {
      out.blocked = true;  // a corner another run also uses: its UV is not this run's to change
      return out;
    }
  }
  if (forced != k_none) {
    out.island = forced;
    for (u32 k = 0; k < 3; ++k) {
      const u32 a = corner[k];
      if (atlas.label[a] == forced || twin_in(atlas, a, forced) != k_none) ++out.support;
    }
    return out;
  }
  // No corner is in an island yet: count, per island, how many corners have a twin in it.
  constexpr u32 k_most = 12;  // islands meeting at a triangle's corners; more is not a real mesh
  u32 islands[k_most] = {};
  u32 votes[k_most] = {};
  u32 count = 0;
  for (u32 k = 0; k < 3; ++k) {
    // The distinct islands this corner has a twin in: each gets one vote from it.
    const u32 a = corner[k];
    u32 mine[k_most] = {};
    u32 mine_count = 0;
    for (u32 t = atlas.group_begin[a]; t < atlas.group_end[a]; ++t) {
      const u32 island = atlas.label[t];
      if (t == a || island == k_none) continue;
      bool again = false;
      for (u32 m = 0; m < mine_count; ++m)
        again = again || mine[m] == island;
      if (!again && mine_count < k_most) mine[mine_count++] = island;
    }
    for (u32 m = 0; m < mine_count; ++m) {
      u32 slot = 0;
      while (slot < count && islands[slot] != mine[m])
        ++slot;
      if (slot == count) {
        if (count == k_most) continue;
        islands[count] = mine[m];
        votes[count] = 0;
        ++count;
      }
      ++votes[slot];
    }
  }
  for (u32 i = 0; i < count; ++i) {
    const bool better = out.island == k_none || votes[i] > out.support ||
                        (votes[i] == out.support &&
                         (atlas.island_area[islands[i]] > atlas.island_area[out.island] ||
                          (atlas.island_area[islands[i]] == atlas.island_area[out.island] &&
                           islands[i] < out.island)));
    if (better) {
      out.island = islands[i];
      out.support = votes[i];
    }
  }
  return out;
}

// Folds one triangle into `island`: corners with a twin in the island take the twin's UV, and
// the rest are put on the edge between the corners that did.
void refold(RunAtlas& atlas, const u32 corner[3], u32 island, UvRepairReport& report) {
  bool anchored[3] = {};
  for (u32 k = 0; k < 3; ++k) {
    const u32 a = corner[k];
    if (atlas.label[a] == island) {
      anchored[k] = true;
      continue;
    }
    const u32 twin = twin_in(atlas, a, island);
    if (twin == k_none) continue;
    // The twin's UV, and its normal: the corner is to *become* the island's vertex at this point,
    // so that the weld merges the two. A generator's remesher splits the normals along every chart
    // border it cut (E10's second pass: every one of 22,862 seam pairs on one prop), and a corner
    // that kept its own normal there would take the right UV and still be an island of its own.
    atlas.uv[a] = atlas.uv[twin];
    // A twin that was itself folded lends the normal it borrowed, so the chain is one step long
    // and the write-back below never copies a normal it is about to overwrite.
    atlas.normal_from[a] =
        atlas.normal_from[twin] != k_none ? atlas.normal_from[twin] : atlas.first_vertex[twin];
    atlas.label[a] = island;
    atlas.written[a] = 1u;
    anchored[k] = true;
    ++report.corners_from_neighbours;
  }
  for (u32 k = 0; k < 3; ++k) {
    if (anchored[k]) continue;
    const u32 a = corner[k];
    u32 ends[2] = {k_none, k_none};
    u32 n = 0;
    for (u32 j = 0; j < 3; ++j) {
      if (j != k && anchored[j] && n < 2) ends[n++] = corner[j];
    }
    Vec2 uv = atlas.uv[ends[0]];
    if (n == 2) {
      // The point of the shared edge nearest this corner, in the positions' metric.
      const Vec3 p = atlas.position[a];
      const Vec3 p0 = atlas.position[ends[0]];
      const Vec3 p1 = atlas.position[ends[1]];
      const f64 d[3] = {static_cast<f64>(p1.x) - static_cast<f64>(p0.x),
                        static_cast<f64>(p1.y) - static_cast<f64>(p0.y),
                        static_cast<f64>(p1.z) - static_cast<f64>(p0.z)};
      const f64 w[3] = {static_cast<f64>(p.x) - static_cast<f64>(p0.x),
                        static_cast<f64>(p.y) - static_cast<f64>(p0.y),
                        static_cast<f64>(p.z) - static_cast<f64>(p0.z)};
      const f64 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
      f64 t = dd > 0.0 ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd : 0.0;
      t = std::clamp(t, 0.0, 1.0);
      const Vec2 u0 = atlas.uv[ends[0]];
      const Vec2 u1 = atlas.uv[ends[1]];
      uv = Vec2{static_cast<f32>(static_cast<f64>(u0.x) +
                                 (static_cast<f64>(u1.x) - static_cast<f64>(u0.x)) * t),
                static_cast<f32>(static_cast<f64>(u0.y) +
                                 (static_cast<f64>(u1.y) - static_cast<f64>(u0.y)) * t)};
    }
    atlas.uv[a] = uv;
    atlas.label[a] = island;
    atlas.written[a] = 1u;
    anchored[k] = true;
    ++report.corners_on_edge;
  }
}

}  // namespace

f64 uv_area_texels(Vec2 a, Vec2 b, Vec2 c) noexcept {
  const f64 e0x = static_cast<f64>(b.x) - static_cast<f64>(a.x);
  const f64 e0y = static_cast<f64>(b.y) - static_cast<f64>(a.y);
  const f64 e1x = static_cast<f64>(c.x) - static_cast<f64>(a.x);
  const f64 e1y = static_cast<f64>(c.y) - static_cast<f64>(a.y);
  return std::fabs(e0x * e1y - e0y * e1x) * 0.5 * k_atlas_texels;
}

f64 stored_uv_area_texels(Vec2 a, Vec2 b, Vec2 c) noexcept {
  return uv_area_texels(stored(a), stored(b), stored(c));
}

bool repair_uv_degenerate_triangles(std::span<const Vec3> positions, Vector<Vec3>& normals,
                                    Vector<Vec2>& uvs, Vector<u32>& indices,
                                    std::span<UvRepairRange> ranges, const UvRepairOptions& options,
                                    UvRepairReport& report, std::string* error) {
  report = UvRepairReport{};
  report.threshold_texels = options.threshold_texels;
  const u32 vertex_count = static_cast<u32>(positions.size());
  if (uvs.empty()) return true;  // nothing samples a texture through UVs that do not exist
  if (uvs.size() != vertex_count) {
    return fail(error, "uv repair: " + std::to_string(uvs.size()) + " UVs for " +
                           std::to_string(vertex_count) + " positions");
  }
  if (!normals.empty() && normals.size() != vertex_count) {
    return fail(error, "uv repair: " + std::to_string(normals.size()) + " normals for " +
                           std::to_string(vertex_count) + " positions");
  }
  u32 previous_end = 0;
  for (u32 r = 0; r < ranges.size(); ++r) {
    const UvRepairRange& range = ranges[r];
    const std::string which = "uv repair: run " + std::to_string(r);
    if (range.first % 3 != 0 || range.count % 3 != 0)
      return fail(error, which + " is not a whole number of triangles");
    if (u64{range.first} + range.count > indices.size())
      return fail(error, which + " runs past the " + std::to_string(indices.size()) + " indices");
    if (range.first < previous_end) return fail(error, which + " overlaps or precedes the last");
    previous_end = range.first + range.count;
    for (u32 i = range.first; i < range.first + range.count; ++i) {
      if (indices[i] >= vertex_count) {
        return fail(error, which + ": index " + std::to_string(indices[i]) + " is outside the " +
                               std::to_string(vertex_count) + " vertices");
      }
    }
  }

  // Which runs a vertex belongs to (a refold may only write a UV no other run reads), and a first,
  // cheap look for islands under the threshold: components over vertex *indices*, which are never
  // larger than the atlas islands below (duplicates at one position and UV only join them), so a
  // run whose index islands are all big enough has no small atlas island either and is skipped.
  // That is every run of a clean mesh, which then costs one pass over its triangles.
  Vector<u32> run_of(vertex_count, k_none);
  Vector<u32> parent(vertex_count);
  for (u32 v = 0; v < vertex_count; ++v)
    parent[v] = v;
  for (u32 r = 0; r < ranges.size(); ++r) {
    const UvRepairRange& range = ranges[r];
    for (u32 i = range.first; i < range.first + range.count; i += 3) {
      for (u32 k = 0; k < 3; ++k) {
        u32& owner = run_of[indices[i + k]];
        owner = owner == k_none || owner == r ? r : k_shared;
      }
      if (!range.uv_mapped) continue;
      unite(parent, indices[i], indices[i + 1]);
      unite(parent, indices[i], indices[i + 2]);
    }
  }
  Vector<f64> index_island_area(vertex_count, 0.0);
  for (const UvRepairRange& range : ranges) {
    if (!range.uv_mapped) continue;
    for (u32 i = range.first; i < range.first + range.count; i += 3) {
      index_island_area[find_root(parent, indices[i])] +=
          stored_uv_area_texels(uvs[indices[i]], uvs[indices[i + 1]], uvs[indices[i + 2]]);
    }
  }
  Vector<u8> run_needs_atlas(static_cast<u32>(ranges.size()), u8{0});
  bool any_run = false;
  for (u32 r = 0; r < ranges.size(); ++r) {
    const UvRepairRange& range = ranges[r];
    if (!range.uv_mapped) continue;
    for (u32 i = range.first; i < range.first + range.count; i += 3) {
      if (index_island_area[find_root(parent, indices[i])] < options.threshold_texels) {
        run_needs_atlas[r] = 1u;
        any_run = true;
        break;
      }
    }
  }
  if (!any_run) return true;
  index_island_area = Vector<f64>{};

  // The 16-bit grid `quantize_positions` will put the whole mesh on: its step is the largest
  // extent of the referenced positions over 65535, and 1 for a mesh with no extent, as there.
  Vec3 lo{0.0f, 0.0f, 0.0f};
  Vec3 hi{0.0f, 0.0f, 0.0f};
  bool first_position = true;
  for (const UvRepairRange& range : ranges) {
    for (u32 i = range.first; i < range.first + range.count; ++i) {
      const Vec3 p = positions[indices[i]];
      if (first_position) {
        lo = hi = p;
        first_position = false;
        continue;
      }
      lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
  }
  const f64 extent = std::max(static_cast<f64>(hi.x) - static_cast<f64>(lo.x),
                              std::max(static_cast<f64>(hi.y) - static_cast<f64>(lo.y),
                                       static_cast<f64>(hi.z) - static_cast<f64>(lo.z)));
  const f64 grid_step = extent > 0.0 ? extent / 65535.0 : 1.0;

  const u32 triangle_count = static_cast<u32>(indices.size() / 3);
  Vector<TriangleState> state(triangle_count, TriangleState::sound);
  RunAtlas atlas;
  Vector<u32> order;
  Vector<AtlasKey> keys;
  Vector<u32> atlas_parent;
  Vector<u32> corners;  // per triangle of the run, its three atlas vertices
  Vector<u32> pending;
  for (u32 r = 0; r < ranges.size(); ++r) {
    if (run_needs_atlas[r] == 0u) continue;
    const UvRepairRange& range = ranges[r];
    const u32 t_first = range.first / 3;
    const u32 t_end = (range.first + range.count) / 3;

    // The run's atlas vertices: its distinct vertices sorted by position, then UV, then index.
    atlas.vertices.clear();
    atlas.vertices.reserve(range.count);
    for (u32 i = range.first; i < range.first + range.count; ++i)
      atlas.vertices.push_back(indices[i]);
    std::sort(atlas.vertices.begin(), atlas.vertices.end());
    atlas.vertices.erase(std::unique(atlas.vertices.begin(), atlas.vertices.end()),
                         atlas.vertices.end());
    const u32 local_count = atlas.vertices.size();
    keys.clear();
    keys.reserve(local_count);
    order.clear();
    order.reserve(local_count);
    for (u32 s = 0; s < local_count; ++s) {
      keys.push_back(key_of(positions[atlas.vertices[s]], uvs[atlas.vertices[s]]));
      order.push_back(s);
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
      if (key_less(keys[a], keys[b])) return true;
      if (key_less(keys[b], keys[a])) return false;
      return atlas.vertices[a] < atlas.vertices[b];
    });
    atlas.atlas_of.clear();
    atlas.atlas_of.resize(local_count);
    atlas.position.clear();
    atlas.uv.clear();
    atlas.locked.clear();
    atlas.first_vertex.clear();
    for (u32 o = 0; o < local_count; ++o) {
      const u32 s = order[o];
      if (o == 0 || !same_key(keys[s], keys[order[o - 1]])) {
        // Sorted by index within a key, so the first vertex seen is the lowest index.
        atlas.position.push_back(positions[atlas.vertices[s]]);
        atlas.uv.push_back(uvs[atlas.vertices[s]]);
        atlas.locked.push_back(0u);
        atlas.first_vertex.push_back(atlas.vertices[s]);
      }
      const u32 a = atlas.position.size() - 1;
      atlas.atlas_of[s] = a;
      if (run_of[atlas.vertices[s]] == k_shared) atlas.locked[a] = 1u;
    }
    const u32 atlas_count = atlas.position.size();
    atlas.group_begin.clear();
    atlas.group_begin.resize(atlas_count);
    atlas.group_end.clear();
    atlas.group_end.resize(atlas_count);
    for (u32 a = 0; a < atlas_count;) {
      u32 b = a + 1;
      const AtlasKey ka = key_of(atlas.position[a], Vec2{});
      while (b < atlas_count && same_position(key_of(atlas.position[b], Vec2{}), ka))
        ++b;
      for (u32 c = a; c < b; ++c) {
        atlas.group_begin[c] = a;
        atlas.group_end[c] = b;
      }
      a = b;
    }

    // Islands over atlas vertices, and their areas as stored.
    corners.clear();
    corners.reserve((t_end - t_first) * 3);
    for (u32 i = range.first; i < range.first + range.count; ++i)
      corners.push_back(atlas_vertex(atlas, indices[i]));
    atlas_parent.clear();
    atlas_parent.resize(atlas_count);
    for (u32 a = 0; a < atlas_count; ++a)
      atlas_parent[a] = a;
    for (u32 j = 0; j < corners.size(); j += 3) {
      unite(atlas_parent, corners[j], corners[j + 1]);
      unite(atlas_parent, corners[j], corners[j + 2]);
    }
    atlas.island_area.clear();
    atlas.island_area.resize(atlas_count, 0.0);
    for (u32 j = 0; j < corners.size(); j += 3) {
      const u32 i = range.first + j;
      atlas.island_area[find_root(atlas_parent, corners[j])] +=
          stored_uv_area_texels(uvs[indices[i]], uvs[indices[i + 1]], uvs[indices[i + 2]]);
    }

    // A triangle of an island under the threshold is UV-degenerate; every other triangle is
    // sound, and its atlas vertices are what a fold may borrow UVs from.
    atlas.label.clear();
    atlas.label.resize(atlas_count, k_none);
    atlas.written.clear();
    atlas.written.resize(atlas_count, u8{0});
    atlas.normal_from.clear();
    atlas.normal_from.resize(atlas_count, k_none);
    Vector<u8> small_root(atlas_count, u8{0});
    u32 remaining = t_end - t_first;
    for (u32 t = t_first; t < t_end; ++t) {
      const u32 j = (t - t_first) * 3;
      const u32 root = find_root(atlas_parent, corners[j]);
      if (atlas.island_area[root] < options.threshold_texels) {
        state[t] = TriangleState::pending;
        ++report.triangles;
        if (small_root[root] == 0u) {
          small_root[root] = 1u;
          ++report.islands;
        }
        continue;
      }
      for (u32 k = 0; k < 3; ++k)
        atlas.label[corners[j + k]] = root;
    }

    // Dropped first: a triangle thinner than a grid step whose texture cannot be sampled is
    // nothing a viewer can see, so it is not worth a neighbour's UVs. The last triangle of a run
    // is never dropped, because an empty primitive is a different problem for every consumer.
    for (u32 t = t_first; t < t_end; ++t) {
      if (state[t] != TriangleState::pending || remaining <= 1) continue;
      const u32 i = t * 3;
      if (thinness(positions[indices[i]], positions[indices[i + 1]], positions[indices[i + 2]]) <
          grid_step) {
        state[t] = TriangleState::dropped;
        ++report.dropped;
        --remaining;
      }
    }

    // Refold in waves: each sweep takes the triangles best supported by a sound island first (all
    // three corners, then two, then one), and a triangle folded in one sweep lends its corners to
    // the next, so a small island of several triangles folds in from its border. Every sweep that
    // makes progress folds at least one triangle, so this ends.
    pending.clear();
    for (u32 t = t_first; t < t_end; ++t) {
      if (state[t] == TriangleState::pending) pending.push_back(t);
    }
    bool progress = true;
    while (progress) {
      progress = false;
      for (u32 level = 3; level >= 1 && !progress; --level) {
        for (const u32 t : pending) {
          if (state[t] != TriangleState::pending) continue;
          const u32* c = &corners[(t - t_first) * 3];
          const Choice choice = choose_island(atlas, c);
          if (choice.blocked || choice.island == k_none || choice.support < level) continue;
          refold(atlas, c, choice.island, report);
          state[t] = TriangleState::refolded;
          ++report.refolded;
          progress = true;
        }
      }
    }
    for (const u32 t : pending) {
      if (state[t] == TriangleState::pending) {
        state[t] = TriangleState::unrepaired;
        ++report.unrepaired;
      }
    }

    // Write the new UVs (and borrowed normals) back to every vertex of an atlas vertex a refold
    // moved. Only a vertex of a small island is ever written, and nothing else reads one.
    for (u32 s = 0; s < local_count; ++s) {
      const u32 a = atlas.atlas_of[s];
      if (atlas.written[a] == 0u) continue;
      uvs[atlas.vertices[s]] = atlas.uv[a];
      if (!normals.empty() && atlas.normal_from[a] != k_none)
        normals[atlas.vertices[s]] = normals[atlas.normal_from[a]];
    }
  }

  // The dropped triangles leave the index buffer; runs keep their order and move down.
  if (report.dropped != 0) {
    Vector<u32> kept;
    kept.reserve(indices.size() - report.dropped * 3);
    u32 cursor = 0;
    for (UvRepairRange& range : ranges) {
      for (; cursor < range.first; ++cursor)
        kept.push_back(indices[cursor]);
      const u32 first_kept = kept.size();
      for (u32 i = range.first; i < range.first + range.count; i += 3) {
        if (state[i / 3] == TriangleState::dropped) continue;
        kept.push_back(indices[i]);
        kept.push_back(indices[i + 1]);
        kept.push_back(indices[i + 2]);
      }
      cursor = range.first + range.count;
      range.first = first_kept;
      range.count = kept.size() - first_kept;
    }
    for (; cursor < indices.size(); ++cursor)
      kept.push_back(indices[cursor]);
    indices = std::move(kept);
  }
  return true;
}

}  // namespace engine::geometry
