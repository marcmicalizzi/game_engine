#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/stress_mesh.h>
#include <domain/texture/material_textures.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <algorithm>
#include <cmath>
#include <renderer_log.h>
#include <schemas/scene.h>
#include <utility>

#if ENGINE_RENDERER_RUINS
#include <domain/ruins/assembler.h>
#include <domain/ruins/kit.h>
#endif

namespace engine::renderer {

namespace {

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a valid scene") : text;
}

// splitmix64 to a float in [0, 1): the scatters' generator. Seeded per scatter, so adding a
// scatter to a file moves nothing another one placed.
f32 seeded_unit(u64& state) noexcept {
  state += 0x9E3779B97F4A7C15ull;
  u64 z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<f32>(z >> 40) / static_cast<f32>(1u << 24);
}

// An (n x n) heightfield over [-extent, extent]^2 in XZ, dunes-and-ridges in Y. The scene that
// needs no content at all: it is what a bring-up on a new machine draws, and what the
// renderer's own tests fall back to when there is no fixture.
void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.reserve(n * n);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f) +
                    0.12f * std::cos(fx * 4.3f - fz * 3.7f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  indices.reserve((n - 1) * (n - 1) * 6);
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

// The texture step `engine-content build --cache` runs, for a mesh this load reads through the
// derived-data cache (`content_build::run_texture_step`, the same function, so the two apps build
// the same `.tex` under the same keys): every image the container's records name is built into
// `<ddc>/textures/<key>.tex` unless an entry with that identity is already there, which costs a
// read and a hash per image on a warm cache. Without it a mesh the renderer clustered itself on a
// cold cache drew its textures decoded at one level — no mips, shimmering at a distance — until
// someone ran engine-content over it, because only engine-content built textures. A texture that
// cannot be built is a warning and the mesh draws through the decode, as it always could.
// `source_dir` is what the records' image paths are relative to.
void build_cache_textures(const geometry::ClusterFileData& data, std::string_view source_dir,
                          const std::string& ddc, const std::string& what) {
  if (ddc.empty()) return;
  Vector<content_build::TextureSource> sources;
  content_build::collect_texture_sources(data, source_dir, sources);
  if (sources.empty()) return;
  Vector<const content_build::TextureSource*> pointers;
  Vector<u32> mesh_of;
  pointers.reserve(sources.size());
  mesh_of.reserve(sources.size());
  for (const content_build::TextureSource& source : sources) {
    pointers.push_back(&source);
    mesh_of.push_back(0u);
  }
  const i64 start_ns = time::monotonic_ns();
  jobs::JobSystem pool(content_build::job_config(0));
  Vector<content_build::TextureResult> results;
  content_build::run_texture_step(
      std::span<const content_build::TextureSource* const>(pointers.data(), pointers.size()),
      std::span<const u32>(mesh_of.data(), mesh_of.size()), ddc, pool, results);
  u32 built = 0;
  u32 found = 0;
  for (const content_build::TextureResult& result : results) {
    switch (result.state) {
      case content_build::TextureState::Built: ++built; break;
      case content_build::TextureState::Skipped:
      case content_build::TextureState::Shared: ++found; break;
      case content_build::TextureState::Missing:
      case content_build::TextureState::Failed:
      case content_build::TextureState::Unreadable:
        ENGINE_LOG_WARN(log_renderer, "texture not built", log::field("mesh", what),
                        log::field("image", result.image),
                        log::field("state", content_build::texture_state_name(result.state)),
                        log::field("error", result.error));
        break;
    }
  }
  ENGINE_LOG_INFO(log_renderer, "mesh textures built", log::field("mesh", what),
                  log::field("built", built), log::field("found", found),
                  log::field("images", results.size()),
                  log::field("ms", static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6));
}

// Writes a mesh that was just imported and clustered into the derived-data cache, byte for byte
// the container `engine-content build` writes from the same source: the DAG, the materials as
// GPU records, a material index per cluster (a primitive that names none gets one appended
// default, shared), the image paths as the glTF gave them, and the source path they are
// relative to, plus the streaming page table the clusters were laid out in. `lod` and `pages`
// are moved into the container and back out again, so neither is ever copied. A cache that
// cannot be written is a warning and nothing more: the picture does not depend on it.
// True when the entry landed, which is what tells the caller its pages may be read back out of it.
// The textures its records name are built into `ddc` beside it (`build_cache_textures`), landed or
// not, because the texture lookup reads the records this function hands back, not the file.
bool write_cluster_cache(const std::string& path, const std::string& source, const std::string& ddc,
                         const assets::MeshData& mesh_data, const Vector<i32>& part_material,
                         const Vector<u32>& part_of_cluster, geometry::ClusterLodMesh& lod,
                         geometry::ClusterPages& pages,
                         Vector<geometry::ClusterFileTexture>& textures) {
  geometry::ClusterFileData data;
  data.mesh = std::move(lod);
  data.pages = std::move(pages);
  data.source_path = source;
  data.materials.reserve(mesh_data.materials.size() + 1);
  for (const assets::Material& source_material : mesh_data.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = source_material.base_color;
    material.metallic = source_material.metallic;
    material.roughness = source_material.roughness;
    material.base_color_image = source_material.base_color_image;
    material.normal_image = source_material.normal_image;
    material.metallic_roughness_image =
        geometry::encode_optional_image(source_material.metallic_roughness_image);
    material.occlusion_image = geometry::encode_optional_image(source_material.occlusion_image);
    material.emissive_image = geometry::encode_optional_image(source_material.emissive_image);
    material.emissive = source_material.emissive;
    material.normal_scale = source_material.normal_scale;
    material.alpha = geometry::encode_alpha_word(
        source_material.alpha_mode, source_material.double_sided, source_material.alpha_cutoff);
    data.materials.push_back(material);
    data.material_sampling.push_back(geometry::encode_material_sampling(
        source_material.sampling, source_material.occlusion_strength));
  }
  constexpr u32 k_no_default = ~u32{0};
  u32 default_material = k_no_default;
  data.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster) {
    const i32 index = part_material[part];
    if (index >= 0 && static_cast<u32>(index) < mesh_data.materials.size()) {
      data.cluster_material.push_back(static_cast<u32>(index));
      continue;
    }
    if (default_material == k_no_default) {
      default_material = data.materials.size();
      data.materials.push_back(geometry::ClusterFileMaterial{});
      data.material_sampling.push_back(geometry::ClusterFileMaterialSampling{});
    }
    data.cluster_material.push_back(default_material);
  }
  // The image paths as the glTF gave them, and beside them the bytes of the images it embedded,
  // so that what this container draws is what the glTF draws. Writing only the (empty) paths is
  // what made a cache hit a different picture from a cache miss ([geometry](geometry.md)).
  data.image_paths.reserve(mesh_data.images.size());
  data.images.reserve(mesh_data.images.size());
  for (const assets::ImageRef& image : mesh_data.images) {
    data.image_paths.push_back(image.uri);
    geometry::ClusterImage carried;
    carried.mime_type = image.mime_type;
    carried.bytes = image.bytes;
    data.images.push_back(std::move(carried));
  }
  // Where each image's built texture is, by the same function engine-content build calls, so the
  // two apps' entries stay the same bytes; the records come back to the caller whether or not the
  // entry lands, because the texture lookup does not depend on it (texture.md).
  texture::fill_cluster_texture_records(data);

  std::string error;
  const io::Status status = io::make_directories(io::parent_path(path));
  if (status != io::Status::Ok) {
    error = std::string("cannot create the directory: ") + io::status_name(status);
  } else if (geometry::write_cluster_file(path, data, &error)) {
    ENGINE_LOG_INFO(log_renderer, "cluster cache written", log::field("path", path),
                    log::field("clusters", data.mesh.mesh.clusters.size()));
  }
  if (!error.empty()) {
    ENGINE_LOG_WARN(log_renderer, "cluster cache not written", log::field("path", path),
                    log::field("error", error));
  }
  build_cache_textures(data, io::parent_path(source), ddc, source.empty() ? path : source);
  lod = std::move(data.mesh);
  pages = std::move(data.pages);
  textures = std::move(data.textures);
  return error.empty();
}

