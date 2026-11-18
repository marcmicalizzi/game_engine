// The recipe file and the mesh generator `structure` (docs/subsystems/structures.md, "The file
// and the container"): each surface's triangles clustered as a part and the DAGs merged into one
// mesh of a material a surface, each its role's set from the host's pack through the tile contract.

#include "emit.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/materials/materials.h>
#include <domain/structures/structures.h>
#include <domain/texture/material_textures.h>

namespace engine::structures {

bool read_file(std::string_view text, structure::Recipe& out, std::string& error) {
  JsonValue value;
  const auto parsed = parse_json(text, value);
  if (!parsed.ok) {
    error = "structure recipe: " + std::string(parsed.message);
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx) || !ctx.ok()) {
    error =
        ctx.diagnostics.empty()
            ? std::string("structure recipe: invalid engine.structure.Recipe")
            : "structure recipe: " + ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message;
    return false;
  }
  std::string why;
  if (!validate(out.description, why)) {
    error = "structure recipe: " + why;
    return false;
  }
  return true;
}

std::string canonical_json(const structure::Recipe& recipe) {
  return write_json(schema::to_json(recipe), {.pretty = false});
}

u64 source_hash(const structure::Recipe& recipe) {
  const std::string text = canonical_json(recipe);
  u64 hash = hash_combine(hash_bytes(text.data(), text.size()), k_structure_version);
  // A description that hangs a leaf has a container with section 53 since S91, so an entry built
  // before it, which hands a host no doors, is rebuilt; every description with none keeps its key.
  // A tag rather than a bump of `k_structure_version`, which would move every structure's key.
  for (const structure::Opening& o : recipe.description.openings) {
    if (o.leaf != structure::LeafKind::none && o.kind != structure::OpeningKind::doorway) {
      hash = hash_combine(hash, u64{0x444f4f5253303533ull});  // "DOORS053"
      break;
    }
  }
  return hash;
}

// **The leaves as doors** (structures.md, "Leaves and mullions"; geometry.md, "Doors"; S91): every
// leaf `leaf_places` hangs, as the container's section 53 records it, so a host places a door where
// the description put one and never reads it back out of the triangles. The place turns the leaf to
// the angle the description stands it at; the record keeps it shut and says which way it swings,
// undoing that turn with the same whole-degree sine and cosine (`turn_deg`), so every toolchain
// writes the same bytes.
void door_records(const structure::Description& d, Vector<geometry::ClusterFileDoor>& out) {
  out.clear();
  for (u32 i = 0; i < d.openings.size(); ++i) {
    const structure::Opening& o = d.openings[i];
    detail::LeafPlace places[2];
    const u32 leaves = detail::leaf_places(d, o, places);
    f64 sine = 0.0, cosine = 1.0;
    detail::turn_deg(o.leaf_open, sine, cosine);
    for (u32 k = 0; k < leaves; ++k) {
      const detail::LeafPlace& p = places[k];
      // At the angle a, u = cos a shut + sin a swing and v = sin a shut - cos a swing.
      const f64 shut_x = cosine * p.ux + sine * p.vx, shut_z = cosine * p.uz + sine * p.vz;
      const f64 swing_x = sine * p.ux - cosine * p.vx, swing_z = sine * p.uz - cosine * p.vz;
      geometry::ClusterFileDoor door;
      door.pivot = detail::metres(p.px, p.y0, p.pz);
      door.shut_x = static_cast<f32>(shut_x);
      door.shut_z = static_cast<f32>(shut_z);
      door.swing_x = static_cast<f32>(swing_x);
      door.swing_z = static_cast<f32>(swing_z);
      door.width_m = static_cast<f32>(p.width * detail::k_m);
      door.height_m = static_cast<f32>((p.y1 - p.y0) * detail::k_m);
      door.thickness_m = static_cast<f32>((p.boards + p.ledges) * detail::k_m);
      door.opening = i;
      door.open_deg = static_cast<i16>(o.leaf_open);
      door.flags = static_cast<u16>((p.door ? geometry::k_door_flag_door : 0u) |
                                    (leaves == 2 ? geometry::k_door_flag_paired : 0u));
      out.push_back(door);
    }
  }
}

