#include <core/hash/hash.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/material_sampling.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/crown_priors.h>

#include <cmath>
#include <cstring>
#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

// **A card's leaf, measured from its own mask** (T146; renderer.md, "Stage 7"). A material's
// base colour alpha decoded once, on the CPU, for the triangles that sample it; `ok` false when
// the material has no image (its alpha is the factor's alone) or the image cannot be read.
struct MaskImage {
  bool tried = false;
  bool ok = false;
  image::Image alpha;  // one channel
  // The share of the whole image the test keeps: what a triangle counts when the mesh's UVs are
  // gone (a host that released the uploaded streams before the prior was built, page_source.h).
  f32 mean = 1.0f;
};

bool decode_mask(const SourceMesh& source, const assets::Material& material, MaskImage& out) {
  out.tried = true;
  const i32 index = material.base_color_image;
  if (index < 0 || static_cast<u32>(index) >= source.data.images.size()) return false;
  const assets::ImageRef& ref = source.data.images[static_cast<u32>(index)];
  // Four channels and keep A: a one-channel request is the decoder's grey from RGB, not alpha.
  image::Image rgba;
  // W51-3: a mesh whose bytes the load let go reads them from its container, once a mask.
  Vector<u8> read_again;
  const std::span<const u8> bytes =
      source_image_bytes(source, static_cast<u32>(index), read_again, nullptr);
  if (!bytes.empty()) {
    out.ok = image::decode_image(bytes, rgba, 4);
  } else if (!ref.uri.empty()) {
    const std::string file = source.image_dir.empty() || io::is_absolute_path(ref.uri)
                                 ? ref.uri
                                 : io::join_path(source.image_dir, ref.uri);
    out.ok = image::read_image(file, rgba, 4) == io::Status::Ok;
  }
  if (!out.ok) return false;
  const u32 texels = rgba.width * rgba.height;
  out.alpha.width = rgba.width;
  out.alpha.height = rgba.height;
  out.alpha.channels = 1;
  out.alpha.pixels.resize(texels);
  u32 kept = 0;
  for (u32 t = 0; t < texels; ++t) {
    out.alpha.pixels[t] = rgba.pixels[t * 4 + 3];
    if (material.base_color.w * static_cast<f32>(out.alpha.pixels[t]) / 255.0f >=
        material.alpha_cutoff)
      ++kept;
  }
  out.mean = texels == 0 ? 1.0f : static_cast<f32>(kept) / static_cast<f32>(texels);
  return true;
}

// Which decode a material's mask is: its base colour image (the resolved file, or the embedded
// bytes) under its test (the factor's alpha and the cutoff). A forest's thousands of tree meshes
// share a handful of leaf images, so a mask is decoded once a load and not once a mesh: measured
// at about 18 ms a 2K image, one a mesh was a minute of a 3,000-tree load (T139, second leg).
u64 mask_key(const SourceMesh& source, const assets::Material& material) {
  const i32 index = material.base_color_image;
  if (index < 0 || static_cast<u32>(index) >= source.data.images.size()) return 0;
  const assets::ImageRef& ref = source.data.images[static_cast<u32>(index)];
  u64 key = 0;
  if (!ref.bytes.empty()) {
    key = hash_bytes(ref.bytes.data(), ref.bytes.size());
  } else if (source.image_bytes_released && ref.uri.empty() &&
             source.textures[static_cast<u32>(index)].source_hash != 0) {
    // W51-3: the bytes went after the texture pass, which recorded this same hash of them.
    key = source.textures[static_cast<u32>(index)].source_hash;
  } else if (!ref.uri.empty()) {
    const std::string file = source.image_dir.empty() || io::is_absolute_path(ref.uri)
                                 ? ref.uri
                                 : io::join_path(source.image_dir, ref.uri);
    key = hash_bytes(file.data(), file.size());
  } else {
    return 0;
  }
  u32 factor = 0;
  u32 cutoff = 0;
  std::memcpy(&factor, &material.base_color.w, sizeof(factor));
  std::memcpy(&cutoff, &material.alpha_cutoff, sizeof(cutoff));
  return hash_combine(hash_combine(key, u64{factor}), u64{cutoff}) | 1u;  // 0 is "no image"
}