// The page layout renumbers a mesh's clusters, so it is only ever run where the result is
// already the renumbered one — the glTF, terrain and container paths below, whose layout is part
// of the derived-data cache key — or where nothing has been built yet and the caller asked to
// stream. Paging the classic heightfield unconditionally would change every id and every cut of
// the scene the renderer's own tests draw, for a table nobody would read.
void lay_out_pages(const SceneDesc& desc, const char* what, u32 page_bytes, SourceMesh& out,
                   geometry::ClusterLodMesh& lod, geometry::ClusterPages& pages) {
  if (!desc.stream) return;
  Vector<u32> source_of_cluster;
  std::string page_error;
  geometry::ClusterPagesOptions options;
  if (page_bytes > 0) options.page_bytes = page_bytes;
  if (geometry::build_cluster_pages(lod, options, pages, &page_error, &source_of_cluster)) {
    geometry::permute_cluster_array(source_of_cluster, out.part_of_cluster);
    return;
  }
  pages = geometry::ClusterPages{};
  ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("mesh", what),
                  log::field("error", page_error));
}

// A container's materials, images, material map and (when streaming) page table, in the shape the
// glTF path leaves behind, so that everything downstream is the same code for both: one part per
// material, and the file's own map from cluster to part. `container` is where it was read from.
void adopt_container(geometry::ClusterFileData& container_data, const std::string& container,
                     const SceneDesc& desc, SourceMesh& out, geometry::ClusterLodMesh& lod,
                     geometry::ClusterPages& pages) {
  lod = std::move(container_data.mesh);
  out.data.materials.reserve(container_data.materials.size());
  out.part_material.reserve(container_data.materials.size());
  for (u32 m = 0; m < container_data.materials.size(); ++m) {
    const geometry::ClusterFileMaterial& source = container_data.materials[m];
    assets::Material material;
    // How its slots sample their images, or the glTF defaults for a container written before
    // the records existed (geometry.md, "How a material samples its images").
    geometry::decode_material_sampling(geometry::cluster_material_sampling(container_data, m),
                                       material.sampling, material.occlusion_strength);
    material.base_color = source.base_color;
    material.metallic = source.metallic;
    material.roughness = source.roughness;
    material.base_color_image = source.base_color_image;
    material.normal_image = source.normal_image;
    material.metallic_roughness_image =
        geometry::decode_optional_image(source.metallic_roughness_image);
    material.occlusion_image = geometry::decode_optional_image(source.occlusion_image);
    material.emissive_image = geometry::decode_optional_image(source.emissive_image);
    material.emissive = source.emissive;
    material.normal_scale = source.normal_scale;
    material.alpha_mode = geometry::alpha_word_mode(source.alpha);
    material.double_sided = geometry::alpha_word_double_sided(source.alpha);
    material.alpha_cutoff = geometry::alpha_word_cutoff(source.alpha);
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(static_cast<i32>(out.part_material.size()));
  }
  // The images: a path names a file beside the source mesh, and an image the source embedded
  // travels inside the container, so everything below is the same code for both and a container
  // draws what its source draws. A container written before the bytes were carried has the
  // paths alone, and an embedded image of one still draws untextured with a warning.
  out.data.images.reserve(container_data.image_paths.size());
  for (u32 i = 0; i < container_data.image_paths.size(); ++i) {
    assets::ImageRef image;
    image.uri = container_data.image_paths[i];
    if (i < container_data.images.size()) {
      image.mime_type = std::move(container_data.images[i].mime_type);
      image.bytes = std::move(container_data.images[i].bytes);
    }
    out.data.images.push_back(std::move(image));
  }
  out.textures = std::move(container_data.textures);  // where the built textures are, if any
  out.part_of_cluster = std::move(container_data.cluster_material);
  if (out.part_of_cluster.empty()) {  // a container with no material map: one default for all
    out.part_of_cluster = Vector<u32>(lod.mesh.clusters.size(), out.part_material.size());
    out.part_material.push_back(-1);
  }
  // Relative image paths belong to the mesh the container was built from; a container that
  // does not name one resolves them beside itself.
  out.image_dir = std::string(io::parent_path(container_data.source_path.empty()
                                                  ? std::string_view(container)
                                                  : std::string_view(container_data.source_path)));
  out.primitives = 0;         // a container does not record how many were merged into it
  out.container = container;  // where its pages can be read from by range
  // The container's own page table, which is the one its clusters were renumbered into. A
  // container written with `--page-bytes 0` carries none, and the layout is run here instead so
  // that a streamed run never depends on which build wrote the cache entry.
  if (desc.stream) {
    pages = std::move(container_data.pages);
    if (pages.pages.empty()) lay_out_pages(desc, container.c_str(), 0, out, lod, pages);
  }
  ENGINE_LOG_INFO(log_renderer, "mesh loaded", log::field("path", container),
                  log::field("from", "cluster file"),
                  log::field("source", container_data.source_path), log::field("cache", out.cache),
                  log::field("clusters", lod.mesh.clusters.size()),
                  log::field("vertices", lod.mesh.vertices.size()),
                  log::field("triangles", lod.leaf_triangle_count),
                  log::field("lod_levels", lod.level_cluster_counts.size()),
                  log::field("materials", out.data.materials.size()),
                  log::field("images", out.data.images.size()));
}