bool build_container(const structure::Recipe& recipe, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error) {
  out = {};
  MeshPart parts[k_surfaces];
  if (!solve(recipe.description, recipe.scope, parts, report, error)) return false;
  geometry::ClusterLodOptions options = lod;
  // A floor, a ceiling and a roof's underside are sheets seen from one side only, but a wall's
  // face is not closed either: no cone may cull what the other side of a doorway shows.
  options.normal_cones = false;
  Vector<geometry::ClusterLodMesh> built;
  Vector<u32> surface_of;
  for (u32 s = 0; s < k_surfaces; ++s) {
    if (parts[s].indices.empty()) continue;
    geometry::ClusterLodMesh one;
    std::string build_error;
    geometry::AttributeSource attributes;
    attributes.normals = {parts[s].normals.data(), parts[s].normals.size()};
    attributes.uvs = {parts[s].uvs.data(), parts[s].uvs.size()};
    if (!geometry::build_cluster_lod({parts[s].positions.data(), parts[s].positions.size()},
                                     {parts[s].indices.data(), parts[s].indices.size()}, options,
                                     one, &build_error, attributes)) {
      error = "the cluster build refused the structure: " + build_error;
      return false;
    }
    built.push_back(std::move(one));
    surface_of.push_back(s);
  }
  Vector<u32> part_of_cluster;
  std::string merge_error;
  if (!geometry::merge_cluster_lod({built.data(), built.size()}, out.mesh, &part_of_cluster,
                                   &merge_error)) {
    error = "the structure's parts did not merge: " + merge_error;
    return false;
  }
  if (page_bytes > 0) {
    Vector<u32> source_of_cluster;
    geometry::ClusterPagesOptions paging;
    paging.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, paging, out.pages, &merge_error,
                                       &source_of_cluster)) {
      error = "the structure's pages were not laid out: " + merge_error;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }
  // A material a surface that has triangles, in the surfaces' order.
  out.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster)
    out.cluster_material.push_back(part);
  for (u32 m = 0; m < surface_of.size(); ++m) {
    const structure::Surface surface = static_cast<structure::Surface>(surface_of[m]);
    geometry::ClusterFileMaterial material;
    material.base_color = fallback_colour(surface);
    material.metallic = 0;
    material.roughness = 0.9f;
    material.alpha = geometry::encode_alpha_word(assets::k_alpha_opaque, false, 0.5f);
    out.materials.push_back(material);
    const assets::Material defaults;
    out.material_sampling.push_back(geometry::encode_material_sampling(defaults.sampling, 1.0f));
  }
  // **Materials by role** (structures.md, "Materials by role"): the description's role from the
  // host's pack; where the pack lacks it, the surface's fallback role; where it lacks that too,
  // the flat colour above (and the lookup says once which role is missing).
  for (u32 m = 0; m < surface_of.size(); ++m) {
    const structure::Surface surface = static_cast<structure::Surface>(surface_of[m]);
    std::string_view role = role_of(recipe.description, surface);
    if (surface == structure::Surface::roof)
      for (const structure::Roof& r : recipe.description.roofs)
        if (!r.role.empty()) role = r.role;
    const materials::MaterialSet* set =
        role.empty() ? nullptr : materials::find_role(inputs.materials, role);
    // A variant (`<base>_<variant>`: a limewash's tint, a thatch's age) the pack lacks is drawn
    // with its base, so a village described for one pack still has walls under another.
    const auto slash = role.find('/'), variant = role.rfind('_');
    if (set == nullptr && slash != std::string_view::npos && variant != std::string_view::npos &&
        variant > slash)
      set = materials::find_role(inputs.materials, role.substr(0, variant));
    const std::string_view behind = fallback_role(surface);
    if (set == nullptr && !behind.empty() && behind != role)
      set = materials::find_role(inputs.materials, behind);
    if (set == nullptr && !role.empty())
      set = materials::resolve_set(nullptr, inputs.materials, role);  // says once it is missing
    if (set != nullptr) materials::apply_set(*set, materials::UvLayout::metres, out, m);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  report.clusters = out.mesh.mesh.clusters.size();
  for (u32 c = 0; c < out.mesh.lod.size(); ++c)
    if (out.mesh.lod[c].parent_error == geometry::k_lod_terminal_error)
      report.root_triangles += out.mesh.mesh.clusters[c].triangle_count;
  door_records(recipe.description, out.doors);
  out.source_hash = source_hash(recipe);
  out.build_key = assets::generated_mesh_build_key(out.source_hash, inputs, lod, page_bytes);
  out.generator = "structure";
  out.generator_kind = recipe.description.style.empty() ? "building" : recipe.description.style;
  out.generator_params = canonical_json(recipe);
  return true;
}

namespace {
std::string_view text_of(std::span<const u8> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool hash_recipe(std::span<const u8> bytes, u64& out, std::string& error) {
  structure::Recipe recipe;
  if (!read_file(text_of(bytes), recipe, error)) return false;
  out = source_hash(recipe);
  return true;
}
bool build_recipe(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                  const geometry::ClusterLodOptions& lod, u32 page_bytes,
                  geometry::ClusterFileData& out, std::string& error) {
  structure::Recipe recipe;
  Report report;
  return read_file(text_of(bytes), recipe, error) &&
         build_container(recipe, inputs, lod, page_bytes, out, report, error);
}
constexpr assets::MeshGeneratorDesc k_mesh_generator{
    .name = "structure", .source_hash = hash_recipe, .build = build_recipe};
const assets::MeshGeneratorRegistrar k_mesh_registrar{k_mesh_generator};
}  // namespace

}  // namespace engine::structures