// The share of one triangle its mask keeps: the material's test (base colour factor's alpha times
// the texel's, against the cutoff, at level 0, through the slot's UV transform and repeat wrap)
// asked at the centroids of the triangle's 4 x 4 equal-area sub-triangles — the alpha coverage
// bake's sampling (alpha_coverage.slang), on the CPU because the prior is built from the CPU's
// copy of the mesh.
f32 triangle_fill(const assets::Material& material, const MaskImage& mask, Vec2 uv0, Vec2 uv1,
                  Vec2 uv2) {
  const f32 factor = material.base_color.w;
  if (!mask.ok) return factor >= material.alpha_cutoff ? 1.0f : 0.0f;
  const geometry::TextureTransform& transform = material.sampling[0].transform;
  const f64 c = transform.rotation == 0.0f ? 1.0 : det::cos(static_cast<f64>(transform.rotation));
  const f64 s = transform.rotation == 0.0f ? 0.0 : det::sin(static_cast<f64>(transform.rotation));
  constexpr u32 n = 4;
  u32 kept = 0;
  const auto keeps = [&](f32 a, f32 b) {
    const f32 u0 = uv0.x + a * (uv1.x - uv0.x) + b * (uv2.x - uv0.x);
    const f32 v0 = uv0.y + a * (uv1.y - uv0.y) + b * (uv2.y - uv0.y);
    // KHR_texture_transform: scale, then rotate, then offset.
    const f64 su = static_cast<f64>(u0) * static_cast<f64>(transform.scale.x);
    const f64 sv = static_cast<f64>(v0) * static_cast<f64>(transform.scale.y);
    f64 u = c * su + s * sv + static_cast<f64>(transform.offset.x);
    f64 v = -s * su + c * sv + static_cast<f64>(transform.offset.y);
    u -= std::floor(u);
    v -= std::floor(v);
    u32 x = static_cast<u32>(u * static_cast<f64>(mask.alpha.width));
    u32 y = static_cast<u32>(v * static_cast<f64>(mask.alpha.height));
    x = x < mask.alpha.width ? x : mask.alpha.width - 1;
    y = y < mask.alpha.height ? y : mask.alpha.height - 1;
    const f32 alpha =
        factor * static_cast<f32>(mask.alpha.pixels[y * mask.alpha.width + x]) / 255.0f;
    return alpha >= material.alpha_cutoff;
  };
  for (u32 i = 0; i < n; ++i) {
    for (u32 j = 0; i + j < n; ++j) {
      const f32 fi = static_cast<f32>(i);
      const f32 fj = static_cast<f32>(j);
      if (keeps((fi + 1.0f / 3.0f) / n, (fj + 1.0f / 3.0f) / n)) ++kept;
      if (i + j + 1 < n && keeps((fi + 2.0f / 3.0f) / n, (fj + 2.0f / 3.0f) / n)) ++kept;
    }
  }
  return static_cast<f32>(kept) / static_cast<f32>(n * n);
}

}  // namespace