// The scene file's terrain (terrain.h): read from the derived-data cache when an entry for these
// fields and these LOD options exists, built and written there otherwise — so a 2049 x 2049
// terrain costs its clustering once per machine, like a glTF. It is always laid out in pages,
// exactly as a glTF's cache entry is, because the layout renumbers the clusters and a cache hit
// has to be the same scene as the build that wrote it.
//
// Its materials are the desert's four — sand, ridge rock, basin sand and the basin's floor —
// chosen per cluster from the features under the cluster's centre, and they are ordinary file
// materials: the terrain is a mesh like any other from here on, which is what lets it share a
// scene with glTF meshes (the classic heightfield's height-banded palette is the GPU scene's own
// and only exists when the heightfield is the whole scene).
bool load_terrain(const SceneDesc& desc, SourceMesh& out, geometry::ClusterLodMesh& lod,
                  geometry::ClusterPages& pages, std::string& error) {
  const u64 hash = terrain_hash(desc.terrain);
  out.source_hash = hash;
  std::string cache_path;
  if (desc.cache) {
    const u64 key = geometry::cluster_cache_key(hash, desc.lod, false,
                                                geometry::ClusterPagesOptions{}.page_bytes);
    cache_path = geometry::cluster_cache_path(desc.ddc, key);
    out.cache = "miss";
    if (io::exists(cache_path)) {
      geometry::ClusterFileData container_data;
      std::string read_error;
      if (geometry::read_cluster_file(cache_path, container_data, &read_error)) {
        out.cache = "hit";
        adopt_container(container_data, cache_path, desc, out, lod, pages);
        return true;
      }
      ENGINE_LOG_WARN(log_renderer, "cluster cache entry ignored", log::field("path", cache_path),
                      log::field("error", read_error));
    }
  }
  Vector<Vec3> positions;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  if (!build_terrain_mesh(desc.terrain, positions, indices, uvs, &error)) return false;
  // The normals are the grid's own, by central differences, and they go to the builder as an
  // attribute rather than being recomputed per level. An edge collapse keeps source vertices, so a
  // coarse cluster then shades with the fine surface's normals; recomputed from the coarse
  // triangles they made every LOD boundary a visible change of shade — a darker patch of sand the
  // shape of a cluster, a few hundred metres out, which is what the first pictures showed.
  const u32 n = desc.terrain.size;
  Vector<Vec3> normals(positions.size());
  for (u32 zi = 0; zi < n; ++zi) {
    for (u32 xi = 0; xi < n; ++xi) {
      const u32 x0 = xi > 0 ? xi - 1 : xi;
      const u32 x1 = xi + 1 < n ? xi + 1 : xi;
      const u32 z0 = zi > 0 ? zi - 1 : zi;
      const u32 z1 = zi + 1 < n ? zi + 1 : zi;
      const Vec3 dx = positions[zi * n + x1] - positions[zi * n + x0];
      const Vec3 dz = positions[z1 * n + xi] - positions[z0 * n + xi];
      normals[zi * n + xi] = normalize(cross(dz, dx));  // +y for a flat grid
    }
  }
  geometry::AttributeSource attribute_source;
  attribute_source.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  attribute_source.normals = std::span<const Vec3>(normals.data(), normals.size());
  if (!geometry::build_cluster_lod(positions, indices, desc.lod, lod, &error, attribute_source))
    return false;
  geometry::ClusterPages built;
  std::string page_error;
  if (!geometry::build_cluster_pages(lod, geometry::ClusterPagesOptions{}, built, &page_error)) {
    ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("mesh", "terrain"),
                    log::field("error", page_error));
  }
  // The materials, picked per cluster after the layout has renumbered them, by a vote of the
  // cluster's own vertices: a cluster is one material, so a border between sand and rock is a
  // cluster edge, and a coarse cluster that spans a ridge's foot and a stretch of plain is sand.
  constexpr f32 k_colours[k_terrain_materials][4] = {
      {0.84f, 0.69f, 0.47f, 0.92f},  // sand: albedo and roughness
      {0.47f, 0.39f, 0.32f, 0.78f},  // ridge rock
      {0.62f, 0.52f, 0.36f, 0.95f},  // basin sand
      {0.36f, 0.40f, 0.22f, 0.85f},  // the basin's floor
  };
  constexpr const char* k_names[k_terrain_materials] = {"sand", "rock", "basin", "floor"};
  for (u32 m = 0; m < k_terrain_materials; ++m) {
    assets::Material material;
    material.name = k_names[m];
    material.base_color = Vec4{k_colours[m][0], k_colours[m][1], k_colours[m][2], 1.0f};
    material.roughness = k_colours[m][3];
    material.metallic = 0.0f;
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(static_cast<i32>(m));
  }
  out.part_of_cluster.resize(lod.mesh.clusters.size(), 0u);
  for (u32 c = 0; c < lod.mesh.clusters.size(); ++c) {
    const geometry::ClusterDesc& cluster = lod.mesh.clusters[c];
    out.part_of_cluster[c] = terrain_majority_material(
        desc.terrain, std::span<const Vec3>(lod.mesh.vertices.data() + cluster.vertex_offset,
                                            cluster.vertex_count));
  }
  out.primitives = 1;
  ENGINE_LOG_INFO(log_renderer, "mesh loaded", log::field("from", "terrain"),
                  log::field("size", desc.terrain.size), log::field("extent", desc.terrain.extent),
                  log::field("triangles", lod.leaf_triangle_count),
                  log::field("clusters", lod.mesh.clusters.size()),
                  log::field("lod_levels", lod.level_cluster_counts.size()));
  if (!cache_path.empty() &&
      write_cluster_cache(cache_path, "", desc.ddc, out.data, out.part_material,
                          out.part_of_cluster, lod, built, out.textures)) {
    out.container = cache_path;
  }
  if (desc.stream) pages = std::move(built);
  return true;
}

