#include <core/hash/hash.h>
#include <domain/flora/companion.h>
#include <domain/flora/impostor.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>
#include <domain/wetland/wetland.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>

#include <algorithm>
#include <cmath>

namespace engine::wetland {
namespace {
flora::TreeLevel cards(assets::MeshData& mesh, flora::Motion& motion) {
  Vec3 lo = mesh.positions[0], hi = lo;
  for (const auto p : mesh.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const Vec3 centre = (lo + hi) * .5f, extent = hi - lo;
  flora::TreeLevel level;
  level.kind = flora::ImpostorKind::tree;
  level.tile_px = 128;
  level.columns = 2;
  level.rows = 2;
  level.groups = 3;
  level.parallax_m = std::max({extent.x, extent.y, extent.z}) * .5f;
  level.texel_m = level.parallax_m / 126;
  level.error = level.parallax_m + level.texel_m;
  level.feature_m = std::max(.02f, std::min({extent.x, extent.y, extent.z}) * .25f);
  flora::KitWriter writer{mesh, motion};
  const u32 part = flora::kit_begin_part(writer, {0, 0, 0}, {0, 1, 0}, std::max(.05f, hi.y),
                                         geometry::k_motion_no_parent, 0, 0, 0);
  for (u32 plane = 0; plane < 3; ++plane) {
    flora::BakeJob job;
    job.tile = job.card_tile = plane;
    auto& view = job.view;
    view.origin = centre;
    if (plane == 0) {
      view.u_min = lo.x - centre.x;
      view.u_max = hi.x - centre.x;
      view.v_min = lo.y - centre.y;
      view.v_max = hi.y - centre.y;
    } else if (plane == 1) {
      view.u_axis = {0, 0, 1};
      view.normal = {-1, 0, 0};
      view.u_min = lo.z - centre.z;
      view.u_max = hi.z - centre.z;
      view.v_min = lo.y - centre.y;
      view.v_max = hi.y - centre.y;
    } else {
      view.u_axis = {1, 0, 0};
      view.v_axis = {0, 0, 1};
      view.normal = {0, -1, 0};
      view.u_min = lo.x - centre.x;
      view.u_max = hi.x - centre.x;
      view.v_min = lo.z - centre.z;
      view.v_max = hi.z - centre.z;
    }
    job.card = view;
    job.ranges.push_back({0, mesh.indices.size()});
    const f32 um = (view.u_max - view.u_min) / 126, vm = (view.v_max - view.v_min) / 126;
    view.u_min -= um;
    view.u_max += um;
    view.v_min -= vm;
    view.v_max += vm;
    const u32 first = mesh.positions.size();
    for (u32 corner = 0; corner < 4; ++corner) {
      const bool right = corner == 1 || corner == 2, top = corner >= 2;
      const auto& v = job.card;
      const Vec3 p =
          v.origin + v.u_axis * (right ? v.u_max : v.u_min) + v.v_axis * (top ? v.v_max : v.v_min);
      mesh.positions.push_back(p);
      mesh.normals.push_back(v.normal);
      mesh.uvs.push_back({(static_cast<f32>(plane % 2) * 128 + (right ? 127.0f : 1.0f)) / 256,
                          (static_cast<f32>(plane / 2) * 128 + (top ? 127.0f : 1.0f)) / 256});
      motion.vertex_part.push_back(part);
      // Far cards are a rest-pose representation: no fictitious rigid leaf oscillation.
      motion.vertex_weight.push_back(0);
    }
    for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
      level.indices.push_back(first + i);
    level.jobs.push_back(std::move(job));
  }
  return level;
}
void near_materials(const Plant& p, const assets::MeshData& mesh,
                    const assets::MeshGeneratorInputs& in, geometry::ClusterFileData& out) {
  for (u32 i = 0; i < mesh.materials.size(); ++i) {
    const auto& m = mesh.materials[i];
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = 0;
    material.roughness = m.roughness;
    material.alpha = geometry::encode_alpha_word(m.alpha_mode, true, m.alpha_cutoff);
    out.materials.push_back(material);
    out.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
    const std::string role = p.species + (i == 0 ? "/dead" : "/leaf");
    if (const auto* set = materials::resolve_set(nullptr, in.materials, role))
      materials::apply_set(*set, i == 0 ? materials::UvLayout::repeats : materials::UvLayout::card,
                           out, i);
  }
}
bool bake(const assets::MeshData& mesh, const flora::TreeLevel& level,
          const assets::MeshGeneratorInputs& in, geometry::ClusterFileData& out, f32& coverage,
          std::string& error) {
  flora::TreeLevels chain;
  chain.leaf_index_end = mesh.indices.size();
  chain.leaf_first_index = chain.leaf_index_end;
  for (const auto& p : mesh.primitives)
    if (p.material == 1) chain.leaf_first_index = p.first_index;
  flora::BakeLook look;
  look.bark = out.materials[0].base_color;
  look.leaf = out.materials[1].base_color;
  look.bark_roughness = out.materials[0].roughness;
  look.leaf_roughness = out.materials[1].roughness;
  image::Image images[2][3];
  for (u32 m = 0; m < 2; ++m) {
    const auto& material = out.materials[m];
    const i32 slots[] = {material.base_color_image, material.normal_image,
                         geometry::decode_optional_image(material.metallic_roughness_image)};
    for (u32 kind = 0; kind < 3; ++kind) {
      const i32 slot = slots[kind];
      if (slot < 0 || static_cast<u32>(slot) >= out.image_paths.size() ||
          out.image_paths[slot].empty())
        continue;
      if (!in.hosts || !in.hosts->read_image) {
        error = "wetland pack bake requires a read_image host";
        return false;
      }
      Vector<u8> bytes;
      if (!in.hosts->read_image(in.hosts->context, out.image_paths[slot], bytes, error) ||
          !image::decode_image(bytes, images[m][kind], 4, &error))
        return false;
      if (kind == 0) (m ? look.leaf_image : look.bark_image) = &images[m][kind];
      if (kind == 1) (m ? look.leaf_normal_image : look.bark_normal_image) = &images[m][kind];
      if (kind == 2) (m ? look.leaf_roughness_image : look.bark_roughness_image) = &images[m][kind];
    }
    (m ? look.leaf_normal_scale : look.bark_normal_scale) = material.normal_scale;
  }
  look.leaf_outline = geometry::alpha_word_mode(out.materials[1].alpha) != 0;
  look.leaf_cutoff = geometry::alpha_word_cutoff(out.materials[1].alpha);
  flora::BakedAtlas atlas;
  flora::bake_level(mesh, chain, level, look, atlas);
  const f32 difference = flora::measure_level(mesh, chain, level, look, atlas);
  // Three planes are a silhouette approximation. Delay until the whole stand is
  // about ten pixels high at the default five-pixel foliage threshold.
  coverage = std::max(level.parallax_m, level.feature_m * difference);
  const i32 colour = static_cast<i32>(out.image_paths.size());
  out.images.resize(out.image_paths.size());
  const Vector<u8>* pixels[] = {&atlas.colour, &atlas.normal, &atlas.roughness};
  for (u32 i = 0; i < (atlas.roughness.empty() ? 2u : 3u); ++i) {
    geometry::ClusterImage image;
    image.mime_type = "image/png";
    if (!image::encode_png(atlas.width, atlas.height, 4, *pixels[i], image.bytes)) {
      error = "wetland atlas failed to encode";
      return false;
    }
    out.image_paths.push_back({});
    out.images.push_back(std::move(image));
  }
  geometry::ClusterFileMaterial material;
  material.base_color = {1, 1, 1, 1};
  material.metallic = 0;
  material.roughness = atlas.roughness.empty() ? .7f : 1;
  material.base_color_image = colour;
  material.normal_image = colour + 1;
  material.metallic_roughness_image =
      geometry::encode_optional_image(atlas.roughness.empty() ? -1 : colour + 2);
  material.alpha = geometry::encode_alpha_word(assets::k_alpha_mask, true, .5f);
  out.materials.push_back(material);
  assets::Material defaults;
  out.material_sampling.push_back(geometry::encode_material_sampling(defaults.sampling, 1));
  out.impostors.push_back({static_cast<u32>(level.kind), 0, 2, 3, 128, 2, 2, level.error,
                           level.parallax_m, level.texel_m, coverage, difference});
  texture::fill_cluster_texture_records(out);
  return true;
}
bool identity(const Plant& p, const assets::MeshGeneratorInputs& in, u64& out, std::string& error) {
  out = source_hash(p);
  for (const auto* suffix : {"/dead", "/leaf"}) {
    const auto* set = materials::find_role(in.materials, p.species + suffix);
    if (!set) continue;
    for (const auto* path : {&set->base_color, &set->normal, &set->roughness}) {
      if (path->empty()) continue;
      if (!in.hosts || (!in.hosts->hash_image && !in.hosts->read_image)) {
        error = "wetland pack bake requires an image identity host";
        return false;
      }
      u64 h = 0;
      if (in.hosts->hash_image) {
        if (!in.hosts->hash_image(in.hosts->context, *path, h, error)) return false;
      } else {
        Vector<u8> bytes;
        if (!in.hosts->read_image(in.hosts->context, *path, bytes, error)) return false;
        h = hash_bytes(bytes.data(), bytes.size());
      }
      out = hash_combine(out, h);
    }
  }
  return true;
}
}  // namespace
bool build_container(const Plant& p, const assets::MeshGeneratorInputs& in,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error) {
  out = {};
  assets::MeshData mesh;
  flora::Motion motion;
  if (!solve(p, mesh, motion, report, error)) return false;
  auto level = cards(mesh, motion);
  geometry::ClusterFileData result;
  near_materials(p, mesh, in, result);
  f32 coverage = 0;
  if (!bake(mesh, level, in, result, coverage, error)) return false;
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> materials, authored;
  const auto attributes = assets::attribute_source(mesh);
  for (const auto& primitive : mesh.primitives) {
    geometry::ClusterLodMesh part;
    geometry::ClusterLodOptions options = lod;
    options.normal_cones = false;
    // Preserve thin organs until the coverage-tested card transition. The first authored
    // rung caps generic decimation at a micrometre; it carries the complete source cut.
    const geometry::ClusterLodLevel levels[] = {
        {{mesh.indices.data() + primitive.first_index, primitive.index_count}, 1e-6f},
        {parts.empty() ? std::span<const u32>(level.indices) : std::span<const u32>{},
         level.error}};
    if (!geometry::build_cluster_lod_levels(
            mesh.positions, {mesh.indices.data() + primitive.first_index, primitive.index_count},
            levels, options, part, &error, attributes))
      return false;
    authored.push_back(parts.empty() ? part.level_cluster_counts.size() - 1 : ~0u);
    materials.push_back(static_cast<u32>(primitive.material));
    parts.push_back(std::move(part));
  }
  Vec4 sphere{};
  for (const auto& part : parts)
    sphere = geometry::authored_lod_sphere(part, level.error, sphere);
  for (auto& part : parts)
    if (!geometry::widen_lod_spheres(part, level.error, sphere, &error)) return false;
  Vector<u32> owners;
  if (!geometry::merge_cluster_lod(parts, result.mesh, &owners, &error)) return false;
  for (u32 i = 0; i < owners.size(); ++i)
    owners[i] = result.mesh.lod[i].level >= authored[owners[i]] ? 2 : materials[owners[i]];
  if (page_bytes) {
    geometry::ClusterPagesOptions options;
    options.page_bytes = page_bytes;
    Vector<u32> order;
    if (!geometry::build_cluster_pages(result.mesh, options, result.pages, &error, &order))
      return false;
    geometry::permute_cluster_array(order, owners);
  }
  result.cluster_material = std::move(owners);
  for (const auto& l : result.mesh.lod) {
    const f32 own = l.own_error >= level.error ? coverage : geometry::k_lod_terminal_error;
    const f32 parent =
        l.parent_error >= level.error && l.parent_error < geometry::k_lod_terminal_error
            ? coverage
            : geometry::k_lod_terminal_error;
    result.lod_coverage.push_back(geometry::pack_lod_coverage(own, parent));
  }
  if (!flora::motion_records(motion, result.mesh, result.motion_vertices)) {
    error = "wetland motion records do not cover cluster vertices";
    return false;
  }
  result.motion_parts = std::move(motion.parts);
  result.structural_parts = std::move(motion.structure);
  if (!identity(p, in, result.source_hash, error)) return false;
  result.build_key = assets::generated_mesh_build_key(result.source_hash, in, lod, page_bytes);
  result.generator = "wetland";
  result.generator_kind = p.species;
  result.generator_params = canonical_json(p);
  report.triangles += 6;
  report.parts = result.motion_parts.size();
  out = std::move(result);
  return true;
}
namespace {
std::string_view text_of(std::span<const u8> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool hash_recipe(std::span<const u8> bytes, u64& out, std::string& error) {
  Plant p;
  if (!read_file(text_of(bytes), p, error)) return false;
  out = source_hash(p);
  return true;
}
bool hash_with(std::span<const u8> bytes, const assets::MeshGeneratorInputs& in, u64& out,
               std::string& error) {
  Plant p;
  return read_file(text_of(bytes), p, error) && identity(p, in, out, error);
}
bool build_recipe(std::span<const u8> bytes, const assets::MeshGeneratorInputs& in,
                  const geometry::ClusterLodOptions& lod, u32 page, geometry::ClusterFileData& out,
                  std::string& error) {
  Plant p;
  Report report;
  return read_file(text_of(bytes), p, error) &&
         build_container(p, in, lod, page, out, report, error);
}
const assets::MeshGeneratorDesc k_generator{.name = "wetland",
                                            .source_hash = hash_recipe,
                                            .build = build_recipe,
                                            .source_hash_with = hash_with};
const assets::MeshGeneratorRegistrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::wetland