void build_crown_priors(const SceneData& data, CrownPriorTable& out) {
  out = CrownPriorTable{};
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  const u32 meshes = static_cast<u32>(data.parts.size());
  if (meshes == 0 || mesh.vertices.empty() || data.sources.size() != data.parts.size()) return;
  Vector<gfx::CrownPriorDesc> records;
  records.resize(meshes);
  Vector<u8> grids;
  Vector<Vec3> corners;
  Vector<f32> fill;  // one a triangle of `corners`: the share its mask keeps
  // The load's masks, one a distinct image and test (`mask_key`); entry 0 is the material with no
  // image, whose test is its factor's alone. `mask_of` takes a mesh's material to its entry.
  Vector<u64> mask_keys;
  Vector<MaskImage> masks;
  mask_keys.push_back(0);
  masks.push_back(MaskImage{});
  masks[0].tried = true;
  Vector<u32> mask_of;
  // What measuring the masks costs the load: one CPU decode a distinct mask, timed, so a world's
  // count of leaf images says what its load pays (static-asset-world.md, "Bought foliage").
  u32 decodes = 0;
  i64 decode_ns = 0;
  const bool has_uvs = mesh.attributes.size() == mesh.vertices.size();
  if (!has_uvs && meshes > 0) {
    ENGINE_LOG_INFO(log_renderer, "crown prior measured by image means",
                    log::field("why", "the mesh's UVs were released before the prior was built"));
  }
  // A distance form (`SceneData::form_source`) is a cut of its source's own clusters in its
  // source's mesh space: it reads its source's grid, made from the source's leaves, and none of
  // its own (a form's finest cards are merged ones).
  const auto is_form = [&](u32 m) noexcept {
    return m < data.form_source.size() && data.form_source[m] != m;
  };
  // A far forest's tile (far_forest.h) is many trees' impostors merged into one mesh over a world
  // tile: not one crown, so it has no grid. It was kept out by the card-size cap before T146.
  Vector<u8> far_tile;
  far_tile.resize(meshes, u8{0});
  for (const FarForestTile& tile : data.far_forest.tiles) {
    if (tile.mesh < meshes) far_tile[tile.mesh] = 1;
  }
  for (u32 m = 0; m < meshes; ++m) {
    if (is_form(m) || far_tile[m] != 0) continue;
    const geometry::ClusterMeshPart& part = data.parts[m];
    const SourceMesh& source = data.sources[m];
    corners.clear();
    fill.clear();
    mask_of.clear();
    mask_of.resize(source.data.materials.size(), ~0u);
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
      const assets::Material& leaf = source.data.materials[static_cast<u32>(material)];
      u32& entry = mask_of[static_cast<u32>(material)];
      if (entry == ~0u) {
        const u64 key = mask_key(source, leaf);
        entry = 0;
        for (u32 k = 1; key != 0 && k < mask_keys.size(); ++k) {
          if (mask_keys[k] == key) {
            entry = k;
            break;
          }
        }
        if (key != 0 && entry == 0) {
          entry = static_cast<u32>(masks.size());
          mask_keys.push_back(key);
          masks.push_back(MaskImage{});
          const i64 start_ns = time::monotonic_ns();
          decode_mask(source, leaf, masks[entry]);
          decode_ns += time::monotonic_ns() - start_ns;
          ++decodes;
          // With no UVs only the mean is read: the texels go now, not at the end of the load.
          if (!has_uvs) masks[entry].alpha = image::Image{};
        }
        if (!masks[entry].ok) {
          ENGINE_LOG_INFO(log_renderer, "crown prior mask read from the factor alone",
                          log::field("mesh", m), log::field("material", material));
        }
      }
      const MaskImage& mask = masks[entry];
      for (u32 t = 0; t < cluster.triangle_count; ++t) {
        const u32 packed = mesh.triangles[cluster.triangle_offset + t];
        Vec2 uv[3];
        for (u32 k = 0; k < 3; ++k) {
          const u32 local = (packed >> (8 * k)) & 0xffu;
          const u32 v = cluster.vertex_offset + (local < cluster.vertex_count ? local : 0);
          const Vec3 p = mesh.vertices[v];
          uv[k] = has_uvs ? geometry::decode_half2(mesh.attributes[v].uv_half2) : Vec2{};
          leaf_lo = p.y < leaf_lo ? p.y : leaf_lo;
          leaf_hi = p.y > leaf_hi ? p.y : leaf_hi;
          corners.push_back(p);
        }
        // With no UVs the triangle counts its image's mean: right for a card that maps its whole
        // image, an estimate for an atlas (the T138 order builds the prior before they go).
        const f32 factor_only = leaf.base_color.w >= leaf.alpha_cutoff ? 1.0f : 0.0f;
        fill.push_back(has_uvs   ? triangle_fill(leaf, mask, uv[0], uv[1], uv[2])
                       : mask.ok ? mask.mean
                                 : factor_only);
      }
    }
    if (corners.empty()) continue;
    if (leaf_hi - leaf_lo < gfx::k_crown_min_height_m) {
      // Said, not silent: a bought sapling of 1.3 m is a tree to its owner and gets no crown here.
      ENGINE_LOG_INFO(log_renderer, "crown prior skipped: leaves under the minimum height",
                      log::field("mesh", m),
                      log::field("leaf_height_m", static_cast<f64>(leaf_hi - leaf_lo)),
                      log::field("minimum_m", static_cast<f64>(gfx::k_crown_min_height_m)));
      continue;
    }
    // Leaf, not card: each triangle counts the share of it its own mask keeps (T146), so a solver's
    // 2 cm leaf card and a bought tree's 1.2 m branch card both count the leaf they show, and no
    // card is refused for its size.
    gfx::CrownPriorStats stats;
    const u32 at = static_cast<u32>(grids.size());
    gfx::build_crown_prior({corners.data(), corners.size()}, {fill.data(), fill.size()}, lo, hi,
                           records[m], grids, &stats);
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
  out.mask_decodes = decodes;
  if (decodes > 0) {
    ENGINE_LOG_INFO(log_renderer, "crown prior masks decoded", log::field("decodes", decodes),
                    log::field("ms", static_cast<f64>(decode_ns) / 1.0e6));
  }
  if (out.crowns == 0) return;
  for (u32 m = 0; m < meshes; ++m) {
    if (!is_form(m) || data.form_source[m] >= meshes) continue;
    records[m] = records[data.form_source[m]];
    if (records[m].offset != gfx::k_no_crown) ++out.forms;
  }
  const u32 head = meshes * static_cast<u32>(sizeof(gfx::CrownPriorDesc));
  out.bytes.resize(head + static_cast<u32>(grids.size()));
  std::memcpy(out.bytes.data(), records.data(), head);
  std::memcpy(out.bytes.data() + head, grids.data(), grids.size());
}