// Reads one mesh: a `.clusters` container named outright, the derived-data cache entry this
// glTF and these options address, or the glTF itself (imported, welded, clustered per primitive
// so every cluster has a single material, merged, and written into the cache for the next run).
// An empty `path` builds the scene's terrain when it has one and the procedural scene otherwise.
// `expected_hash`, when not zero, is the source hash the file must have (a scene file's `hash`).
bool load_source_mesh(const std::string& path, const SceneDesc& desc, u64 expected_hash,
                      SourceMesh& out, geometry::ClusterLodMesh& lod, geometry::ClusterPages& pages,
                      std::string& error) {
  auto page_layout = [&](const char* what, u32 page_bytes) {
    lay_out_pages(desc, what, page_bytes, out, lod, pages);
  };
  // Built textures live in the same derived-data root as built meshes, and a load that reads no
  // cache reads neither (texture.md, "In the renderer").
  out.texture_ddc = desc.cache ? desc.ddc : std::string();
  if (path.empty() && desc.terrain.enabled) return load_terrain(desc, out, lod, pages, error);
  if (path.empty() && desc.procedural == Procedural::shredded_atlas) {
    // The atlas stress fixture, built here rather than committed as a file so that the scene
    // corpus can guard the LOD seam defect on a fresh clone and without a third-party model
    // ([geometry](geometry.md), "The shredded atlas"). It comes with a material and a probe
    // texture whose colour encodes atlas position, because the whole point of the scene is that a
    // UV a collapse dragged into the wrong island reads the wrong colour — a picture comparison
    // of this mesh measures exactly that and nothing else. The texture travels as an encoded PNG
    // in the mesh's images, which is the same road a GLB's embedded textures take, so nothing
    // downstream needs to know the mesh was procedural.
    geometry::ShreddedAtlasMesh fixture;
    geometry::ShreddedAtlasOptions fixture_options;
    // Denser than the geometry module's own unit fixture on purpose: a picture comparison needs a
    // DAG deep enough that a coarse threshold reaches a *much* smaller cut than the finest, and
    // 192 x 96 quads (36,864 triangles, 1,152 islands) is where that happens while the scene still
    // loads in well under a second.
    fixture_options.segments = 192;
    fixture_options.rings = 96;
    geometry::build_shredded_atlas_torus(fixture_options, fixture);
    geometry::AttributeSource attribute_source;
    attribute_source.normals =
        std::span<const Vec3>(fixture.normals.data(), fixture.normals.size());
    attribute_source.uvs = std::span<const Vec2>(fixture.uvs.data(), fixture.uvs.size());
    if (!geometry::build_cluster_lod(fixture.positions, fixture.indices, desc.lod, lod, &error,
                                     attribute_source)) {
      return false;
    }
    Vector<u8> texels;
    geometry::build_atlas_probe_texture(1024, fixture.atlas_cells, texels);
    assets::ImageRef probe;
    probe.name = "atlas-probe";
    probe.mime_type = "image/png";
    if (!image::encode_png(1024, 1024, 4, std::span<const u8>(texels.data(), texels.size()),
                           probe.bytes)) {
      error = "the atlas probe texture could not be encoded";
      return false;
    }
    out.data.images.push_back(std::move(probe));
    assets::Material material;
    material.name = "atlas-probe";
    material.base_color_image = 0;
    material.metallic = 0.0f;
    material.roughness = 0.6f;
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(0);
    out.part_of_cluster.resize(lod.mesh.clusters.size(), 0u);
    out.primitives = 1;
    ENGINE_LOG_INFO(log_renderer, "mesh loaded", log::field("from", "shredded atlas"),
                    log::field("islands", fixture.island_count),
                    log::field("atlas_cells", fixture.atlas_cells),
                    log::field("seam_vertices", fixture.seam_vertices),
                    log::field("triangles", lod.leaf_triangle_count),
                    log::field("clusters", lod.mesh.clusters.size()),
                    log::field("lod_levels", lod.level_cluster_counts.size()));
    page_layout("shredded-atlas", desc.page_bytes);
    return true;
  }
  if (path.empty()) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(desc.heightfield_grid, 10.0f, positions, indices);
    Vector<Vec2> uvs;
    uvs.reserve(positions.size());
    for (const Vec3& p : positions)
      uvs.push_back(Vec2{(p.x + 10.0f) / 20.0f, (p.z + 10.0f) / 20.0f});
    geometry::AttributeSource attribute_source;
    attribute_source.uvs = std::span<const Vec2>(uvs.data(), uvs.size());  // normals: computed
    if (!geometry::build_cluster_lod(positions, indices, desc.lod, lod, &error, attribute_source)) {
      return false;
    }
    page_layout("heightfield", desc.page_bytes);
    return true;
  }
  // Where a built mesh may already be: a container named outright, or the cache entry this
  // source and these options address.
  std::string container;
  std::string cache_path;
  if (io::extension(path) == ".clusters") {
    container = path;
    out.cache = "file";
  } else if (desc.cache || expected_hash != 0) {
    u64 source_hash = 0;
    if (!assets::source_mesh_hash(path, source_hash, &error)) return false;
    out.source_hash = source_hash;
    // A scene that names a mesh by its bytes gets those bytes or nothing: a number measured on a
    // different file would still carry this scene's name (plan 09 §9.4, "content-addressed").
    if (expected_hash != 0 && source_hash != expected_hash) {
      error = path + " hashes to " + hash_hex(source_hash) + ", and the scene names " +
              hash_hex(expected_hash);
      return false;
    }
    if (desc.cache) {
      // The page target is part of the key, so this has to be the one the build below uses —
      // and the one engine-content uses by default, or the two apps would stop sharing entries.
      const u64 key = geometry::cluster_cache_key(source_hash, desc.lod, true,
                                                  geometry::ClusterPagesOptions{}.page_bytes);
      cache_path = geometry::cluster_cache_path(desc.ddc, key);
      out.cache = "miss";
      if (io::exists(cache_path)) container = cache_path;
    }
  }
  geometry::ClusterFileData container_data;
  bool from_container = false;
  if (!container.empty()) {
    std::string read_error;
    if (geometry::read_cluster_file(container, container_data, &read_error)) {
      from_container = true;
      if (!cache_path.empty()) out.cache = "hit";
    } else if (cache_path.empty()) {
      error = read_error;  // a file named on the command line must load
      return false;
    } else {
      // A cache entry this build cannot read is not an error; it is a miss with a warning.
      ENGINE_LOG_WARN(log_renderer, "cluster cache entry ignored", log::field("path", container),
                      log::field("error", read_error));
    }
  }
  if (from_container) {
    // A container found in or named beside the cache may have been written before its textures
    // were built (an older engine-view, a cache that lost them, `engine-content build` without
    // `--cache`): the step builds whatever of them is missing and finds the rest.
    if (desc.cache) {
      const std::string& source = container_data.source_path;
      build_cache_textures(container_data, io::parent_path(source.empty() ? container : source),
                           desc.ddc, container);
    }
    adopt_container(container_data, container, desc, out, lod, pages);
    return true;
  }
  if (!assets::load_gltf(path, out.data, &error)) return false;
  out.image_dir = std::string(io::parent_path(path));
  // The UV repair, exactly where `engine-content build` runs it — after the load, before the weld
  // — because the two write and read the same cache entries and have to build the same bytes
  // ([geometry](geometry.md), "UV-degenerate triangles: the repair"). A mesh the repair refuses
  // (primitives out of order, which the importer never produces) is drawn unrepaired, as before.
  geometry::UvRepairReport uv_repair;
  std::string repair_error;
  if (!assets::repair_uv_degenerate_triangles(out.data, uv_repair, &repair_error)) {
    ENGINE_LOG_WARN(log_renderer, "uv repair refused", log::field("path", path),
                    log::field("error", repair_error));
  } else if (uv_repair.triangles != 0) {
    ENGINE_LOG_INFO(
        log_renderer, "uv repair", log::field("path", path),
        log::field("rule", "geometry.uv_degenerate"), log::field("islands", uv_repair.islands),
        log::field("triangles", uv_repair.triangles), log::field("refolded", uv_repair.refolded),
        log::field("dropped", uv_repair.dropped), log::field("unrepaired", uv_repair.unrepaired));
  }
  // Exporters duplicate vertices freely; welding the identical ones gives the cluster builder
  // shared vertices to fill clusters with and the LOD builder edges to collapse. The weld is
  // `assets::weld_vertices`, the one `engine-content build` calls, because the two write and read
  // the same cache entries: it hands the weld **every per-vertex stream the mesh owns**, so two
  // duplicates that disagree on skin weights or morph deltas stay two vertices and the morph
  // channels are renumbered with the vertices rather than left in the old numbering
  // ([geometry](geometry.md), "Skinned meshes" and "Morph channels").
  const u32 loaded_vertices = out.data.positions.size();
  const u32 welded_vertices = assets::weld_vertices(out.data);
  ENGINE_LOG_INFO(
      log_renderer, "mesh loaded", log::field("path", path), log::field("from", "gltf"),
      log::field("cache", out.cache), log::field("vertices", loaded_vertices),
      log::field("welded", welded_vertices), log::field("triangles", out.data.indices.size() / 3),
      log::field("primitives", out.data.primitives.size()),
      log::field("materials", out.data.materials.size()),
      log::field("skins", out.data.skins.size()), log::field("images", out.data.images.size()));
  const geometry::AttributeSource attribute_source = assets::attribute_source(out.data);
  Vector<geometry::ClusterLodMesh> parts;
  for (const assets::Primitive& primitive : out.data.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(out.data.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(out.data.positions, range, desc.lod, part, &error,
                                     attribute_source)) {
      return false;
    }
    parts.push_back(std::move(part));
    out.part_material.push_back(primitive.material);
  }
  if (!geometry::merge_cluster_lod(parts, lod, &out.part_of_cluster, &error)) return false;
  out.primitives = parts.size();
  // Laid out in streaming pages, exactly as engine-content lays it out, because the page target
  // is part of the cache key and the entry either app writes has to be the same container. The
  // layout renumbers the clusters, so the map from cluster to primitive comes along. It runs
  // whether or not anyone will stream from the table, because the *renumbering* is what the cache
  // key promises; `pages` is only kept when a caller asked for it.
  geometry::ClusterPages built;
  Vector<u32> source_of_cluster;
  std::string page_error;
  if (geometry::build_cluster_pages(lod, geometry::ClusterPagesOptions{}, built, &page_error,
                                    &source_of_cluster)) {
    geometry::permute_cluster_array(source_of_cluster, out.part_of_cluster);
  } else {
    ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("path", path),
                    log::field("error", page_error));
  }
  // Into the cache for the next run, as the same container engine-content build writes: either
  // app fills the cache, either app finds it.
  if (!cache_path.empty()) {
    if (write_cluster_cache(cache_path, path, desc.ddc, out.data, out.part_material,
                            out.part_of_cluster, lod, built, out.textures)) {
      // The entry this load just wrote holds the same bytes in the same order this mesh is now in,
      // so a streamed run may read its pages back out of it rather than out of host memory.
      out.container = cache_path;
    }
  }
  if (desc.stream) pages = std::move(built);
  return true;
}

