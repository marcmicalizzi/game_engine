// The far form of a rigid mesh (far_form.h; docs/subsystems/geometry.md, "The far form"; G198).
#include <domain/geometry/far_form.h>

#include <core/hash/hash.h>

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::geometry {
namespace {

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

u32 find_root(Vector<u32>& parent, u32 v) noexcept {
  while (parent[v] != v) {
    parent[v] = parent[parent[v]];
    v = parent[v];
  }
  return v;
}

// The smaller root wins, so the pieces are a function of the vertices' order alone.
void unite(Vector<u32>& parent, u32 a, u32 b) noexcept {
  a = find_root(parent, a);
  b = find_root(parent, b);
  if (a == b) return;
  if (a < b)
    parent[b] = a;
  else
    parent[a] = b;
}

u32 bits_of(f32 v) noexcept {
  u32 b = 0;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

f32 length3(Vec3 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

}  // namespace

bool bake_far_form(const ClusterLodMesh& mesh, std::span<const u32> material, FarForm& out,
                   std::string* error) {
  out = FarForm{};
  const ClusterMesh& m = mesh.mesh;
  const u32 clusters = m.clusters.size();
  if (mesh.lod.size() != clusters) return fail(error, "far form: the mesh has no LOD records");
  if (!material.empty() && material.size() != clusters)
    return fail(error, "far form: the material map is not one entry a cluster");
  const bool by_source = m.vertex_source.size() == m.vertices.size();

  // The finest level, welded by source vertex (every copy of a source vertex across clusters is
  // one vertex here), in cluster order.
  u32 keys = 0;
  for (u32 c = 0; c < clusters; ++c) {
    if (mesh.lod[c].level != 0) continue;
    const ClusterDesc& cl = m.clusters[c];
    for (u32 v = 0; v < cl.vertex_count; ++v) {
      const u32 at = cl.vertex_offset + v;
      keys = std::max(keys, (by_source ? m.vertex_source[at] : at) + 1);
    }
  }
  Vector<u32> vertex_of(keys, ~0u);
  Vector<FarFormVertex> vertices;
  Vector<u32> tri;      // three per triangle, into `vertices`
  Vector<u32> tri_mat;  // per triangle
  for (u32 c = 0; c < clusters; ++c) {
    if (mesh.lod[c].level != 0) continue;
    const ClusterDesc& cl = m.clusters[c];
    const u32 mat = material.empty() ? 0u : material[c];
    for (u32 t = 0; t < cl.triangle_count; ++t) {
      const u32 packed = m.triangles[cl.triangle_offset + t];
      u32 corner[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 at = cl.vertex_offset + ((packed >> (8 * k)) & 0xFFu);
        const u32 key = by_source ? m.vertex_source[at] : at;
        if (vertex_of[key] == ~0u) {
          vertex_of[key] = vertices.size();
          FarFormVertex fv;
          fv.position = m.vertices[at];
          if (at < m.attributes.size()) {
            fv.normal_oct = m.attributes[at].normal_oct;
            fv.uv_half2 = m.attributes[at].uv_half2;
          }
          vertices.push_back(fv);
        }
        corner[k] = vertex_of[key];
      }
      if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
      for (u32 k = 0; k < 3; ++k)
        tri.push_back(corner[k]);
      tri_mat.push_back(mat);
    }
  }
  const u32 triangles = tri_mat.size();
  if (triangles == 0) return fail(error, "far form: the mesh has no finest triangles");

  // Its pieces: vertices joined by a triangle or standing at the same point (a seam's two copies).
  const u32 n = vertices.size();
  Vector<u32> parent(n);
  for (u32 v = 0; v < n; ++v)
    parent[v] = v;
  for (u32 t = 0; t < triangles; ++t) {
    unite(parent, tri[3 * t], tri[3 * t + 1]);
    unite(parent, tri[3 * t], tri[3 * t + 2]);
  }
  // What the simplifier may not cross: a vertex two materials share stays where it is (each
  // material is simplified alone, and the two must still meet), and one whose point another vertex
  // with another UV shares is protected (an atlas seam: geometry.md, "What the simplifier is given,
  // and why"). Everything else — a faceted stone's hard edges among them — may collapse across its
  // attributes, which is what lets a field of faceted stones simplify at all.
  Vector<u32> vertex_material(n, ~0u);
  Vector<u8> lock(n, 0);
  for (u32 t = 0; t < triangles; ++t)
    for (u32 k = 0; k < 3; ++k) {
      const u32 v = tri[3 * t + k];
      if (vertex_material[v] == ~0u)
        vertex_material[v] = tri_mat[t];
      else if (vertex_material[v] != tri_mat[t])
        lock[v] = meshopt_SimplifyVertex_Lock;
    }
  {
    struct Key {
      u32 x, y, z, v;
    };
    Vector<Key> order;
    order.reserve(n);
    for (u32 v = 0; v < n; ++v) {
      const Vec3 p = vertices[v].position;
      order.push_back(Key{bits_of(p.x), bits_of(p.y), bits_of(p.z), v});
    }
    std::sort(order.begin(), order.end(), [](const Key& a, const Key& b) {
      if (a.x != b.x) return a.x < b.x;
      if (a.y != b.y) return a.y < b.y;
      if (a.z != b.z) return a.z < b.z;
      return a.v < b.v;
    });
    for (u32 k = 1; k < n; ++k) {
      const Key& a = order[k - 1];
      const Key& b = order[k];
      if (a.x == b.x && a.y == b.y && a.z == b.z) unite(parent, a.v, b.v);
    }
    // Each run of one point: a seam of materials locks it, a seam of UVs protects it.
    for (u32 first = 0; first < n;) {
      u32 last = first + 1;
      while (last < n && order[last].x == order[first].x && order[last].y == order[first].y &&
             order[last].z == order[first].z)
        ++last;
      bool materials = false, uvs = false;
      for (u32 k = first + 1; k < last; ++k) {
        materials = materials || vertex_material[order[k].v] != vertex_material[order[first].v];
        uvs = uvs || vertices[order[k].v].uv_half2 != vertices[order[first].v].uv_half2;
      }
      for (u32 k = first; k < last && (materials || uvs); ++k) {
        u8& l = lock[order[k].v];
        if (materials) l = meshopt_SimplifyVertex_Lock;
        if (uvs && l == 0) l = meshopt_SimplifyVertex_Protect;
      }
      first = last;
    }
  }
  // The attribute the simplifier weighs: the UV, as the DAG's own simplification weighs it
  // (`ClusterLodOptions::uv_weight`, 0.5).
  Vector<f32> uv(2 * n);
  for (u32 v = 0; v < n; ++v) {
    const Vec2 t = decode_half2(vertices[v].uv_half2);
    uv[2 * v] = t.x;
    uv[2 * v + 1] = t.y;
  }
  const f32 uv_weights[2] = {0.5f, 0.5f};
  Vector<u32> piece_of(n, ~0u);
  u32 pieces = 0;
  for (u32 v = 0; v < n; ++v) {
    const u32 r = find_root(parent, v);
    if (piece_of[r] == ~0u) piece_of[r] = pieces++;
    piece_of[v] = piece_of[r];
  }
  Vector<Vec3> lo(pieces), hi(pieces);
  Vector<u8> seen(pieces, 0);
  Vec3 box_lo = vertices[0].position, box_hi = box_lo;
  for (u32 v = 0; v < n; ++v) {
    const Vec3 p = vertices[v].position;
    const u32 k = piece_of[v];
    if (seen[k] == 0) {
      lo[k] = hi[k] = p;
      seen[k] = 1;
    }
    lo[k] = Vec3{std::min(lo[k].x, p.x), std::min(lo[k].y, p.y), std::min(lo[k].z, p.z)};
    hi[k] = Vec3{std::max(hi[k].x, p.x), std::max(hi[k].y, p.y), std::max(hi[k].z, p.z)};
    box_lo = Vec3{std::min(box_lo.x, p.x), std::min(box_lo.y, p.y), std::min(box_lo.z, p.z)};
    box_hi = Vec3{std::max(box_hi.x, p.x), std::max(box_hi.y, p.y), std::max(box_hi.z, p.z)};
  }
  Vector<f32> extent(pieces);
  for (u32 k = 0; k < pieces; ++k)
    extent[k] = length3(hi[k] - lo[k]);
  const Vec3 center = (box_lo + box_hi) * 0.5f;
  f32 radius = 0.0f;
  for (u32 v = 0; v < n; ++v)
    radius = std::max(radius, length3(vertices[v].position - center));
  if (!(radius > 0.0f)) return fail(error, "far form: the mesh has no extent");

  // The materials in ascending order: each level's parts in that order.
  Vector<u32> mats(tri_mat);
  std::sort(mats.begin(), mats.end());
  mats.resize(static_cast<u32>(std::unique(mats.begin(), mats.end()) - mats.begin()));

  // Each level is simplified from the one before it (the finest for the first), so a level costs
  // the triangles of the one before and not the mesh's; its simplifier error then adds to the one
  // before's, which is what `simplify_m` records, and the target of each step is what is left of
  // the level's error after it.
  Vector<u32> raw;  // every level's indices, into `vertices`, before the compaction
  Vector<Vector<u32>> previous(mats.size());
  for (u32 mi = 0; mi < mats.size(); ++mi) {
    for (u32 t = 0; t < triangles; ++t) {
      if (tri_mat[t] != mats[mi]) continue;
      previous[mi].push_back(tri[3 * t]);
      previous[mi].push_back(tri[3 * t + 1]);
      previous[mi].push_back(tri[3 * t + 2]);
    }
  }
  Vector<f32> previous_error(mats.size(), 0.0f);
  Vector<u32> subset;
  Vector<u32> dest;
  f32 e = radius / k_far_form_radius_px;
  for (u32 level = 0; level < k_far_form_levels; ++level, e = e * 2.0f) {
    FarFormLevel record;
    record.error_m = e;
    record.first_part = out.parts.size();
    Vector<u8> piece_kept(pieces, 0), piece_used(pieces, 0);
    for (u32 t = 0; t < triangles; ++t) {
      const u32 k = piece_of[tri[3 * t]];
      piece_used[k] = 1;
      if (extent[k] > e) piece_kept[k] = 1;
    }
    for (u32 k = 0; k < pieces; ++k) {
      if (piece_used[k] == 0) continue;
      if (piece_kept[k] != 0) {
        ++record.kept_pieces;
      } else {
        ++record.dropped_pieces;
        record.dropped_m = std::max(record.dropped_m, extent[k]);
      }
    }
    for (u32 mi = 0; mi < mats.size(); ++mi) {
      subset.clear();
      const Vector<u32>& from = previous[mi];
      for (u32 i = 0; i + 2 < from.size(); i += 3) {
        if (piece_kept[piece_of[from[i]]] == 0) continue;
        subset.push_back(from[i]);
        subset.push_back(from[i + 1]);
        subset.push_back(from[i + 2]);
      }
      previous[mi].clear();
      if (subset.empty()) continue;
      dest.resize(subset.size());
      f32 reported = 0.0f;
      const f32 budget = std::max(e - previous_error[mi], 0.0f);
      const size_t count = meshopt_simplifyWithAttributes(
          dest.data(), subset.data(), subset.size(), &vertices[0].position.x, n,
          sizeof(FarFormVertex), uv.data(), 2 * sizeof(f32), uv_weights, 2, lock.data(), 0,
          budget,
          meshopt_SimplifyLockBorder | meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute |
              meshopt_SimplifyPermissive,
          &reported);
      previous_error[mi] = previous_error[mi] + reported;
      if (count == 0) continue;
      FarFormPart part;
      part.first_index = raw.size();
      part.index_count = static_cast<u32>(count);
      part.material = mats[mi];
      part.level = level;
      for (size_t i = 0; i < count; ++i) {
        raw.push_back(dest[static_cast<u32>(i)]);
        previous[mi].push_back(dest[static_cast<u32>(i)]);
      }
      out.parts.push_back(part);
      record.triangles += part.index_count / 3;
      record.simplify_m = std::max(record.simplify_m, previous_error[mi]);
    }
    record.part_count = out.parts.size() - record.first_part;
    out.levels.push_back(record);
  }

  // Only the vertices some level names, in the order the levels first name them.
  Vector<u32> compact(n, ~0u);
  out.indices.reserve(raw.size());
  for (const u32 v : raw) {
    if (compact[v] == ~0u) {
      compact[v] = out.vertices.size();
      out.vertices.push_back(vertices[v]);
    }
    out.indices.push_back(compact[v]);
  }
  out.header.levels = out.levels.size();
  out.header.source_triangles = triangles;
  out.header.pieces = pieces;
  out.header.center = center;
  out.header.radius = radius;
  return true;
}

f32 far_form_distance(const FarFormLevel& level, f32 proj_scale, f32 threshold_px,
                      f32 scale) noexcept {
  if (!(threshold_px > 0.0f)) return 0.0f;
  return FarForm::level_error(level) * scale * proj_scale / threshold_px;
}

u64 far_form_hash(const FarForm& form) noexcept {
  u64 h = hash_bytes(&form.header, sizeof(form.header));
  h = hash_combine(h, hash_bytes(form.levels.data(), form.levels.size() * sizeof(FarFormLevel)));
  h = hash_combine(h, hash_bytes(form.parts.data(), form.parts.size() * sizeof(FarFormPart)));
  h = hash_combine(h,
                   hash_bytes(form.vertices.data(), form.vertices.size() * sizeof(FarFormVertex)));
  h = hash_combine(h, hash_bytes(form.indices.data(), form.indices.size() * sizeof(u32)));
  return h;
}

}  // namespace engine::geometry