bool upload_crown_priors(const gfx::Device& device, const CrownPriorTable& table,
                         gfx::BufferResource& buffer, u32& count, std::string* error) {
  count = 0;
  if (table.crowns == 0) return true;
  if (!gfx::upload_buffer(device, table.bytes.data(), table.bytes.size(),
                          gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, buffer,
                          error)) {
    return false;
  }
  count = table.meshes;
  ENGINE_LOG_INFO(log_renderer, "crown priors", log::field("meshes", table.meshes),
                  log::field("crowns", table.crowns), log::field("forms", table.forms),
                  log::field("leaf_triangles", table.leaf_triangles),
                  log::field("leaf_area_m2", table.leaf_area_m2),
                  log::field("leaf_volume_m3", table.leaf_volume_m3),
                  log::field("min_up", static_cast<f64>(table.min_up)),
                  log::field("bytes", static_cast<u64>(table.bytes.size())));
  return true;
}

// A mesh's leaves for its sound (scene.h `MeshLeaves`; S112): the leaves `build_crown_priors` takes
// (level 0, an alpha-tested material), their cards' one-sided area and their height. No mask is
// decoded and no grid built: one pass over the triangles, so every load can afford it.
void measure_mesh_leaves(const SceneData& data, Vector<MeshLeaves>& out) {
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  const u32 meshes = static_cast<u32>(data.parts.size());
  out.clear();
  out.resize(meshes);
  if (meshes == 0 || mesh.vertices.empty() || data.sources.size() != data.parts.size()) return;
  for (u32 m = 0; m < meshes; ++m) {
    const geometry::ClusterMeshPart& part = data.parts[m];
    const SourceMesh& source = data.sources[m];
    f64 area = 0.0;
    f32 lo = 1e30f;
    f32 hi = -1e30f;
    for (u32 i = 0; i < part.cluster_count; ++i) {
      const u32 c = part.first_cluster + i;
      if (c >= mesh.clusters.size() || c >= data.lod.lod.size() || data.lod.lod[c].level != 0)
        continue;
      if (i >= source.part_of_cluster.size()) continue;
      const u32 part_index = source.part_of_cluster[i];
      if (part_index >= source.part_material.size()) continue;
      const i32 material = source.part_material[part_index];
      if (material < 0 || static_cast<u32>(material) >= source.data.materials.size()) continue;
      // Foliage: an alpha-tested card (a bought tree's, or a generated one's with an image), or a
      // two-sided material that lets light through (the flora's own leaves without an image).
      const assets::Material& leaf = source.data.materials[static_cast<u32>(material)];
      if (leaf.alpha_mode != assets::k_alpha_mask && !(leaf.double_sided && leaf.transmission > 0.0f))
        continue;
      const geometry::ClusterDesc& cluster = mesh.clusters[c];
      if (static_cast<usize>(cluster.triangle_offset) + cluster.triangle_count >
              mesh.triangles.size() ||
          static_cast<usize>(cluster.vertex_offset) + cluster.vertex_count > mesh.vertices.size())
        continue;
      for (u32 t = 0; t < cluster.triangle_count; ++t) {
        const u32 packed = mesh.triangles[cluster.triangle_offset + t];
        Vec3 p[3];
        for (u32 k = 0; k < 3; ++k) {
          const u32 local = (packed >> (8 * k)) & 0xffu;
          p[k] = mesh.vertices[cluster.vertex_offset + (local < cluster.vertex_count ? local : 0)];
          lo = p[k].y < lo ? p[k].y : lo;
          hi = p[k].y > hi ? p[k].y : hi;
        }
        area += 0.5 * static_cast<f64>(length(cross(p[1] - p[0], p[2] - p[0])));
      }
    }
    if (area > 0.0) out[m] = MeshLeaves{static_cast<f32>(area), lo, hi};
  }
  // A distance form is a cut of its source's clusters: it sounds as its source's leaves.
  for (u32 m = 0; m < meshes && m < data.form_source.size(); ++m) {
    const u32 s = data.form_source[m];
    if (s != m && s < meshes) out[m] = out[s];
  }
}

}  // namespace engine::renderer