// n x n copies of mesh 0 on a grid, with a rotation and a scale that vary per copy and a
// non-uniform scale on every third, so one flag exercises the uniform and the non-uniform path,
// the cone test's precondition, and the LOD scaling all at once.
void make_instance_grid(u32 n, f32 mesh_radius, u32 joints, f32 bounds_padding,
                        Vector<SceneInstance>& out) {
  const f32 spacing = 3.0f * mesh_radius;
  const f32 center = 0.5f * static_cast<f32>(n - 1);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const u32 k = z * n + x;
      SceneInstance instance;
      instance.mesh = 0;
      instance.joints = joints;
      instance.bounds_padding = bounds_padding;
      instance.transform.position = Vec3{(static_cast<f32>(x) - center) * spacing, 0.0f,
                                         (static_cast<f32>(z) - center) * spacing};
      instance.transform.rotation = quat_from_euler(radians(static_cast<f32>(k * 37 % 360)),
                                                    radians(static_cast<f32>(k * 11 % 25)), 0.0f);
      const f32 s = 0.6f + 0.2f * static_cast<f32>(k % 4);
      instance.transform.scale =
          k % 3 == 2 ? Vec3{s, s * 1.4f, s * 0.7f} : Vec3{s, s, s};  // every third non-uniform
      out.push_back(instance);
    }
  }
}

#if ENGINE_RENDERER_RUINS
// The terrain as the assembler's height query: a plain function over the scene's description.
f32 terrain_ground(const void* context, f32 x, f32 z) noexcept {
  return terrain_height(*static_cast<const TerrainDesc*>(context), x, z);
}

// A scene's `ruins` entries (schema `engine.scene.RuinScatter`, docs/subsystems/ruins.md): each
// kit read once, its members' meshes appended after the file's own — so every index the file used
// still names what it named, and the terrain, pushed after this, stays the last mesh — and every
// building assembled here, standing on the terrain, so the renderer sees plain instances of kit
// meshes and nothing else about ruins. Single-threaded on purpose: the loader has no job system,
// and a thousand buildings cost milliseconds (ruins.md, "Performance notes").
bool expand_ruins(const scene::Scene& file, const std::string& path, const std::string& dir,
                  SceneDesc& out, std::string& error) {
  struct LoadedKit {
    std::string path;
    ruins::Kit kit;
    u32 first_mesh = 0;
  };
  Vector<LoadedKit> kits;
  kits.reserve(file.ruins.size());
  for (u32 r = 0; r < file.ruins.size(); ++r) {
    const scene::RuinScatter& scatter = file.ruins[r];
    const std::string where = path + ": ruins " + std::to_string(r);
    if (scatter.kit.empty()) {
      error = where + " names no kit";
      return false;
    }
    const std::string kit_path = io::is_absolute_path(scatter.kit) || dir.empty()
                                     ? scatter.kit
                                     : io::join_path(dir, scatter.kit);
    u32 k = 0;
    while (k < kits.size() && kits[k].path != kit_path)
      ++k;
    if (k == kits.size()) {
      LoadedKit loaded;
      loaded.path = kit_path;
      if (!ruins::read_kit_file(kit_path, loaded.kit, error)) {
        error = where + ": " + error;
        return false;
      }
      loaded.first_mesh = out.meshes.size();
      for (u32 m = 0; m < loaded.kit.meshes.size(); ++m) {
        out.meshes.push_back(loaded.kit.meshes[m]);
        SceneMeshInfo info;
        info.name = loaded.kit.name + "/" + std::string(io::file_name(loaded.kit.meshes[m]));
        info.hash = loaded.kit.mesh_hashes[m];
        out.mesh_info.push_back(std::move(info));
      }
      kits.push_back(std::move(loaded));
    }
    const LoadedKit& loaded = kits[k];
    const i32 tile_cm = ruins::to_cm(scatter.tile_size);
    if (tile_cm <= 0) {
      error = where + ": tile_size must be positive";
      return false;
    }
    Vector<ruins::TileCoord> tiles;
    ruins::choose_tiles(scatter.seed, ruins::TileCoord{scatter.tile_min[0], scatter.tile_min[1]},
                        ruins::TileCoord{scatter.tile_max[0], scatter.tile_max[1]}, scatter.count,
                        scatter.density, tiles);
    ruins::Placement placement;
    placement.world_seed = scatter.seed;
    placement.tile_cm = tile_cm;
    placement.wind_step = ruins::yaw_step_from_degrees(scatter.wind_deg);
    if (out.terrain.enabled) placement.ground = ruins::Ground{&terrain_ground, &out.terrain};
    ruins::Output built;
    const i64 start = time::monotonic_ns();
    if (!ruins::assemble_tiles(loaded.kit, placement,
                               std::span<const ruins::TileCoord>(tiles.data(), tiles.size()),
                               nullptr, built, &error)) {
      error = where + ": " + error;
      return false;
    }
    out.instances.reserve(out.instances.size() + built.instances.size());
    for (const ruins::Instance& piece : built.instances) {
      SceneInstance instance;
      instance.mesh = loaded.first_mesh + loaded.kit.members[piece.member].mesh_index;
      instance.transform.position = ruins::instance_translation(loaded.kit, piece);
      instance.transform.rotation = quat_from_axis_angle(
          Vec3{0.0f, 1.0f, 0.0f},
          radians(22.5f * static_cast<f32>(ruins::instance_yaw_step(loaded.kit, piece))));
      out.instances.push_back(instance);
    }
    out.ruin_buildings += built.sites.size();
    out.ruin_instances += built.instances.size();
    ENGINE_LOG_INFO(log_renderer, "ruins assembled", log::field("scene", path),
                    log::field("kit", loaded.kit.name), log::field("buildings", built.sites.size()),
                    log::field("instances", built.instances.size()),
                    log::field("ms", static_cast<f64>(time::monotonic_ns() - start) / 1.0e6),
                    log::field("hash", hash_hex(ruins::hash_output(built))));
  }
  return true;
}
#endif

}  // namespace

void mesh_bounds(const geometry::ClusterLodMesh& lod, u32 first, u32 count, Vec3& center,
                 f32& radius) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (u32 i = first; i < first + count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
              std::min(lo.z, c.center.z - c.radius)};
    hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
              std::max(hi.z, c.center.z + c.radius)};
  }
  center = (lo + hi) * 0.5f;
  radius = 1e-6f;  // tighter than the box diagonal for elongated meshes
  for (u32 i = first; i < first + count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    radius = std::max(radius, length(c.center - center) + c.radius);
  }
}

std::string hash_hex(u64 hash) {
  static constexpr char k_digits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (u32 i = 0; i < 16; ++i)
    text[15 - i] = k_digits[(hash >> (4 * i)) & 0xfu];
  return text;
}

bool parse_hash_hex(std::string_view text, u64& out) noexcept {
  if (text.size() != 16) return false;
  u64 value = 0;
  for (const char c : text) {
    u64 digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<u64>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<u64>(c - 'a' + 10);
    } else {
      return false;
    }
    value = value << 4 | digit;
  }
  out = value;
  return true;
}

bool read_scene_file(const std::string& path, SceneDesc& out, std::string& error) {
  return read_scene_file(path, SceneFileOptions{}, out, error);
}

