#include <domain/assets/gltf.h>
#include <systems/renderer/crown_priors.h>

#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

void build_crown_priors(const SceneData& data, CrownPriorTable& out) {
  out = CrownPriorTable{};
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  const u32 meshes = static_cast<u32>(data.parts.size());
  if (meshes == 0) return;
  // A scene whose float positions are gone (a streamed one) has no leaves to read, and no table.
  const bool leaves = !mesh.vertices.empty() && data.sources.size() == data.parts.size();
  Vector<gfx::CrownPriorDesc> records;
  records.resize(meshes);
  Vector<u8> grids;
  Vector<Vec3> corners;
  Vector<u8> low;  // a mesh of leaf cards that stand under a crown's height
  low.resize(meshes, static_cast<u8>(0));
  // A distance form (`SceneData::form_source`) is a cut of its source's own clusters in its
  // source's mesh space: it reads its source's grid, made from the source's leaves, and none of
  // its own (a form's finest cards are merged ones).
  const auto is_form = [&](u32 m) noexcept {
    return m < data.form_source.size() && data.form_source[m] != m;
  };
  for (u32 m = 0; leaves && m < meshes; ++m) {
    if (is_form(m)) continue;
    const geometry::ClusterMeshPart& part = data.parts[m];
    const SourceMesh& source = data.sources[m];
    corners.clear();
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    f32 leaf_lo = 1e30f;
    f32 leaf_hi = -1e30f;
    for (u32 i = 0; i < part.cluster_count; ++i) {
      const u32 c = part.first_cluster + i;
      if (c >= mesh.clusters.size() || c >= data.lod.lod.size() || data.lod.lod[c].level != 0) {
        continue;
      }
      const geometry::ClusterDesc& cluster = mesh.clusters[c];
      if (static_cast<usize>(cluster.vertex_offset) + cluster.vertex_count > mesh.vertices.size()) {
        continue;
      }
      for (u32 v = 0; v < cluster.vertex_count; ++v) {
        const Vec3 p = mesh.vertices[cluster.vertex_offset + v];
        lo = Vec3{p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
        hi = Vec3{p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
      }
      // The cluster's material, as the upload finds it: its part's, alpha-tested for a leaf card.
      if (i >= source.part_of_cluster.size()) continue;
      const u32 part_index = source.part_of_cluster[i];
      if (part_index >= source.part_material.size()) continue;
      const i32 material = source.part_material[part_index];
      if (material < 0 || static_cast<u32>(material) >= source.data.materials.size() ||
          source.data.materials[static_cast<u32>(material)].alpha_mode != assets::k_alpha_mask) {
        continue;
      }
      if (static_cast<usize>(cluster.triangle_offset) + cluster.triangle_count >
          mesh.triangles.size()) {
        continue;
      }
      for (u32 t = 0; t < cluster.triangle_count; ++t) {
        const u32 packed = mesh.triangles[cluster.triangle_offset + t];
        for (u32 k = 0; k < 3; ++k) {
          const u32 local = (packed >> (8 * k)) & 0xffu;
          const Vec3 p =
              mesh.vertices[cluster.vertex_offset + (local < cluster.vertex_count ? local : 0)];
          leaf_lo = p.y < leaf_lo ? p.y : leaf_lo;
          leaf_hi = p.y > leaf_hi ? p.y : leaf_hi;
          corners.push_back(p);
        }
      }
    }
    if (corners.empty()) continue;
    // Leaves, not pictures of leaves: a mesh whose alpha-tested triangles are large on average
    // (a far forest's impostors, merged cards) has no leaf area to count.
    f64 card_area = 0.0;
    for (u32 t = 0; t + 2 < corners.size(); t += 3) {
      card_area +=
          0.5 *
          static_cast<f64>(length(cross(corners[t + 1] - corners[t], corners[t + 2] - corners[t])));
    }
    if (card_area >
        static_cast<f64>(gfx::k_crown_card_max_m2) * static_cast<f64>(corners.size() / 3)) {
      continue;
    }
    // Leaves that stand too low for a crown — a sward, a shrub, a sapling, a fern: foliage with
    // no shade of its own to divide by (stage 8, below).
    if (leaf_hi - leaf_lo < gfx::k_crown_min_height_m) {
      low[m] = 1;
      continue;
    }
    gfx::CrownPriorStats stats;
    const u32 at = static_cast<u32>(grids.size());
    gfx::build_crown_prior({corners.data(), corners.size()}, lo, hi, records[m], grids, &stats);
    records[m].offset = meshes * static_cast<u32>(sizeof(gfx::CrownPriorDesc)) + at;
    ++out.crowns;
    out.leaf_triangles += static_cast<u32>(corners.size() / 3);
    out.leaf_area_m2 += stats.leaf_area_m2;
    out.leaf_volume_m3 += stats.leaf_volume_m3;
    out.min_up = stats.min_up < out.min_up ? stats.min_up : out.min_up;
    ENGINE_LOG_INFO(log_renderer, "crown prior", log::field("mesh", m),
                    log::field("leaf_triangles", static_cast<u32>(corners.size() / 3)),
                    log::field("leaf_area_m2", stats.leaf_area_m2),
                    log::field("leaf_volume_m3", stats.leaf_volume_m3),
                    log::field("mean_up", static_cast<f64>(stats.mean_up)),
                    log::field("min_up", static_cast<f64>(stats.min_up)),
                    log::field("dims", records[m].dims));
  }
  out.meshes = meshes;
  const u32 head = meshes * static_cast<u32>(sizeof(gfx::CrownPriorDesc));
  for (u32 m = 0; m < meshes; ++m) {
    if (!is_form(m) || data.form_source[m] >= meshes) continue;
    records[m] = records[data.form_source[m]];
    if (records[m].offset != gfx::k_no_crown) ++out.forms;
  }
  // **A mesh of low leaves reads one cell of no shade** (stage 8; renderer.md, "Indirect light",
  // "Stage 8"). A record makes a mesh's alpha-tested surfaces foliage: keyed by no face, read
  // one coarse level. A sward, a shrub, a sapling and a fern are foliage in every way that
  // matters there — cards facing every way at every depth, own cells that never fill, and so a
  // walk over face-keyed levels that never ends — and have no shade of their own to divide by
  // (their own solvers paint it). Their record is a grid of one cell whose two means are one
  // (255: everything let through) with no extent (`inv_cell` 0, so every point reads that cell):
  // the shader's lookup returns one for them with no rule of its own, which keeps the resolve's
  // modules the bytes they were.
  //
  // **Pictures of leaves get none**: a far forest's impostor and a form's merged cards are seen
  // from where their level is the ladder's last, so their walk is one level already, and a
  // record would add the grid's lookup to every one of their pixels and save nothing (the first
  // build of this stage gave every mesh the cell: the forest's resolve was 0.4 ms dearer for it
  // and the water's reflections 0.14).
  const u32 unit_at = static_cast<u32>(grids.size());
  for (u32 k = 0; k < 4; ++k)
    grids.push_back(static_cast<u8>(255));
  gfx::CrownPriorDesc unit;
  unit.offset = head + unit_at;
  unit.dims = 1u | (1u << 8) | (1u << 16);
  for (u32 m = 0; m < meshes; ++m) {
    if (low[m] == 0 || records[m].offset != gfx::k_no_crown) continue;
    records[m] = unit;
    ++out.plain;
  }
  if (out.crowns == 0 && out.plain == 0) return;
  out.bytes.resize(head + static_cast<u32>(grids.size()));
  std::memcpy(out.bytes.data(), records.data(), head);
  std::memcpy(out.bytes.data() + head, grids.data(), grids.size());
}

bool upload_crown_priors(const gfx::Device& device, const SceneData& data,
                         gfx::BufferResource& buffer, u32& count, std::string* error) {
  count = 0;
  CrownPriorTable table;
  build_crown_priors(data, table);
  if (table.crowns == 0 && table.plain == 0) return true;
  if (!gfx::upload_buffer(device, table.bytes.data(), table.bytes.size(),
                          gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, buffer,
                          error)) {
    return false;
  }
  count = table.meshes;
  ENGINE_LOG_INFO(log_renderer, "crown priors", log::field("meshes", table.meshes),
                  log::field("crowns", table.crowns), log::field("forms", table.forms),
                  log::field("plain", table.plain),
                  log::field("leaf_triangles", table.leaf_triangles),
                  log::field("leaf_area_m2", table.leaf_area_m2),
                  log::field("leaf_volume_m3", table.leaf_volume_m3),
                  log::field("min_up", static_cast<f64>(table.min_up)),
                  log::field("bytes", static_cast<u64>(table.bytes.size())));
  return true;
}

}  // namespace engine::renderer