bool read_scene_file(const std::string& path, const SceneFileOptions& options, SceneDesc& out,
                     std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = std::string("cannot read ") + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = std::string(path) + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  // One grammar, and it is the schema's: an unknown field is refused with its path rather than
  // skipped, because a misspelt `"ground"` that quietly did nothing would float a building.
  scene::Scene file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    error = path + ": " + schema_errors(ctx);
    return false;
  }
  if (!file.format.empty() && file.format != "engine.scene.v1") {
    error = path + ": format is '" + file.format + "', not engine.scene.v1";
    return false;
  }
  if (file.meshes.empty() && !file.terrain.has_value() && file.ruins.empty()) {
    error = path + ": no \"meshes\", no \"terrain\" and no \"ruins\": a scene needs at least one";
    return false;
  }
  out.name = file.name;
  out.file_hash = hash_bytes(text.data(), text.size());

  // The overlay: a map from content hash to a local file, read before the meshes so that a mesh
  // it names is loaded from there and checked against the hash the scene gave for it.
  Vector<std::pair<u64, std::string>> overlay;  // hash, path
  if (!options.overlay.empty()) {
    std::string overlay_text;
    const io::Status read = io::read_file(options.overlay, overlay_text);
    if (read != io::Status::Ok) {
      error = "cannot read the overlay " + options.overlay + ": " + io::status_name(read);
      return false;
    }
    JsonValue overlay_json;
    const JsonParseResult overlay_parsed = parse_json(overlay_text, overlay_json);
    scene::Overlay manifest;
    schema::ReadContext overlay_ctx;
    if (!overlay_parsed.ok) {
      error = options.overlay + ":" + std::to_string(overlay_parsed.line) + ": " +
              overlay_parsed.message;
      return false;
    }
    if (!schema::from_json(manifest, overlay_json, overlay_ctx) || !overlay_ctx.ok()) {
      error = options.overlay + ": " + schema_errors(overlay_ctx);
      return false;
    }
    const std::string overlay_dir(io::parent_path(options.overlay));
    for (const scene::OverlayEntry& entry : manifest.entries) {
      u64 hash = 0;
      if (!parse_hash_hex(entry.hash, hash)) {
        error = options.overlay + ": '" + entry.hash + "' is not 16 lower-case hex digits";
        return false;
      }
      overlay.push_back({hash, io::is_absolute_path(entry.path) || overlay_dir.empty()
                                   ? entry.path
                                   : io::join_path(overlay_dir, entry.path)});
    }
  }

  const std::string dir(io::parent_path(path));
  out.camera_path =
      file.camera_path.empty() || io::is_absolute_path(file.camera_path) || dir.empty()
          ? file.camera_path
          : io::join_path(dir, file.camera_path);
  for (u32 i = 0; i < file.meshes.size(); ++i) {
    const scene::Mesh& mesh = file.meshes[i];
    const std::string where = path + ": mesh " + std::to_string(i);
    if (mesh.path.empty()) {
      error = where + " has no \"path\"";
      return false;
    }
    SceneMeshInfo info;
    info.name = mesh.name;
    if (!mesh.hash.empty() && !parse_hash_hex(mesh.hash, info.hash)) {
      error = where + ": hash '" + mesh.hash + "' is not 16 lower-case hex digits";
      return false;
    }
    if (mesh.fit.has_value()) {
      info.fit.height = mesh.fit->height;
      info.fit.extent = mesh.fit->extent;
      info.fit.ground = mesh.fit->ground;
    }
    std::string resolved =
        io::is_absolute_path(mesh.path) || dir.empty() ? mesh.path : io::join_path(dir, mesh.path);
    if (!mesh.overlay.empty()) {
      u64 wanted = 0;
      if (!parse_hash_hex(mesh.overlay, wanted)) {
        error = where + ": overlay '" + mesh.overlay + "' is not 16 lower-case hex digits";
        return false;
      }
      for (const auto& [hash, overlay_path] : overlay) {
        if (hash != wanted) continue;
        resolved = overlay_path;
        info.hash = hash;  // the load hashes the replacement and refuses other bytes
        info.origin = "overlay";
        break;
      }
      if (!overlay.empty() && std::string_view(info.origin) != "overlay") {
        ENGINE_LOG_WARN(log_renderer, "the overlay does not name this mesh; the scene's file stays",
                        log::field("mesh", mesh.name), log::field("overlay", mesh.overlay));
      }
    }
    out.meshes.push_back(std::move(resolved));
    out.mesh_info.push_back(std::move(info));
  }
  if (file.terrain.has_value()) {
    const scene::Terrain& t = *file.terrain;
    out.terrain.enabled = true;
    out.terrain.size = t.size;
    out.terrain.extent = t.extent;
    out.terrain.seed = t.seed;
    out.terrain.dune_height = t.dune_height;
    out.terrain.dune_wavelength = t.dune_wavelength;
    for (const scene::Ridge& r : t.ridges)
      out.terrain.ridges.push_back(TerrainRidge{r.from, r.to, r.height, r.width, r.roughness});
    for (const scene::Basin& b : t.basins)
      out.terrain.basins.push_back(TerrainBasin{b.center, b.radius, b.depth});
    if (t.size < 2 || t.size > k_terrain_max_size || !(t.extent > 0.0f)) {
      error = path + ": terrain size must be within 2.." + std::to_string(k_terrain_max_size) +
              " and extent positive";
      return false;
    }
  }
  const u32 file_meshes = file.meshes.size();
  auto ground_at = [&](f32 x, f32 z) {
    return out.terrain.enabled ? terrain_height(out.terrain, x, z) : 0.0f;
  };

  for (u32 i = 0; i < file.instances.size(); ++i) {
    const scene::Instance& entry = file.instances[i];
    if (entry.mesh >= file_meshes) {
      error = path + ": instance " + std::to_string(i) + " names no known mesh";
      return false;
    }
    if (entry.ground && !out.terrain.enabled) {
      error = path + ": instance " + std::to_string(i) + " stands on a terrain the scene has not";
      return false;
    }
    SceneInstance instance;
    instance.mesh = entry.mesh;
    instance.transform.position = entry.translation;
    if (entry.ground)
      instance.transform.position.y += ground_at(entry.translation.x, entry.translation.z);
    instance.transform.rotation =
        normalize(quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(entry.yaw_deg)) *
                  normalize(entry.rotation));
    if (entry.scale.has_value()) instance.transform.scale = *entry.scale;
    // {"animation":{"clip":"Run","speed":1.5,"phase":0.4}} — read whole and handed on unread.
    // An empty object is a legal block and means "the skin's first clip at speed 1".
    if (entry.animation.has_value()) {
      instance.animation.play = true;
      instance.animation.clip = entry.animation->clip;
      instance.animation.speed = entry.animation->speed;
      instance.animation.phase = entry.animation->phase;
    }
    out.instances.push_back(instance);
  }
  // Scatters, expanded here from their seeds, so the renderer sees plain instances and two
  // machines place the same ones.
  for (u32 s = 0; s < file.scatters.size(); ++s) {
    const scene::Scatter& scatter = file.scatters[s];
    if (scatter.mesh >= file_meshes) {
      error = path + ": scatter " + std::to_string(s) + " names no known mesh";
      return false;
    }
    if (scatter.ground && !out.terrain.enabled) {
      error = path + ": scatter " + std::to_string(s) + " stands on a terrain the scene has not";
      return false;
    }
    u64 state = static_cast<u64>(scatter.seed) * 0x9E3779B97F4A7C15ull + s;
    for (u32 k = 0; k < scatter.count; ++k) {
      // Uniform over the annulus's area, not its radius, so the middle is not crowded.
      const f32 angle = seeded_unit(state) * 2.0f * k_pi;
      const f32 r0 = scatter.radius_min * scatter.radius_min;
      const f32 r1 = scatter.radius_max * scatter.radius_max;
      const f32 radius = std::sqrt(r0 + (r1 - r0) * seeded_unit(state));
      const f32 scale =
          scatter.scale_min + (scatter.scale_max - scatter.scale_min) * seeded_unit(state);
      const f32 yaw = seeded_unit(state) * 2.0f * k_pi;
      SceneInstance instance;
      instance.mesh = scatter.mesh;
      const f32 x = scatter.center.x + std::cos(angle) * radius;
      const f32 z = scatter.center.y + std::sin(angle) * radius;
      instance.transform.position =
          Vec3{x, scatter.y + (scatter.ground ? ground_at(x, z) : 0.0f), z};
      instance.transform.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, yaw);
      instance.transform.scale = Vec3{scale, scale, scale};
      out.instances.push_back(instance);
    }
  }
  // Ruins, assembled from their kits by the ruins capability, when this build has it.
  if (!file.ruins.empty()) {
#if ENGINE_RENDERER_RUINS
    if (!expand_ruins(file, path, dir, out, error)) return false;
#else
    error = path +
            ": the scene names ruins, and this build has no ruins capability "
            "(ENGINE_WITH_RUINS=OFF or ENGINE_MINIMAL=ON)";
    return false;
#endif
  }
  if (file.instances.empty() && file.scatters.empty() && file.ruins.empty()) {
    for (u32 i = 0; i < file_meshes; ++i)
      out.instances.push_back(SceneInstance{i, Transform3::identity()});
  }
  // The terrain is the last mesh, with one identity instance after everything the file placed,
  // so every index the file used still names what it named.
  if (out.terrain.enabled) {
    // After the file's meshes and any ruin kit's, so its index is the count so far.
    const u32 terrain_mesh = out.meshes.size();
    out.meshes.push_back(std::string());
    SceneMeshInfo info;
    info.name = "terrain";
    info.origin = "terrain";
    out.mesh_info.push_back(std::move(info));
    out.instances.push_back(SceneInstance{terrain_mesh, Transform3::identity()});
  }
  return true;
}

bool load_scene(const SceneDesc& desc, SceneData& out, std::string& error) {
  if (desc.meshes.empty()) {
    error = "a scene needs at least one mesh";
    return false;
  }
  SceneDesc resolved = desc;
  // The derived-data cache lives at <repo>/ddc, found by walking up from the executable to the
  // directory that holds AGENTS.md, unless a root was named. With no root there is nowhere to
  // keep a built mesh, so the cache is simply off.
  if (resolved.cache && resolved.ddc.empty()) {
    resolved.ddc = geometry::find_ddc_root(platform::executable_directory());
    if (resolved.ddc.empty()) {
      resolved.cache = false;
      ENGINE_LOG_WARN(log_renderer, "no derived-data root above the executable",
                      log::field("from", platform::executable_directory()));
    }
  }

  const i64 build_start = time::monotonic_ns();
  // `heightfield` is what makes the GPU scene synthesize the ripple texture and the height-banded
  // materials, so it is the *terrain* and not "procedural": the shredded atlas brings its own
  // material and its own image and goes down the ordinary path.
  out.heightfield = resolved.meshes.size() == 1 && resolved.meshes[0].empty() &&
                    resolved.procedural == Procedural::heightfield && !resolved.terrain.enabled;
  const bool described = resolved.mesh_info.size() == resolved.meshes.size();
  if (!resolved.mesh_info.empty() && !described) {
    error = "SceneDesc::mesh_info must be empty or parallel to meshes";
    return false;
  }
  out.sources.resize(resolved.meshes.size());
  Vector<geometry::ClusterLodMesh> dags(resolved.meshes.size());
  Vector<geometry::ClusterPages> tables(resolved.meshes.size());
  for (u32 m = 0; m < resolved.meshes.size(); ++m) {
    const u64 expected = described ? resolved.mesh_info[m].hash : 0;
    if (!load_source_mesh(resolved.meshes[m], resolved, expected, out.sources[m], dags[m],
                          tables[m], error)) {
      if (described && !resolved.mesh_info[m].name.empty())
        error = resolved.mesh_info[m].name + ": " + error;
      return false;
    }
  }
  out.terrain = resolved.terrain;
  out.mesh_info = resolved.mesh_info;
  out.name = resolved.name;
  out.file_hash = resolved.file_hash;
  out.mesh_primitives = out.sources[0].primitives;
  out.mesh_cache = out.sources[0].cache;
  // Streaming needs one page table over the whole scene, and it is a different merge: a paged mesh
  // is ordered coarse to fine, so pulling its leaves to the front — which the default merge does,
  // to make a direct draw of the leaves one range — would leave every one of its pages naming
  // clusters that are no longer beside each other. `merge_paged_cluster_meshes` keeps the order and
  // shifts the table ([geometry](geometry.md), "A scene of paged meshes"). A mesh whose table could
  // not be built takes the whole scene off the streaming path rather than half of it.
  bool paged = resolved.stream;
  for (const geometry::ClusterPages& table : tables)
    paged = paged && !table.pages.empty();
  if (paged) {
    if (!geometry::merge_paged_cluster_meshes(
            std::span<const geometry::ClusterLodMesh>(dags.data(), dags.size()),
            std::span<const geometry::ClusterPages>(tables.data(), tables.size()), out.lod,
            out.parts, out.pages, &error)) {
      return false;
    }
  } else if (dags.size() == 1) {
    out.lod = std::move(dags[0]);
    geometry::ClusterMeshPart part;
    part.cluster_count = out.lod.mesh.clusters.size();
    part.leaf_cluster_count = out.lod.level_cluster_counts[0];
    // **Where the leaves are has to be found, not assumed.** This branch takes a DAG as it
    // arrived, and a DAG read from a `.clusters` container arrived in **page order** — coarse
    // first, leaves last ([geometry](geometry.md), "Pages and streaming") — whether or not anyone
    // asked to stream from it, because the page layout is part of the derived-data cache key and
    // every container is written that way. Leaving `first_leaf_cluster` at zero therefore pointed
    // every consumer of it at the *coarsest* clusters of the mesh. The camera is the one that
    // showed it: it frames the leaves' bounds, a coarse cluster's sphere is looser than the
    // surface it stands for, and the looseness depends on how far the simplifier got — so two
    // builds of one mesh that differ only in their coarse levels drew it at two different sizes.
    // Found comparing a seam-aware LOD build against a position-only one, whose pictures
    // disagreed by 0.23 FLIP at the *finest* threshold, where they draw the same triangles.
    part.first_leaf_cluster = 0;
    for (u32 i = 0; i < out.lod.lod.size(); ++i) {
      if (out.lod.lod[i].level == 0) {
        part.first_leaf_cluster = i;
        break;
      }
    }
    part.quant_origin = out.lod.mesh.quant_origin;
    part.quant_scale = out.lod.mesh.quant_scale;
    // **The mesh's channels are the scene's channels**, and this branch has to say so because
    // nothing else will: `merge_cluster_meshes` fills `first_morph_channel`/`morph_channel_count`
    // for every part it concatenates, and a single mesh never goes through it. Left at zero, the
    // GPU scene gave the instance no morph stages and no deform record while `resolve_settings`
    // (which reads the stream, not the part) had turned the deform pass on — so it sized the pool
    // for zero vertices and every single morphed mesh failed to load with `vmaCreateBuffer:
    // VK_ERROR_INITIALIZATION_FAILED`. Found by the first real files through it, the Khronos morph
    // samples; the renderer's own morph test builds its `SceneData` by hand, sets the part's
    // channel run itself, and so had never been through `load_scene` at all.
    part.first_morph_channel = 0;
    part.morph_channel_count = out.lod.mesh.morph_channels.size();
    // And the same for the id space, for the same reason: only `merge_cluster_meshes` fills it.
    part.vertex_id_source = out.lod.mesh.vertex_ids.empty() ? geometry::VertexIdSource::none
                                                            : out.lod.mesh.vertex_id_source;
    out.parts.push_back(part);
  } else {
    // **The merge reorders a mesh, and its material map has to follow.** The scene convention
    // puts each mesh's leaves first, and every mesh that came through the cache arrived in page
    // order, leaves last — so the merge moves nearly every one of its clusters, and a
    // `part_of_cluster` left in the mesh's own order gives each merged cluster the material of
    // whichever cluster used to sit where it now sits. That drew rock on the dunes and metal on
    // the FlightHelmet's leather in every scene of more than one mesh from 2026-09-17 until the
    // first interactive session found it
    // (docs/experiments/first-interactive-session-2026-09-24.md): patches that came and went as the
    // LOD cut moved, because each cut drew other clusters.
    Vector<u32> source_of_cluster;
    if (!geometry::merge_cluster_meshes(dags, out.lod, out.parts, &error,
                                        geometry::ClusterOrder::leaves_first, &source_of_cluster)) {
      return false;
    }
    for (u32 m = 0; m < out.parts.size(); ++m) {
      const geometry::ClusterMeshPart& part = out.parts[m];
      geometry::permute_cluster_array(
          std::span<const u32>(source_of_cluster.data() + part.first_cluster, part.cluster_count),
          out.sources[m].part_of_cluster);
    }
  }

  // Instances: the caller's, the grid's, or one identity instance of the one mesh.
  Vector<SceneInstance> instances = resolved.instances;
  if (resolved.grid_instances > 1) {
    instances.clear();
    Vec3 mesh_center{};
    f32 mesh_radius = 1.0f;
    mesh_bounds(out.lod, out.parts[0].first_cluster + out.parts[0].first_leaf_cluster,
                out.parts[0].leaf_cluster_count, mesh_center, mesh_radius);
    make_instance_grid(resolved.grid_instances, mesh_radius, resolved.grid_joints, 0.0f, instances);
  } else if (instances.empty()) {
    instances.push_back(SceneInstance{0, Transform3::identity()});
  }
  // Each fitted mesh's own transform, from the box of its vertices — every level's vertices, which
  // for an edge-collapse DAG are a subset of the source's, so the box is the source's box. It is
  // read here, while the merged float positions still exist (a streamed scene releases them once
  // its page source is attached).
  Vector<Mat4> fit_of_mesh(out.parts.size(), Mat4::identity());
  if (described) {
    for (u32 m = 0; m < out.parts.size(); ++m) {
      const MeshFit& fit = resolved.mesh_info[m].fit;
      if (!fit.active()) continue;
      const u32 first = out.parts[m].first_vertex;
      const u32 end =
          m + 1 < out.parts.size() ? out.parts[m + 1].first_vertex : out.lod.mesh.vertices.size();
      Vec3 lo{1e30f, 1e30f, 1e30f};
      Vec3 hi{-1e30f, -1e30f, -1e30f};
      for (u32 v = first; v < end; ++v) {
        const Vec3 p = out.lod.mesh.vertices[v];
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
      const Vec3 size = hi - lo;
      const f32 longest = std::max(size.x, std::max(size.y, size.z));
      const f32 scale = fit.height > 0.0f ? (size.y > 0.0f ? fit.height / size.y : 1.0f)
                                          : (longest > 0.0f ? fit.extent / longest : 1.0f);
      const Vec3 anchor{0.5f * (lo.x + hi.x), fit.ground ? lo.y : 0.5f * (lo.y + hi.y),
                        0.5f * (lo.z + hi.z)};
      fit_of_mesh[m] = scaling(Vec3{scale, scale, scale}) * translation(-anchor);
    }
  }
  u32 pair_count = 0;
  const u32 palette = out.lod.mesh.skin_joint_count;
  for (const SceneInstance& source : instances) {
    if (source.mesh >= out.parts.size()) {
      error = "an instance names mesh " + std::to_string(source.mesh) + ", which the scene has no";
      return false;
    }
    gfx::InstanceDesc instance{};
    gfx::set_instance_transform(instance,
                                mat4_from_transform(source.transform) * fit_of_mesh[source.mesh]);
    instance.mesh = source.mesh;
    instance.first_pair = pair_count;
    // The cull pass inflates every sphere of a deformed instance by this, in the instance's own
    // space (gfx::InstanceDesc::bounds_padding). It is the caller's number: only the caller knows
    // which clips this instance will play.
    instance.bounds_padding = source.bounds_padding;
    pair_count += out.parts[source.mesh].cluster_count;
    // An instance may only be skinned by a mesh that carries a binding stream, and never by more
    // joints than that stream's palette: a binding names a palette slot with one byte, so a
    // longer array would be addressed by indices that cannot exist. Both are stated rather than
    // silently clamped, because either one means the caller and the content disagree.
    u32 joints = source.joints;
    if (joints > 0 && out.lod.mesh.skin.empty()) {
      error = "an instance is skinned, but its mesh carries no skin binding stream";
      return false;
    }
    if (joints > palette) {
      error = "an instance names " + std::to_string(joints) +
              " joints, but the mesh's palette is " + std::to_string(palette);
      return false;
    }
    out.instance_joints.push_back(joints);
    out.skinned_instances += joints > 0 ? 1u : 0u;
    out.instances.push_back(instance);
  }
  out.pair_count = pair_count;
  out.max_joints = resolved.max_joints;
  if (out.skinned_instances == 0) out.instance_joints.clear();

  update_scene_bounds(out);
  out.build_ns = time::monotonic_ns() - build_start;
  return true;
}

std::span<const u32> mesh_vertex_ids(const SceneData& scene, u32 mesh) noexcept {
  if (mesh >= scene.parts.size()) return {};
  const geometry::ClusterMeshPart& part = scene.parts[mesh];
  const Vector<u32>& ids = scene.lod.mesh.vertex_ids;
  if (part.vertex_id_source == geometry::VertexIdSource::none || ids.empty()) return {};
  // A part records where its vertices start, not how many it has: the next part's start is where
  // they end. The id stream's own length stands in for the last part's end, because a streamed
  // scene has released `vertices` and kept the ids.
  const u32 first = part.first_vertex;
  const u32 end = mesh + 1 < scene.parts.size() ? scene.parts[mesh + 1].first_vertex : ids.size();
  if (first > end || end > ids.size()) return {};
  return std::span<const u32>(ids.data() + first, end - first);
}

void update_scene_bounds(SceneData& out) {
  // The plain heightfield keeps the half extent it has always used, so a run with no mesh gets
  // exactly the camera it always did.
  if (out.heightfield && out.instances.size() == 1) {
    out.center = Vec3{};
    out.radius = 10.0f;
  } else {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    Vector<Vec3> centers(out.parts.size());
    Vector<f32> radii(out.parts.size(), 0.0f);
    for (u32 m = 0; m < out.parts.size(); ++m) {
      // `first_leaf_cluster` is not decoration: a **paged** mesh is ordered coarse to fine, so its
      // leaves are at the *end* of its run and `first_cluster` alone names the coarsest clusters
      // instead. Their spheres cover the same surface but not the same volume — a coarse cluster's
      // sphere is loose — so framing them gave a camera distance that depended on the shape of the
      // DAG. Two builds of one mesh that differ only in how far they could simplify then drew it
      // at two different sizes, which is how this was found: comparing a seam-aware LOD build
      // against a position-only one, the pictures disagreed by 0.23 FLIP at the *finest* threshold,
      // where they draw exactly the same triangles.
      mesh_bounds(out.lod, out.parts[m].first_cluster + out.parts[m].first_leaf_cluster,
                  out.parts[m].leaf_cluster_count, centers[m], radii[m]);
    }
    // A skinned instance's bounds are its bind pose's plus the displacement bound, for the same
    // reason the cull pass inflates its spheres: the camera has to frame the character wherever
    // the clip puts it, not only where it rests.
    auto instance_radius = [&](const gfx::InstanceDesc& instance) {
      return (radii[instance.mesh] + instance.bounds_padding) * instance.scale_max;
    };
    for (const gfx::InstanceDesc& instance : out.instances) {
      const Vec3 c = transform_point(instance.world, centers[instance.mesh]);
      const f32 r = instance_radius(instance);
      lo = Vec3{std::min(lo.x, c.x - r), std::min(lo.y, c.y - r), std::min(lo.z, c.z - r)};
      hi = Vec3{std::max(hi.x, c.x + r), std::max(hi.y, c.y + r), std::max(hi.z, c.z + r)};
    }
    out.center = (lo + hi) * 0.5f;
    out.radius = 1e-6f;
    for (const gfx::InstanceDesc& instance : out.instances) {
      const Vec3 c = transform_point(instance.world, centers[instance.mesh]);
      out.radius = std::max(out.radius, length(c - out.center) + instance_radius(instance));
    }
  }
}

}  // namespace engine::renderer
