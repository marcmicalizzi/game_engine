// The content build as a library (docs/subsystems/content_build.md). The code moved here from
// apps/engine_content/main.cpp unchanged, so that engine-host's `content.build` runs the build
// engine-content runs rather than a second one: the rules, the job layout and the skip are the
// command line's, and its tests (apps/engine_content/tests) are this module's end-to-end tests.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/geometry/cluster_file.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace engine::content_build {

namespace {

// The same category engine-content's own records use, so `--log content=debug` reaches both.
ENGINE_LOG_CATEGORY_DEFINE(log_content, "content");

u64 split_mix(u64 z) noexcept {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// The build key of a container built with `--no-morph`: the ordinary key, moved by a constant
// through one SplitMix64 round. `geometry::cluster_cache_key` has no morph term because nothing
// else builds without the channels; this keeps such a container from recording the key a full
// build would record, which is what `build-all` skips on.
u64 morph_key(u64 key, const MeshOptions& options) noexcept {
  if (options.morph) return key;
  return split_mix(key ^ 0x6e6f2d6d6f727068ull);  // "no-morph"
}

// And of one built with `--atlas repack`: moved by the atlas step's own key, which covers every
// option that changes its bytes and the module's version (`atlas::repack_options_key`). `keep`
// leaves the key alone, so every container built before the flag existed is still the answer to
// the question it answered — which is why this is a mix here and not a new term in
// `geometry::cluster_cache_key`, whose every existing key would have moved with it.
u64 atlas_key(u64 key, const MeshOptions& options) noexcept {
  if (!options.repack) return key;
  return split_mix(key ^ 0x6174726570616b21ull ^ atlas::repack_options_key(options.atlas));
}

}  // namespace

bool read_atlas_mode(std::string_view text, bool& repack) {
  if (text == "keep")
    repack = false;
  else if (text == "repack")
    repack = true;
  else
    return false;
  return true;
}

bool read_normal_map_mode(std::string_view text, atlas::NormalMapMode& out) {
  if (text == "convert")
    out = atlas::NormalMapMode::convert;
  else if (text == "resample")
    out = atlas::NormalMapMode::resample;
  else
    return false;
  return true;
}

geometry::ClusterLodOptions lod_options_of(const MeshOptions& options) {
  geometry::ClusterLodOptions lod_options;
  lod_options.max_triangles = options.max_triangles;
  lod_options.max_vertices = options.max_vertices;
  lod_options.normal_weight = options.normal_weight;
  lod_options.uv_weight = options.uv_weight;
  lod_options.uv_seams = options.uv_seams;
  lod_options.normal_seams = options.normal_seams;
  return lod_options;
}

geometry::ClusterLodOptions import_lod_options_of(const MeshOptions& options) {
  geometry::ClusterLodOptions lod_options = lod_options_of(options);
  lod_options.loose_islands = options.loose_islands;
  return lod_options;
}

void measure_mask_covers(const assets::MeshData& mesh, std::string_view image_dir,
                         Vector<f32>& out) {
  out.clear();
  out.resize(mesh.primitives.size(), 0.0f);
  // One decode a material: its base colour's alpha, kept while its primitives are measured.
  i32 decoded_material = -1;
  image::Image alpha;
  bool alpha_ok = false;
  for (u32 p = 0; p < mesh.primitives.size(); ++p) {
    const assets::Primitive& primitive = mesh.primitives[p];
    if (primitive.material < 0 || static_cast<u32>(primitive.material) >= mesh.materials.size())
      continue;
    const assets::Material& material = mesh.materials[static_cast<u32>(primitive.material)];
    if (material.alpha_mode != assets::k_alpha_mask || primitive.index_count < 3) continue;
    if (decoded_material != primitive.material) {
      decoded_material = primitive.material;
      alpha = image::Image{};
      alpha_ok = false;
      const i32 index = material.base_color_image;
      if (index >= 0 && static_cast<u32>(index) < mesh.images.size()) {
        const assets::ImageRef& ref = mesh.images[static_cast<u32>(index)];
        // Four channels and keep A: a one-channel request is the decoder's grey, not alpha.
        image::Image rgba;
        if (!ref.bytes.empty()) {
          alpha_ok = image::decode_image({ref.bytes.data(), ref.bytes.size()}, rgba, 4);
        } else if (!ref.uri.empty()) {
          const std::string file = image_dir.empty() || io::is_absolute_path(ref.uri)
                                       ? ref.uri
                                       : io::join_path(image_dir, ref.uri);
          alpha_ok = image::read_image(file, rgba, 4) == io::Status::Ok;
        }
        if (alpha_ok) {
          const u32 texels = rgba.width * rgba.height;
          alpha.width = rgba.width;
          alpha.height = rgba.height;
          alpha.channels = 1;
          alpha.pixels.resize(texels);
          for (u32 t = 0; t < texels; ++t)
            alpha.pixels[t] = rgba.pixels[t * 4 + 3];
        }
      }
    }
    const geometry::TextureTransform& transform = material.sampling[0].transform;
    const bool by_uv = transform.rotation == 0.0f && mesh.uvs.size() == mesh.positions.size();
    const std::span<const u32> range(mesh.indices.data() + primitive.first_index,
                                     primitive.index_count);
    out[p] = geometry::measure_mask_cover(
        std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
        by_uv ? std::span<const Vec2>(mesh.uvs.data(), mesh.uvs.size()) : std::span<const Vec2>(),
        range,
        alpha_ok ? std::span<const u8>(alpha.pixels.data(), alpha.pixels.size())
                 : std::span<const u8>(),
        alpha.width, alpha.height, material.base_color.w, material.alpha_cutoff, transform.scale,
        transform.offset);
  }
}

namespace {

// A generated mesh's levels: the same options without the loose-island rule, which is an
// import's until a generator asks for it (geometry.md, "Generators: what each would gain"), so a
// generated container and its key are what they were. `generator_loose_islands` is the
// measurement that page's table was taken with, and nothing that ships sets it.
geometry::ClusterLodOptions generator_lod_options_of(const MeshOptions& options) {
  geometry::ClusterLodOptions lod_options = lod_options_of(options);
  lod_options.loose_islands = options.generator_loose_islands;  // false but for a measurement
  return lod_options;
}

}  // namespace

const materials::Pack* repository_pack(std::string_view executable_dir) {
  return materials::pack_at(materials::default_pack_dir(executable_dir));
}

void attach_repository_pack(std::string_view source, std::string_view executable_dir,
                            MeshOptions& options) {
  if (!assets::mesh_generator_of(source).empty())
    options.materials = repository_pack(executable_dir);
}

// Every key a build records: the geometry's, then the two steps outside it.
u64 build_key_of(u64 source_hash, const MeshOptions& options) noexcept {
  return atlas_key(
      morph_key(geometry::cluster_cache_key(source_hash, import_lod_options_of(options),
                                            options.weld, options.page_bytes),
                options),
      options);
}

bool read_seam_rule(std::string_view text, geometry::SeamRule& out) {
  if (text == "none")
    out = geometry::SeamRule::none;
  else if (text == "protect")
    out = geometry::SeamRule::protect;
  else if (text == "lock")
    out = geometry::SeamRule::lock;
  else
    return false;
  return true;
}

bool options_in_range(const MeshOptions& options) {
  return options.max_triangles >= 4 && options.max_triangles <= 256 && options.max_vertices >= 1 &&
         options.max_vertices <= 255;
}

namespace {

// A UV that far outside the unit square is a broken unwrap or a unit mix-up, not tiling: 16
// wraps is already a texel density no texture pipeline can serve. A warning, not an error,
// because tiling trim sheets legitimately run to a few wraps.
constexpr f32 k_uv_limit = 16.0f;

struct Diagnostics {
  Vector<Diagnostic> warnings;
  Diagnostic error;  // the first one; `rule` is empty while there is none
  // The first triangle that repeats an index, named as the source numbers it. It is an error, but
  // one reported after the zero-area repair has run, so that its sentence can say what the repair
  // removed and that this one is not of that kind (`build_one`).
  Diagnostic repeated;
};

bool is_finite(Vec3 p) noexcept {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool fail_rule(Diagnostics& out, const char* rule, std::string message) {
  out.error.rule = rule;
  out.error.message = std::move(message);
  return false;
}

// Checks the mesh as it was loaded, before welding, so that a primitive and a triangle in a
// diagnostic are the ones in the source file. Returns false on the first error.
bool validate_mesh(const assets::MeshData& mesh, const std::string& source, Diagnostics& out) {
  const u32 vertex_count = mesh.positions.size();
  const bool has_uvs = mesh.uvs.size() == vertex_count && vertex_count != 0;
  for (u32 p = 0; p < mesh.primitives.size(); ++p) {
    const assets::Primitive& primitive = mesh.primitives[p];
    const std::string where = "primitive " + std::to_string(p) + " of '" + source + "'";
    if (primitive.index_count == 0)
      return fail_rule(out, k_rule_empty_primitive, where + " has no triangles");
    if (primitive.index_count % 3 != 0) {
      return fail_rule(out, k_rule_empty_primitive,
                       where + " has " + std::to_string(primitive.index_count) +
                           " indices, which is not a whole number of triangles");
    }
    f32 uv_extreme = 0.0f;
    for (u32 t = 0; t * 3 < primitive.index_count; ++t) {
      const u32 corner[3] = {mesh.indices[primitive.first_index + t * 3 + 0],
                             mesh.indices[primitive.first_index + t * 3 + 1],
                             mesh.indices[primitive.first_index + t * 3 + 2]};
      for (const u32 index : corner) {
        if (index >= vertex_count) {
          return fail_rule(out, k_rule_index_range,
                           where + ", triangle " + std::to_string(t) + ": index " +
                               std::to_string(index) + " is outside the " +
                               std::to_string(vertex_count) + " vertices");
        }
      }
      for (const u32 index : corner) {
        if (!is_finite(mesh.positions[index])) {
          return fail_rule(out, k_rule_nan_position,
                           where + ", triangle " + std::to_string(t) + ": vertex " +
                               std::to_string(index) + " has a position that is not finite");
        }
      }
      // Degenerate topologically: a repeated corner, which is an indexing fault and stays an
      // error. It is reported after the zero-area repair (`Diagnostics::repeated`). A triangle
      // degenerate only geometrically — three distinct corners whose positions span no area — is
      // not checked here any more: the zero-area repair drops it, by this same test
      // (`geometry::spans_zero_area`), and reports what it dropped (geometry.md, "Zero-area
      // triangles: the repair").
      const bool repeated =
          corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2];
      if (repeated && *out.repeated.rule == '\0') {
        out.repeated.rule = k_rule_degenerate;
        out.repeated.message = where + ", triangle " + std::to_string(t) +
                               " is degenerate: indices " + std::to_string(corner[0]) + " " +
                               std::to_string(corner[1]) + " " + std::to_string(corner[2]) +
                               " repeat a vertex";
      }
      if (has_uvs) {
        for (const u32 index : corner) {
          const Vec2 uv = mesh.uvs[index];
          if (std::isfinite(uv.x) && std::abs(uv.x) > std::abs(uv_extreme)) uv_extreme = uv.x;
          if (std::isfinite(uv.y) && std::abs(uv.y) > std::abs(uv_extreme)) uv_extreme = uv.y;
        }
      }
    }
    if (std::abs(uv_extreme) > k_uv_limit) {
      Diagnostic warning;
      warning.rule = k_rule_uv_range;
      warning.message = where + " has a UV coordinate of " + std::to_string(uv_extreme) +
                        ", far outside [-16, 16]";
      out.warnings.push_back(std::move(warning));
    }
  }

  // A material that names an image the build cannot get bytes for draws untextured, which is
  // worth saying out loud but is not a reason to refuse the mesh.
  const std::string dir(io::parent_path(source));
  for (u32 m = 0; m < mesh.materials.size(); ++m) {
    const assets::Material& material = mesh.materials[m];
    const i32 slots[5] = {material.base_color_image, material.metallic_roughness_image,
                          material.normal_image, material.occlusion_image, material.emissive_image};
    for (const i32 slot : slots) {
      if (slot < 0) continue;
      const std::string named = "material " + std::to_string(m) + " of '" + source + "' names ";
      if (static_cast<u32>(slot) >= mesh.images.size()) {
        return fail_rule(out, k_rule_missing_image,
                         named + "image " + std::to_string(slot) + ", outside the " +
                             std::to_string(mesh.images.size()) + " images the file has");
      }
      const assets::ImageRef& image = mesh.images[static_cast<u32>(slot)];
      Diagnostic warning;
      warning.rule = k_rule_missing_image;
      if (image.uri.empty() && image.bytes.empty()) {
        warning.message = named + "image " + std::to_string(slot) +
                          ", which has neither a path "
                          "nor any bytes";
      } else if (!image.uri.empty() && !io::exists(io::join_path(dir, image.uri))) {
        warning.message = named + "image '" + image.uri + "', which is not there";
      } else {
        continue;
      }
      out.warnings.push_back(std::move(warning));
    }
  }
  return true;
}

// One primitive's DAG, as a job sees it. The result lives here rather than in a shared list, so
// nothing the jobs write is ordered by when they finish.
struct PartBuild {
  const assets::MeshData* mesh = nullptr;
  geometry::ClusterLodOptions options;  // the build's, with this primitive's mask cover
  const geometry::AttributeSource* attributes = nullptr;
  u32 first_index = 0;
  u32 index_count = 0;
  i32 material = -1;
  geometry::ClusterLodMesh out;
  std::string error;
  bool ok = false;
};

void build_part(void* data) {
  PartBuild& part = *static_cast<PartBuild*>(data);
  const std::span<const u32> range(part.mesh->indices.data() + part.first_index, part.index_count);
  part.ok = geometry::build_cluster_lod(part.mesh->positions, range, part.options, part.out,
                                        &part.error, *part.attributes);
}

}  // namespace

void collect_texture_sources(const geometry::ClusterFileData& data, std::string_view dir,
                             Vector<TextureSource>& out) {
  out.clear();
  for (u32 i = 0; i < data.textures.size(); ++i) {
    const geometry::ClusterFileTexture& record = data.textures[i];
    if (record.options == 0) continue;
    TextureSource source;
    source.image = i;
    source.options = record.options;
    if (i < data.images.size() && !data.images[i].bytes.empty()) {
      source.bytes = data.images[i].bytes;
    } else if (i < data.image_paths.size() && !data.image_paths[i].empty()) {
      source.file = dir.empty() ? data.image_paths[i] : io::join_path(dir, data.image_paths[i]);
    } else {
      continue;  // an image with neither bytes nor a path: validation has already said so
    }
    out.push_back(std::move(source));
  }
}

// Imports, validates, clusters, and writes one mesh. `pool` is where the per-primitive DAG
// builds run; null builds them one after another on the calling thread, which is what `build-all`
// does because it already has one job per mesh in flight and nesting the two levels would only
// deepen the wait stack. Either way the merge walks the primitives in index order.
namespace {

// The generator a generated mesh's path names (`assets/mesh_generator.h`), or null with the
// registry's sentence, which names the capability it comes with.
const assets::MeshGeneratorDesc* generator_for(const std::string& path, std::string& message) {
  const std::string_view name = assets::mesh_generator_of(path);
  const assets::MeshGeneratorRegistry& registry = assets::MeshGeneratorRegistry::global();
  const assets::MeshGeneratorDesc* generator = registry.find(name);
  if (generator == nullptr) message = registry.unknown(path, name);
  return generator;
}

// A generated mesh — a `.flora.json`, a `.rock.json` — built through its generator, which makes
// the whole container exactly as the renderer's scene reader has it made, so the two find each
// other's entries. The import, the repairs, the weld and the atlas are the glTF path's and do not
// apply: the generator's mesh is made the way it is meant to be.
bool build_generated(const std::string& input, const std::string& output,
                     const MeshOptions& options, u64 source_hash, u64 build_key, BuildResult& out,
                     Diagnostic& error, const SourceClasses* classes) {
  const i64 start_ns = time::monotonic_ns();
  out.path = output;
  out.source_hash = source_hash;
  out.build_key = build_key;
  std::string message;
  const assets::MeshGeneratorDesc* generator = generator_for(input, message);
  std::string text;
  const io::Status status = generator != nullptr ? io::read_file(input, text) : io::Status::Ok;
  if (generator == nullptr || status != io::Status::Ok) {
    error.rule = "source.unreadable";
    error.message = generator == nullptr
                        ? message
                        : "cannot read " + input + ": " + std::string(io::status_name(status));
    return false;
  }
  geometry::ClusterFileData data;
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
  // A vine's or a cover's host is read through the same seam the renderer's scene reader uses
  // (generator_hosts.h), under these options, so the two build one container.
  const geometry::ClusterLodOptions lod_options = generator_lod_options_of(options);
  const GeneratorHosts hosts(input, lod_options, options.page_bytes, options.materials);
  if (!generator->build(bytes, hosts.inputs(), lod_options, options.page_bytes, data, message)) {
    error.rule = "source.unreadable";
    error.message = input + ": " + message;
    return false;
  }
  data.source_path = input;
  // The generator keys what it made as the scene reader does; the class moves that key only when
  // there is one (C62), so compare the two after the same move, and record the moved one.
  const u64 moved_by = classes != nullptr ? classes->hash : 0;
  if (data.source_hash != source_hash ||
      content_class::classified_key(data.build_key, moved_by) != build_key) {
    error.rule = "source.unreadable";
    error.message = input + ": the generator built under another identity than it hashed";
    return false;
  }
  data.build_key = build_key;
  if (classes != nullptr) {
    data.content_classes = classes->record;
    if (classes->hash != 0)
      out.content_classes = content_class::classes_json(categories_of(options), classes->classes,
                                                        &classes->materials);
  }
  if (!geometry::write_cluster_file(output, data, &message)) {
    error.rule = "output.unwritable";
    error.message = message;
    return false;
  }
  // The images its parts sample (a recipe's sets, a material pack's), for the host's texture step.
  collect_texture_sources(data, io::parent_path(input), out.texture_sources);
  io::FileInfo info;
  const geometry::ClusterLodMesh& lod = data.mesh;
  out.clusters = lod.mesh.clusters.size();
  out.leaf_clusters = lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0];
  out.triangles = lod.leaf_triangle_count;
  out.lod_levels = lod.level_cluster_counts.size();
  out.vertices = lod.mesh.vertices.size();
  out.pages = data.pages.pages.size();
  out.page_bytes = options.page_bytes;
  out.materials = data.materials.size();
  out.images = data.image_paths.size();
  out.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
  out.hash = geometry::cluster_file_hash(data);
  out.build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  return true;
}

}  // namespace

bool source_hash_of(const std::string& path, u64& out, std::string* message) {
  if (assets::mesh_generator_of(path).empty()) return assets::source_mesh_hash(path, out, message);
  std::string why;
  const assets::MeshGeneratorDesc* generator = generator_for(path, why);
  std::string text;
  const io::Status status = generator != nullptr ? io::read_file(path, text) : io::Status::Ok;
  if (generator != nullptr && status != io::Status::Ok)
    why = "cannot read " + path + ": " + std::string(io::status_name(status));
  if (generator != nullptr && status == io::Status::Ok) {
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
    // Identity only: a host's source hash needs no LOD options.
    const GeneratorHosts hosts(path, geometry::ClusterLodOptions{}, 0, nullptr);
    if (assets::generated_source_hash(*generator, bytes, hosts.inputs(), out, why)) return true;
    why = path + ": " + why;
  }
  if (message != nullptr) *message = why;
  return false;
}

u64 mesh_build_key(const std::string& path, u64 source_hash, const MeshOptions& options) noexcept {
  // A generated mesh is keyed as the renderer's scene reader keys it: no weld (the generator's
  // vertices are shared where it means them), no morph or atlas step.
  if (!assets::mesh_generator_of(path).empty())
    return assets::generated_mesh_build_key(source_hash,
                                            assets::MeshGeneratorInputs{options.materials},
                                            generator_lod_options_of(options), options.page_bytes);
  return build_key_of(source_hash, options);
}

const content_class::Categories& categories_of(const MeshOptions& options) {
  static const content_class::Categories engine_only;
  return options.categories != nullptr ? *options.categories : engine_only;
}

namespace {

// Whether there is a sidecar at `sidecar` for the build to read: true and `present` false only
// when nothing of that name is there at all. The name itself is looked at, a link not followed:
// `io::exists` follows one and answers false for a link that leads nowhere and for a path whose
// look fails, and either is a sidecar the build failed to read, never "no sidecar", because what
// cannot be read is never `none` (ADR-0073; content_build.md, "Content classes"). False, with
// `why`, when the look itself fails.
bool sidecar_present(const std::string& sidecar, bool& present, std::string& why) {
  std::error_code ec;
  const std::filesystem::path p(
      std::u8string_view(reinterpret_cast<const char8_t*>(sidecar.data()), sidecar.size()));
  const std::filesystem::file_status st = std::filesystem::symlink_status(p, ec);
  present = st.type() != std::filesystem::file_type::not_found;
  if (!present) return true;  // not found is an answer, whatever `ec` says of it
  if (ec || st.type() == std::filesystem::file_type::none) {
    why = "cannot be looked at: " + ec.message();
    return false;
  }
  return true;
}

// A sidecar's text, or none: nothing of that name declares nothing, and anything else there is
// read (a link that leads nowhere fails the read, and so the source).
bool read_sidecar_text(const std::string& sidecar, std::string& text, bool& present,
                       std::string& why) {
  if (!sidecar_present(sidecar, present, why)) return false;
  if (!present) return true;
  const io::Status status = io::read_file(sidecar, text);
  if (status == io::Status::Ok) return true;
  why = "cannot be read: " + std::string(io::status_name(status));
  return false;
}

// Every set of a pack, through the same reader: a role's `pack.json` entry is read as a sidecar,
// its `content_classes` the set's class. The pack's file is parsed strictly, so a key given twice
// in a role is refused here as in a sidecar.
bool read_pack_classes(const materials::Pack& pack, const content_class::Categories& categories,
                       content_class::ContentClasses& out, std::string& why,
                       ClassReads* counts = nullptr) {
  const i64 start_ns = time::monotonic_ns();
  const std::string path = io::join_path(pack.dir, "pack.json");
  std::string text;
  const io::Status status = io::read_file(path, text);
  // Counted per set, every set a read of its class; the file's bytes once (C72).
  u32 sets = 0;
  u32 classified = 0;
  const auto count = [&] {
    if (counts == nullptr) return;
    ClassReadCount& c = (*counts)[ClassRead::pack_sets];
    c.asked += sets;
    c.found += classified;
    c.bytes += text.size();
    c.ns += time::monotonic_ns() - start_ns;
  };
  struct Counted {
    const decltype(count)& f;
    ~Counted() { f(); }
  } counted{count};
  if (status != io::Status::Ok) {
    why = path + " cannot be read: " + std::string(io::status_name(status));
    return false;
  }
  JsonParseOptions strict;
  strict.refuse_duplicate_keys = true;
  JsonValue document;
  const JsonParseResult parsed = parse_json(text, document, strict);
  const JsonValue* roles = parsed.ok && document.is_object() ? document.find("roles") : nullptr;
  // `roles` is an object, a role's name to its entry (materials.h, `read_pack`).
  if (!parsed.ok || roles == nullptr || !roles->is_object()) {
    why = path + ": not a pack's roles" +
          (parsed.duplicate_key.empty() ? "" : " (the key '" + parsed.duplicate_key + "' twice)");
    return false;
  }
  const JsonValue::Object& entries = roles->as_object();
  for (u32 r = 0; r < entries.size(); ++r) {
    const JsonValue& role = entries.value_at(r);
    ++sets;
    if (!role.is_object() || !role.contains("content_classes")) continue;
    ++classified;
    content_class::ContentClasses one;
    std::string error;
    if (!content_class::read_sidecar_classes(categories, write_json(role), one, error)) {
      why = path + " role '" + entries.key_at(r) + "': " + error;
      return false;
    }
    out = content_class::join(out, one);
  }
  return true;
}

// **The hosts a recipe's generator names** (C64): its key's hash run against hooks that read no
// host, each `identify` and each `load` recorded (resolved beside the recipe, as
// `GeneratorHosts::resolve` does), `identify` answered with zero and `load` refused, so only the
// recipe is read. A generator that reaches another mesh names
// it through this seam, because its key covers the host. A recipe with no generator registered,
// or a file that cannot be read, names none here: the build or the load reads the same file and
// fails on it with its own sentence. A recipe a scene hands as bytes (`handed`, no file needed) is
// probed from those bytes, the ones the load builds it from. False, with `why`, when the generator
// exists and the recipe reads but its hash refuses after these hooks answered it (one that parses
// an image it hashes, answered here with no bytes): the refusal may be the probe's, its hosts are
// then unknown, and the recipe is refused rather than decided without them (C64). A hash that
// refuses before asking a hook refuses the recipe itself (a recipe out of its generator's ranges),
// and the build or the load says so in its own words.
struct HostProbe {
  std::string dependant;
  Vector<std::string> hosts;
  u32 answered = 0;  // the hooks' answers the hash was given
};
// A host the hash asked for, resolved beside the recipe as `GeneratorHosts::resolve` does.
void probe_records(HostProbe& probe, std::string_view path) {
  ++probe.answered;
  const std::string_view dir = io::parent_path(probe.dependant);
  probe.hosts.push_back(io::is_absolute_path(path) || dir.empty() ? std::string(path)
                                                                  : io::join_path(dir, path));
}
bool probe_identify(void* context, std::string_view path, u64& out, std::string&) {
  probe_records(*static_cast<HostProbe*>(context), path);
  out = 0;
  return true;
}
// Recorded as `identify` records one, whether or not the hash goes on without the answer: a hash
// that asks for a host is over it, and its recipe is decided with it (C64).
bool probe_load(void* context, std::string_view path, assets::GeneratorHostMesh&,
                std::string& error) {
  probe_records(*static_cast<HostProbe*>(context), path);
  error = "the class probe reads no host";
  return false;
}
bool probe_image(void* context, std::string_view, Vector<u8>& out, std::string&) {
  ++static_cast<HostProbe*>(context)->answered;
  out.clear();
  return true;
}
bool probe_hash(void* context, std::string_view, u64& out, std::string&) {
  ++static_cast<HostProbe*>(context)->answered;
  out = 0;
  return true;
}
bool generator_hosts(const std::string& recipe, const materials::Pack* pack,
                     Vector<std::string>& hosts, std::string& why,
                     const std::string* handed = nullptr, ClassReads* counts = nullptr) {
  hosts.clear();
  const assets::MeshGeneratorDesc* generator =
      assets::MeshGeneratorRegistry::global().find(assets::mesh_generator_of(recipe));
  // A generator with no host-aware hash never sees the hooks (`generated_source_hash` hands its
  // bytes to `source_hash` alone), so its recipes name no host: the answer the probe would give
  // after reading and hashing the file, known without either (C72).
  if (generator == nullptr || generator->source_hash_with == nullptr) return true;
  std::string read;
  if (handed == nullptr) {
    const i64 read_ns = time::monotonic_ns();
    const bool found = io::read_file(recipe, read) == io::Status::Ok;
    if (counts != nullptr)
      counts->add(ClassRead::recipe_file, found, read.size(), time::monotonic_ns() - read_ns);
    if (!found) return true;
  }
  const std::string& text = handed != nullptr ? *handed : read;
  const i64 probe_ns = time::monotonic_ns();
  struct Counted {
    ClassReads* counts;
    i64 start;
    const Vector<std::string>& hosts;
    ~Counted() {
      if (counts != nullptr)
        counts->add(ClassRead::host_probe, !hosts.empty(), 0, time::monotonic_ns() - start);
    }
  } counted{counts, probe_ns, hosts};
  HostProbe probe{recipe, {}};
  assets::MeshGeneratorHosts hooks;
  hooks.context = &probe;
  hooks.identify = probe_identify;
  hooks.load = probe_load;
  hooks.read_image = probe_image;
  hooks.hash_image = probe_hash;
  u64 hash = 0;
  if (!assets::generated_source_hash(
          *generator, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()),
          assets::MeshGeneratorInputs{pack, &hooks}, hash, why)) {
    if (probe.answered == 0) return true;  // the recipe's own refusal, said where it is read
    why =
        recipe + ": its generator's hash refuses the class probe, so its hosts are unknown: " + why;
    return false;
  }
  hosts = std::move(probe.hosts);
  return true;
}

// A container host's own section 54, its bytes (empty when it has none). False, with `why`, when it
// cannot be opened or read: a host that cannot be opened here (missing, locked, half written) is
// unreadable, never `none`, since the dependant is built only when the host's own read succeeds, so
// refusing it loses nothing, where `none` would build over whatever the finished file's section 54
// declares.
bool read_host_record(const std::string& path, Vector<u8>& record, std::string& why,
                      ClassReads* counts = nullptr) {
  record.clear();
  bool opened = false;
  struct Counted {
    ClassReads* counts;
    i64 start;
    const bool& opened;
    const Vector<u8>& record;
    ~Counted() {
      if (counts != nullptr)
        counts->add(ClassRead::host_record, opened, record.size(), time::monotonic_ns() - start);
    }
  } counted{counts, time::monotonic_ns(), opened, record};
  geometry::ClusterFileReader reader;
  std::string error;
  opened = reader.open(path, nullptr, &error);
  if (!opened) {
    why = path + ": the host cannot be opened for its classes: " + error;
    return false;
  }
  const u64 count = reader.element_count(geometry::ClusterSection::ContentClasses);
  if (count == 0) return true;
  u64 offset = 0;
  u64 bytes = 0;
  bool read = reader.range(geometry::ClusterSection::ContentClasses, 1u, 0, count, offset, bytes);
  if (read) {
    record.resize(static_cast<u32>(bytes));
    u64 got = 0;
    read =
        reader.file().read_at(offset, record.data(), bytes, got) == io::Status::Ok && got == bytes;
  }
  if (!read) {
    record.clear();
    why = path + ": its classes section cannot be read";
    return false;
  }
  return true;
}

// What a teaching round kept for `path` (`index` a path's hash to its entry), or null.
template <typename T>
const T* kept_at(const HashMap<u64, u32>& index, const Vector<std::string>& paths,
                 const Vector<T>& entries, const std::string& path) {
  const u32* at = index.find_value(hash_bytes(path.data(), path.size()));
  return at != nullptr && paths[*at] == path ? &entries[*at] : nullptr;
}

// Kept under `path` unless a path of the same hash already is (that one is then read again).
template <typename T>
void keep_at(HashMap<u64, u32>& index, Vector<std::string>& paths, Vector<T>& entries,
             const std::string& path, const T& entry) {
  if (!index.try_emplace(hash_bytes(path.data(), path.size()), static_cast<u32>(entries.size()))
           .second)
    return;
  paths.push_back(path);
  entries.push_back(entry);
}

// The key a probe made ahead is kept under: its path's hash, moved when its bytes were handed.
u64 ahead_key(const std::string& path, bool handed) noexcept {
  const u64 h = hash_bytes(path.data(), path.size());
  return handed ? hash_combine(h, 0x68616e646564ull) : h;
}

// The probe `probe_recipes_ahead` made of `path` (from its handed bytes or from its file), or null.
const HostReads::Ahead* ahead_at(const HostReads* reads, const std::string& path, bool handed) {
  if (reads == nullptr || reads->ahead.empty()) return nullptr;
  const u32* at = reads->ahead_of.find_value(ahead_key(path, handed));
  if (at == nullptr) return nullptr;
  const HostReads::Ahead& a = reads->ahead[*at];
  return a.path == path && a.handed == handed ? &a : nullptr;
}

// `generator_hosts`, answered from a probe made ahead when there is one (C72): the same function
// of the same bytes, computed earlier and in parallel.
bool probed_hosts(const std::string& recipe, const materials::Pack* pack, const HostReads* reads,
                  Vector<std::string>& hosts, std::string& why, const std::string* handed) {
  if (const HostReads::Ahead* a = ahead_at(reads, recipe, handed != nullptr)) {
    hosts = a->hosts;
    why = a->why;
    return a->ok;
  }
  return generator_hosts(recipe, pack, hosts, why, handed,
                         reads != nullptr ? reads->counts : nullptr);
}

// A recipe's hosts, from what the teaching round kept when it probed it, else probed here.
bool recipe_hosts(const std::string& recipe, const materials::Pack* pack, const HostReads* reads,
                  Vector<std::string>& hosts, std::string& why) {
  const HostReads::Probe* probe =
      reads == nullptr ? nullptr
                       : kept_at(reads->probe_of, reads->probe_paths, reads->probes, recipe);
  if (probe == nullptr) return probed_hosts(recipe, pack, reads, hosts, why, nullptr);
  hosts = probe->hosts;
  why = probe->why;
  return probe->ok;
}

// A container host's section 54, from what the teaching round kept, else read here.
bool host_record(const std::string& path, const HostReads* reads, Vector<u8>& record,
                 std::string& why) {
  const HostReads::Record* kept_record =
      reads == nullptr ? nullptr
                       : kept_at(reads->record_of, reads->record_paths, reads->records, path);
  if (kept_record == nullptr)
    return read_host_record(path, record, why, reads != nullptr ? reads->counts : nullptr);
  record = kept_record->bytes;
  why = kept_record->why;
  return kept_record->ok;
}

// A container host's class, its section 54 decoded against a copy of the build's table: a category
// the table does not hold, or holds under another ladder, cannot be the dependant's, and is
// refused.
bool host_container_classes(const std::string& path, const content_class::Categories& categories,
                            const HostReads* reads, content_class::ContentClasses& out,
                            std::string& why) {
  Vector<u8> record;
  if (!host_record(path, reads, record, why)) return false;
  if (record.empty()) return true;
  content_class::Categories copy = categories;
  std::string error;
  if (!content_class::decode_classes(copy, std::span<const u8>(record.data(), record.size()), out,
                                     error)) {
    why = path + ": its classes section cannot be read: " + error;
    return false;
  }
  if (copy.size() != categories.size()) {
    why = path + ": its classes name a category this build does not know";
    return false;
  }
  return true;
}

// The teaching round's walk: `read_classes_at`'s hosts in its order, each container host's record
// learned, and what was read kept for that read.
bool learn_hosts_at(const std::string& source, const MeshOptions& options,
                    content_class::Categories& learn, HostReads& reads, u32 depth,
                    const std::string* handed) {
  if (assets::mesh_generator_of(source).empty()) return true;
  if (kept_at(reads.probe_of, reads.probe_paths, reads.probes, source) == nullptr) {
    HostReads::Probe probe;
    probe.ok = probed_hosts(source, options.materials, &reads, probe.hosts, probe.why, handed);
    keep_at(reads.probe_of, reads.probe_paths, reads.probes, source, probe);
  }
  Vector<std::string> hosts;
  std::string why;
  if (!recipe_hosts(source, options.materials, &reads, hosts, why)) return false;
  for (const std::string& host : hosts) {
    if (depth >= k_host_depth || !learn_hosts_at(host, options, learn, reads, depth + 1, nullptr))
      return false;
    if (io::extension(host) != ".clusters") continue;
    if (kept_at(reads.record_of, reads.record_paths, reads.records, host) == nullptr) {
      HostReads::Record record;
      record.ok = read_host_record(host, record.bytes, record.why, reads.counts);
      keep_at(reads.record_of, reads.record_paths, reads.records, host, record);
    }
    Vector<u8> record;
    if (!host_record(host, &reads, record, why)) return false;
    content_class::ContentClasses taught;
    if (!record.empty() &&
        !content_class::decode_classes(learn, std::span<const u8>(record.data(), record.size()),
                                       taught, why))
      return false;
  }
  return true;
}

bool read_classes_at(const std::string& source, const MeshOptions& options, SourceClasses& out,
                     Diagnostic& error, u32 depth, const HostReads* reads);

}  // namespace

bool read_source_classes(const std::string& source, const MeshOptions& options, SourceClasses& out,
                         Diagnostic& error, const HostReads* reads) {
  return read_classes_at(source, options, out, error, 0, reads);
}

bool learn_source_hosts(const std::string& source, const MeshOptions& options,
                        content_class::Categories& learn, HostReads& reads,
                        const std::string* handed) {
  return learn_hosts_at(source, options, learn, reads, 0, handed);
}

namespace {

// One probe made ahead, in its own slot: what `generator_hosts` answers for its bytes, and what
// reading them cost, counted apart so no two threads write one count.
struct AheadJob {
  std::string path;
  const std::string* handed = nullptr;
  const materials::Pack* pack = nullptr;
  HostReads::Ahead out;
  ClassReads counts;
};

void run_ahead(void* data) {
  AheadJob& job = *static_cast<AheadJob*>(data);
  job.out.path = job.path;
  job.out.handed = job.handed != nullptr;
  job.out.ok =
      generator_hosts(job.path, job.pack, job.out.hosts, job.out.why, job.handed, &job.counts);
}

// Every job of `jobs` on `pool`, one runner a thread taking the next by an atomic index (never a
// job an item: a forest's recipes outnumber a queue, renderer.md "What the load reads"); with no
// pool, each in order on the calling thread.
void run_ahead_jobs(jobs::JobSystem* pool, Vector<AheadJob>& items) {
  if (items.empty()) return;
  if (pool == nullptr) {
    for (AheadJob& job : items)
      run_ahead(&job);
    return;
  }
  struct Shared {
    Vector<AheadJob>* items = nullptr;
    std::atomic<u32> next{0};
  } shared;
  shared.items = &items;
  const auto runner = [](void* data) {
    Shared& s = *static_cast<Shared*>(data);
    for (u32 i = s.next.fetch_add(1, std::memory_order_relaxed); i < s.items->size();
         i = s.next.fetch_add(1, std::memory_order_relaxed))
      run_ahead(&(*s.items)[i]);
  };
  const u32 runners = std::min<u32>(static_cast<u32>(items.size()), pool->total_worker_count() + 1);
  Vector<jobs::Job> list;
  list.reserve(runners);
  jobs::Counter counter;
  for (u32 r = 0; r < runners; ++r)
    list.push_back(jobs::Job{runner, &shared, &counter});
  pool->schedule(jobs::Pool::Performance, std::span<const jobs::Job>(list.data(), list.size()),
                 counter);
  pool->wait(counter);
}

}  // namespace

void probe_recipes_ahead(std::span<const RecipeAsk> asks, const MeshOptions& options,
                         HostReads& reads, jobs::JobSystem* pool) {
  // What this call has queued or kept, by `ahead_key`: nothing is probed twice.
  HashMap<u64, u32> seen;
  const auto wanted = [&](const std::string& path, bool handed) {
    if (assets::mesh_generator_of(path).empty()) return false;
    const u64 key = ahead_key(path, handed);
    if (ahead_at(&reads, path, handed) != nullptr) return false;
    return seen.try_emplace(key, 0u).second;
  };
  Vector<AheadJob> wave;
  for (const RecipeAsk& ask : asks) {
    if (ask.path == nullptr || !wanted(*ask.path, ask.handed != nullptr)) continue;
    AheadJob job;
    job.path = *ask.path;
    job.handed = ask.handed;
    job.pack = options.materials;
    wave.push_back(std::move(job));
  }
  // The hosts the probes name, wave by wave, as deep as the teaching round walks.
  for (u32 depth = 0; !wave.empty() && depth <= k_host_depth; ++depth) {
    run_ahead_jobs(pool, wave);
    Vector<AheadJob> next;
    for (AheadJob& job : wave) {
      if (reads.counts != nullptr) {
        for (u32 k = 0; k < k_class_read_kinds; ++k) {
          ClassReadCount& to = reads.counts->kinds[k];
          const ClassReadCount& from = job.counts.kinds[k];
          to.asked += from.asked;
          to.found += from.found;
          to.bytes += from.bytes;
          to.ns += from.ns;
        }
      }
      for (const std::string& host : job.out.hosts) {
        if (!wanted(host, false)) continue;
        AheadJob inner;
        inner.path = host;
        inner.pack = options.materials;
        next.push_back(std::move(inner));
      }
      // Kept under its key unless another path holds it (that one is then probed where asked).
      const u64 key = ahead_key(job.out.path, job.out.handed);
      if (reads.ahead_of.try_emplace(key, static_cast<u32>(reads.ahead.size())).second)
        reads.ahead.push_back(std::move(job.out));
    }
    wave = std::move(next);
  }
}

void keep_pack_classes(const MeshOptions& options, HostReads& reads) {
  reads.pack_kept = false;
  reads.pack = HostReads::PackClasses{};
  if (options.materials == nullptr) return;
  const content_class::Categories& categories = categories_of(options);
  reads.pack.pack = options.materials;
  reads.pack.categories = &categories;
  reads.pack.category_count = static_cast<u32>(categories.size());
  reads.pack.ok = read_pack_classes(*options.materials, categories, reads.pack.classes,
                                    reads.pack.why, reads.counts);
  reads.pack_kept = true;
}

namespace {

bool read_classes_at(const std::string& source, const MeshOptions& options, SourceClasses& out,
                     Diagnostic& error, u32 depth, const HostReads* reads) {
  const content_class::Categories& categories = categories_of(options);
  out = SourceClasses{};
  const auto refuse = [&](std::string message) {
    out.classes = content_class::unreadable(categories);
    out.materials.clear();
    out.hash = 0;
    out.record.clear();
    error.rule = k_rule_content_class;
    error.message = std::move(message);
    return false;
  };
  ClassReads* const counts = reads != nullptr ? reads->counts : nullptr;
  const bool generated = !assets::mesh_generator_of(source).empty();
  const std::string sidecar = content_class::sidecar_path_of(source);
  std::string text;
  bool present = false;
  std::string why;
  const i64 sidecar_ns = time::monotonic_ns();
  const bool looked = read_sidecar_text(sidecar, text, present, why);
  if (counts != nullptr) {
    const ClassRead kind = generated                              ? ClassRead::recipe_sidecar
                           : io::extension(source) == ".clusters" ? ClassRead::container_sidecar
                                                                  : ClassRead::gltf_sidecar;
    counts->add(kind, present, text.size(), time::monotonic_ns() - sidecar_ns);
  }
  if (!looked) return refuse(sidecar + " " + why);
  if (present &&
      !content_class::read_sidecar_classes(categories, text, out.classes, why, &out.materials))
    return refuse(sidecar + ": " + why + "; the build refuses " + source);
  // A generator's output is the join of the sets it baked, which its key covers through the pack.
  // Read once a load when the caller kept them (C72): the same sets against the same table.
  const HostReads::PackClasses* kept_pack = reads != nullptr && reads->pack_kept &&
                                                    reads->pack.pack == options.materials &&
                                                    reads->pack.categories == &categories &&
                                                    reads->pack.category_count == categories.size()
                                                ? &reads->pack
                                                : nullptr;
  if (generated && kept_pack != nullptr) {
    if (!kept_pack->ok) return refuse(kept_pack->why + "; the build refuses " + source);
    out.classes = content_class::join(out.classes, kept_pack->classes);
  } else if (generated && options.materials != nullptr &&
             !read_pack_classes(*options.materials, categories, out.classes, why, counts)) {
    return refuse(why + "; the build refuses " + source);
  }
  // **And of every mesh it reads** (C64): each host its generator names, by the host's own class
  // (its sidecar; a recipe's, recursively; a container's section 54 too), read before the host is,
  // joined as a whole asset, so its key and section 54 carry it.
  Vector<std::string> hosts;
  if (generated && !recipe_hosts(source, options.materials, reads, hosts, why))
    return refuse(why + "; the build refuses " + source);
  for (const std::string& host : hosts) {
    // At `GeneratorHosts::identify`'s bound: `depth` is this dependant's, as `depth_` is there,
    // so a chain that builds and loads is one this read takes, and one it refuses is refused.
    if (depth >= k_host_depth)
      return refuse(source + ": a host's host is more than " + std::to_string(k_host_depth) +
                    " levels deep: a loop of recipes?");
    SourceClasses inner;
    Diagnostic inner_error;
    if (!read_classes_at(host, options, inner, inner_error, depth + 1, reads))
      return refuse(inner_error.message + "; the host of " + source);
    out.classes = content_class::join(out.classes, inner.classes);
    if (io::extension(host) == ".clusters") {
      content_class::ContentClasses own;
      if (!host_container_classes(host, categories, reads, own, why))
        return refuse(why + "; the host of " + source);
      out.classes = content_class::join(out.classes, own);
    }
  }
  out.hash = content_class::class_hash(categories, out.classes, &out.materials);
  content_class::encode_classes(categories, out.classes, &out.materials, out.record);
  return true;
}

}  // namespace

u64 classified_build_key(const std::string& path, u64 source_hash, const MeshOptions& options,
                         const SourceClasses& classes) noexcept {
  return content_class::classified_key(mesh_build_key(path, source_hash, options), classes.hash);
}

bool build_one(const std::string& input, const std::string& output, const MeshOptions& options,
               bool strict, jobs::JobSystem* pool, u64 source_hash, u64 build_key, BuildResult& out,
               Diagnostic& error, const SourceClasses* classes) {
  if (!assets::mesh_generator_of(input).empty())
    return build_generated(input, output, options, source_hash, build_key, out, error, classes);
  const i64 start_ns = time::monotonic_ns();
  out.path = output;
  out.source_hash = source_hash;
  out.build_key = build_key;

  assets::MeshData mesh;
  std::string message;
  if (!assets::load_gltf(input, mesh, &message)) {
    error.rule = "source.unreadable";
    error.message = message;
    return false;
  }
  if (!options.morph) mesh.morph.clear();

  Diagnostics diagnostics;
  if (!validate_mesh(mesh, input, diagnostics)) {
    error = std::move(diagnostics.error);
    return false;
  }

  // Zero-area triangles are dropped first: after validation, so every diagnostic above names a
  // triangle as the source numbers it, and before the UV repair, which should not have to fold a
  // triangle that has no area to show (geometry.md, "Zero-area triangles: the repair").
  // `engine-view --mesh` calls the same function at the same point, because the two share cache
  // entries. What it does not cover is still refused, after it, with a sentence that says what it
  // removed: a triangle that repeats an index, and a primitive with no area left at all.
  if (!assets::repair_zero_area_triangles(mesh, out.zero_area_repair, &message)) {
    error.rule = k_rule_degenerate;
    error.message = message;
    return false;
  }
  const geometry::ZeroAreaRepairReport& zero_area = out.zero_area_repair;
  const std::string removed =
      " (the zero-area repair ran first: it found " + std::to_string(zero_area.triangles) +
      " triangle" + (zero_area.triangles == 1 ? "" : "s") +
      " whose three distinct corners span no area, " + std::to_string(zero_area.collinear) +
      " collinear and " + std::to_string(zero_area.coincident) +
      " with coincident corners, and removed " + std::to_string(zero_area.dropped);
  if (*diagnostics.repeated.rule != '\0') {
    error.rule = diagnostics.repeated.rule;
    error.message = diagnostics.repeated.message + removed +
                    "; a repeated index is an indexing fault it does not repair)";
    return false;
  }
  if (zero_area.unrepaired != 0) {
    for (u32 p = 0; p < mesh.primitives.size(); ++p) {
      const assets::Primitive& primitive = mesh.primitives[p];
      if (primitive.index_count != 3) continue;  // a run it kept a triangle of holds just that one
      const u32* corner = mesh.indices.data() + primitive.first_index;
      if (!geometry::spans_zero_area(mesh.positions[corner[0]], mesh.positions[corner[1]],
                                     mesh.positions[corner[2]])) {
        continue;
      }
      error.rule = k_rule_degenerate;
      error.message = "primitive " + std::to_string(p) + " of '" + input +
                      "' has no triangle with any area" + removed +
                      ", keeping this primitive's last triangle rather than empty it)";
      return false;
    }
  }
  out.warnings = std::move(diagnostics.warnings);
  if (strict && !out.warnings.empty()) {
    error.rule = out.warnings[0].rule;
    error.message = out.warnings[0].message + " (--strict)";
    return false;
  }

  // UV-degenerate triangles are repaired before the weld, so a corner that took its neighbouring
  // island's UV is merged into that island's vertex by the weld below. After validation, so a
  // diagnostic still names the triangle as the source numbers it. `engine-view --mesh` calls the
  // same function at the same point, because the two share cache entries.
  if (!assets::repair_uv_degenerate_triangles(mesh, out.uv_repair, &message)) {
    error.rule = k_rule_uv_degenerate;
    error.message = message;
    return false;
  }

  // `--atlas repack` (docs/subsystems/atlas.md): after the repair, so the rebake samples the
  // source through the UVs the repair left, and before the weld, which merges what the new atlas
  // made one point and drops the vertices no repacked triangle references any more. Its images
  // replace the source's in the container below; nothing of the source's UV layout survives.
  if (options.repack) {
    out.repacked = true;
    if (!atlas::repack_atlas(mesh, io::parent_path(input), options.atlas, pool, out.atlas,
                             &message)) {
      error.rule = k_rule_atlas_repack;
      error.message = "'" + input + "': " + message;
      return false;
    }
  }

  const u32 loaded_vertices = mesh.positions.size();
  u32 vertices = loaded_vertices;
  if (options.weld) {
    // `assets::weld_vertices` hands the weld every per-vertex stream the mesh owns — the skin
    // bindings and the morph channels as well as the positions, normals and UVs — so two
    // coincident vertices that disagree about any of them stay two vertices, and the channels are
    // renumbered with the vertices instead of being left in the old numbering
    // ([geometry](geometry.md), "Morph channels"). `engine-view --mesh` calls the same function,
    // because the two apps share cache entries and what one writes the other has to find.
    vertices = assets::weld_vertices(mesh);
  }
  ENGINE_LOG_INFO(log_content, "mesh loaded", log::field("path", input),
                  log::field("vertices", loaded_vertices), log::field("welded", vertices),
                  log::field("triangles", mesh.indices.size() / 3),
                  log::field("primitives", mesh.primitives.size()),
                  log::field("materials", mesh.materials.size()),
                  log::field("skins", mesh.skins.size()), log::field("images", mesh.images.size()));

  // One DAG per primitive, so every cluster belongs to exactly one material; merging keeps the
  // parts' clusters and remembers which part each came from.
  const geometry::ClusterLodOptions lod_options = import_lod_options_of(options);
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  // What each alpha-tested primitive's mask keeps: its island levels' coverage errors count a
  // removed card for that share (geometry.md, "Coverage errors for an imported alpha-tested
  // part"). Only with the rule on, so `--no-loose-islands` is the builder before it.
  Vector<f32> mask_covers;
  if (lod_options.loose_islands) measure_mask_covers(mesh, io::parent_path(input), mask_covers);
  Vector<PartBuild> parts;
  parts.reserve(mesh.primitives.size());
  for (const assets::Primitive& primitive : mesh.primitives) {
    PartBuild part;
    part.mesh = &mesh;
    part.options = lod_options;
    const u32 index = static_cast<u32>(&primitive - mesh.primitives.data());
    if (index < mask_covers.size()) part.options.mask_cover = mask_covers[index];
    if (part.options.mask_cover > 0.0f) {
      ENGINE_LOG_INFO(log_content, "mask cover measured", log::field("primitive", index),
                      log::field("material", primitive.material),
                      log::field("cover", static_cast<f64>(part.options.mask_cover)));
    }
    part.attributes = &attributes;
    part.first_index = primitive.first_index;
    part.index_count = primitive.index_count;
    part.material = primitive.material;
    parts.push_back(std::move(part));
  }
  if (parts.empty()) {
    error.rule = k_rule_empty_primitive;
    error.message = "'" + input + "' has no triangles to cluster";
    return false;
  }

  if (pool != nullptr && parts.size() > 1) {
    Vector<jobs::Job> job_list;
    job_list.reserve(parts.size());
    jobs::Counter counter;
    for (PartBuild& part : parts)
      job_list.push_back(jobs::Job{build_part, &part, &counter});
    pool->schedule(jobs::Pool::Performance,
                   std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
    pool->wait(counter);
  } else {
    for (PartBuild& part : parts)
      build_part(&part);
  }
  // Failures are reported in primitive order too: the same input fails with the same sentence
  // whatever --jobs said.
  for (const PartBuild& part : parts) {
    if (!part.ok) {
      error.rule = "geometry.cluster_build";
      error.message = part.error;
      return false;
    }
  }

  Vector<geometry::ClusterLodMesh> lods;
  Vector<i32> part_material;
  lods.reserve(parts.size());
  part_material.reserve(parts.size());
  for (PartBuild& part : parts) {
    // What the loose-island rule did, summed over the primitives (the merge does not carry it).
    const geometry::AggregateStats& a = part.out.aggregate;
    out.loose_islands.groups += a.groups;
    out.loose_islands.regular += a.regular;
    out.loose_islands.by_islands += a.by_islands;
    out.loose_islands.sloppy += a.sloppy;
    out.loose_islands.islands_removed += a.islands_removed;
    out.loose_islands.islands_grown += a.islands_grown;
    out.loose_islands.islands_restored += a.islands_restored;
    out.loose_islands.vertices_added += a.vertices_added;
    out.loose_islands.errors_raised += a.errors_raised;
    lods.push_back(std::move(part.out));
    part_material.push_back(part.material);
  }

  geometry::ClusterFileData data;
  Vector<u32> part_of_cluster;
  if (!geometry::merge_cluster_lod(lods, data.mesh, &part_of_cluster, &message)) {
    error.rule = "geometry.cluster_merge";
    error.message = message;
    return false;
  }
  if (data.mesh.mesh.clusters.size() > k_max_clusters) {
    error.rule = k_rule_cluster_budget;
    error.message = "'" + input + "' builds " + std::to_string(data.mesh.mesh.clusters.size()) +
                    " clusters, more than the " + std::to_string(k_max_clusters) +
                    " the visibility buffer can name";
    return false;
  }

  // Into streaming pages, coarse to fine and group by group (docs/plan/04-renderer.md §4.3 step 3
  // and §4.9). This renumbers the clusters, so it happens before anything downstream indexes
  // them and the map from cluster to primitive is carried through the same permutation.
  if (options.page_bytes != 0) {
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = options.page_bytes;
    Vector<u32> source_of_cluster;
    if (!geometry::build_cluster_pages(data.mesh, page_options, data.pages, &message,
                                       &source_of_cluster)) {
      error.rule = "geometry.cluster_pages";
      error.message = message;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
    if (!data.mesh.coverage.empty())
      geometry::permute_cluster_array(source_of_cluster, data.mesh.coverage);
  }
  // The alpha-tested primitives' coverage errors, in the clusters' final order: section 39.
  data.lod_coverage = std::move(data.mesh.coverage);
  data.mesh.coverage = Vector<u32>();

  data.materials.reserve(mesh.materials.size() + 1);
  for (const assets::Material& source : mesh.materials) {
    // The one conversion the renderer's glTF path calls too (G2), so the two write one record.
    data.materials.push_back(assets::cluster_file_material(source));
    // How its slots sample their images (section 33), which the texture records below read too.
    data.material_sampling.push_back(
        geometry::encode_material_sampling(source.sampling, source.occlusion_strength));
  }
  // A primitive that names no material gets one appended default, shared by all of them.
  constexpr u32 k_no_default = ~u32{0};
  u32 default_material = k_no_default;
  data.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster) {
    const i32 index = part_material[part];
    if (index >= 0 && static_cast<u32>(index) < mesh.materials.size()) {
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
  // What the import read beyond the materials (G2): the second-UV slots, the lights, the cameras;
  // nothing for a file that states none. The renderer's glTF path calls the same function.
  assets::write_import_sections(mesh, data);
  // Image paths as the glTF gives them, relative to the glTF file; an image the file embedded has
  // no path of its own, so its **encoded bytes** travel in the container instead and the slot
  // keeps an empty path. That is what makes the container drawable wherever it ends up: a GLB
  // holds its textures inside itself, and a container that recorded only their empty paths drew
  // the mesh untextured while the GLB beside it drew it textured ([geometry](geometry.md)).
  data.image_paths.reserve(mesh.images.size());
  data.images.reserve(mesh.images.size());
  for (const assets::ImageRef& image : mesh.images) {
    data.image_paths.push_back(image.uri);
    geometry::ClusterImage carried;
    carried.mime_type = image.mime_type;
    carried.bytes = image.bytes;
    data.images.push_back(std::move(carried));
  }
  const geometry::ClusterImageSummary images = geometry::summarize_cluster_images(data);
  // Where each image's built texture is: the options its material slots ask for and, for the
  // images this container carries, the key (docs/subsystems/texture.md). engine-view's own cache
  // writer calls the same function, because the two share cache entries byte for byte.
  texture::fill_cluster_texture_records(data);
  collect_texture_sources(data, io::parent_path(input), out.texture_sources);
  // The perches its nodes declare (section 43; A43), as engine-view's own cache writer adds them.
  data.perches = mesh.perches;

  // The source as it was given, so the renderer resolves the image paths above against its
  // directory however it came by the container, and the source's identity, so that a later build
  // recognizes this container as the answer to the same question.
  data.source_path = input;
  data.source_hash = source_hash;
  data.build_key = build_key;
  // The source's own class (C62): section 54, written only when there is one, so an unclassified
  // source's container is the bytes it was.
  if (classes != nullptr) {
    data.content_classes = classes->record;
    if (classes->hash != 0)
      out.content_classes = content_class::classes_json(categories_of(options), classes->classes,
                                                        &classes->materials);
  }

  if (!geometry::write_cluster_file(output, data, &message)) {
    error.rule = "output.unwritable";
    error.message = message;
    return false;
  }
  io::FileInfo info;
  const geometry::ClusterLodMesh& lod = data.mesh;
  out.clusters = lod.mesh.clusters.size();
  out.leaf_clusters = lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0];
  out.triangles = lod.leaf_triangle_count;
  out.lod_levels = lod.level_cluster_counts.size();
  out.vertices = lod.mesh.vertices.size();
  out.pages = data.pages.pages.size();
  out.page_bytes = options.page_bytes;
  if (out.pages != 0) {
    u64 page_total = 0;
    for (const geometry::ClusterPageDesc& page : data.pages.pages)
      page_total += page.bytes;
    out.page_fill = static_cast<f64>(page_total) /
                    (static_cast<f64>(out.pages) * static_cast<f64>(options.page_bytes));
  }
  out.materials = data.materials.size();
  out.images = data.image_paths.size();
  out.embedded_images = images.embedded;
  out.deduplicated_images = images.deduplicated;
  out.image_bytes = images.bytes;
  out.vertex_id_source = geometry::vertex_id_source_name(lod.mesh.vertex_id_source);
  out.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
  out.hash = geometry::cluster_file_hash(data);
  out.build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;

  ENGINE_LOG_INFO(log_content, "cluster file written", log::field("path", output),
                  log::field("clusters", out.clusters), log::field("pages", out.pages),
                  log::field("bytes", out.bytes), log::field("build_ms", out.build_ms));
  return true;
}

// The repairs a build made, as the `repairs` array of its JSON line: one row per rule that fired,
// empty when none did, so a reader tells "nothing to repair" from "an older build" by the key.
JsonValue repairs_json(const BuildResult& result) {
  JsonValue rows = JsonValue::array();
  // In the order the build runs them: the zero-area repair, then the UV repair.
  const geometry::ZeroAreaRepairReport& z = result.zero_area_repair;
  if (z.triangles != 0) {
    JsonValue row = JsonValue::object();
    row.set("rule", JsonValue(k_rule_zero_area));
    row.set("triangles", JsonValue(z.triangles));
    row.set("collinear", JsonValue(z.collinear));
    row.set("coincident", JsonValue(z.coincident));
    row.set("dropped", JsonValue(z.dropped));
    row.set("unrepaired", JsonValue(z.unrepaired));
    rows.push_back(std::move(row));
  }
  const geometry::UvRepairReport& r = result.uv_repair;
  if (r.triangles != 0) {
    JsonValue row = JsonValue::object();
    row.set("rule", JsonValue(k_rule_uv_degenerate));
    row.set("islands", JsonValue(r.islands));
    row.set("triangles", JsonValue(r.triangles));
    row.set("refolded", JsonValue(r.refolded));
    row.set("dropped", JsonValue(r.dropped));
    row.set("unrepaired", JsonValue(r.unrepaired));
    row.set("corners_from_neighbours", JsonValue(r.corners_from_neighbours));
    row.set("corners_on_edge", JsonValue(r.corners_on_edge));
    row.set("threshold_texels_4096", JsonValue(r.threshold_texels));
    rows.push_back(std::move(row));
  }
  return rows;
}

namespace {

const char* space_label(atlas::TextureSpace space) {
  switch (space) {
    case atlas::TextureSpace::srgb: return "srgb";
    case atlas::TextureSpace::linear: return "linear";
    case atlas::TextureSpace::tangent_normal: return "normal";
  }
  return "?";
}

}  // namespace

// What the atlas step did, as the `atlas` object of a build's JSON line: `{"mode":"keep"}` when it
// was not asked for, and otherwise the charts, the atlas per material, every rebaked image with
// how far it is from its source on the surface, and the time each stage took.
JsonValue atlas_json(const BuildResult& result, const MeshOptions& options) {
  JsonValue out = JsonValue::object();
  out.set("mode", JsonValue(result.repacked ? "repack" : "keep"));
  if (!result.repacked) return out;
  const atlas::RepackReport& r = result.atlas;
  out.set("normal_maps",
          JsonValue(options.atlas.normal_maps == atlas::NormalMapMode::convert ? "convert"
                                                                               : "resample"));
  out.set("proxy_triangles", JsonValue(options.atlas.proxy_triangles));
  out.set("supersample", JsonValue(options.atlas.supersample));
  out.set("chart_cost_milli", JsonValue(options.atlas.max_chart_cost_milli));
  out.set("padding", JsonValue(options.atlas.padding));
  out.set("materials_repacked", JsonValue(r.materials_repacked));
  out.set("materials_untextured", JsonValue(r.materials_untextured));
  out.set("materials_unreadable", JsonValue(r.materials_unreadable));
  out.set("materials_declined", JsonValue(r.materials_declined));
  out.set("triangles", JsonValue(r.triangles));
  out.set("charts", JsonValue(r.charts));
  out.set("vertices_before", JsonValue(r.vertices_before));
  out.set("vertices_after", JsonValue(r.vertices_after));
  out.set("images_before", JsonValue(r.images_before));
  out.set("images_after", JsonValue(r.images_after));
  out.set("image_bytes", JsonValue(r.image_bytes));
  JsonValue ms = JsonValue::object();
  ms.set("decode", JsonValue(r.decode_ms));
  ms.set("chart", JsonValue(r.chart_ms));
  ms.set("rebake", JsonValue(r.rebake_ms));
  ms.set("encode", JsonValue(r.encode_ms));
  ms.set("total", JsonValue(r.total_ms));
  out.set("ms", std::move(ms));
  JsonValue materials = JsonValue::array();
  for (const atlas::RepackedMaterial& m : r.materials) {
    JsonValue row = JsonValue::object();
    row.set("material", JsonValue(m.material));
    row.set("triangles", JsonValue(m.triangles));
    row.set("charts", JsonValue(m.charts));
    row.set("resolution", JsonValue(m.resolution));
    row.set("utilization", JsonValue(static_cast<f64>(m.utilization)));
    row.set("pack_attempts", JsonValue(m.pack_attempts));
    row.set("proxy_triangles", JsonValue(m.proxy_triangles));
    row.set("proxy_charts", JsonValue(m.proxy_charts));
    row.set("folded_triangles", JsonValue(m.folded_triangles));
    row.set("chart_ms", JsonValue(m.chart_ms));
    row.set("pack_ms", JsonValue(m.pack_ms));
    row.set("unatlased_triangles", JsonValue(m.unatlased_triangles));
    row.set("crumb_charts", JsonValue(m.crumb_charts));
    row.set("crumb_triangles", JsonValue(m.crumb_triangles));
    row.set("source_vertices", JsonValue(m.source_vertices));
    row.set("vertices", JsonValue(m.vertices));
    materials.push_back(std::move(row));
  }
  out.set("detail", std::move(materials));
  JsonValue images = JsonValue::array();
  for (const atlas::RebakedImage& im : r.images) {
    JsonValue row = JsonValue::object();
    row.set("material", JsonValue(im.material));
    row.set("source_image", JsonValue(im.source_image));
    row.set("image", JsonValue(im.image));
    row.set("space", JsonValue(space_label(im.space)));
    row.set("size", JsonValue(im.width));
    row.set("source_width", JsonValue(im.source_width));
    row.set("source_height", JsonValue(im.source_height));
    row.set("png_bytes", JsonValue(im.png_bytes));
    JsonValue e = JsonValue::object();
    e.set("unit", JsonValue(im.space == atlas::TextureSpace::tangent_normal ? "degrees" : "8-bit"));
    e.set("samples", JsonValue(im.error.samples));
    e.set("mean", JsonValue(im.error.mean));
    e.set("p99", JsonValue(im.error.p99));
    e.set("max", JsonValue(im.error.max));
    row.set("rebake_error", std::move(e));
    images.push_back(std::move(row));
  }
  out.set("images", std::move(images));
  JsonValue notes = JsonValue::array();
  for (const std::string& note : r.notes)
    notes.push_back(JsonValue(note));
  out.set("notes", std::move(notes));
  return out;
}

// The job system the per-primitive and per-mesh builds run on. `worker_count` is `--jobs`; 0
// leaves the system its own default, one worker per performance CPU. The efficiency pool is
// asked for one worker because nothing here uses it.
jobs::JobSystemConfig job_config(u32 worker_count) {
  jobs::JobSystemConfig config;
  config.performance_workers = worker_count;
  config.efficiency_workers = 1;
  return config;
}

// Reads the manifest and resolves every path against the manifest's own directory, so a manifest
// is movable as a unit and says the same thing from any working directory.
bool read_manifest(const std::string& path, const MeshOptions& defaults, Vector<ManifestEntry>& out,
                   std::string& error, std::span<const char* const> other_arrays,
                   content_class::Categories* categories) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read manifest '" + path + "': " + io::status_name(status);
    return false;
  }
  // Strictly (C69): the manifest names the categories file every entry's sidecar is read against,
  // and with a key given twice the parser's ordinary rule (the last one wins) would let a text
  // merge or an append choose that file, or an entry's source, without a word.
  JsonParseOptions strict;
  strict.refuse_duplicate_keys = true;
  JsonValue document;
  const JsonParseResult parsed = parse_json(text, document, strict);
  if (!parsed.ok && !parsed.duplicate_key.empty()) {
    error = "manifest '" + path + "' line " + std::to_string(parsed.line) + ": the key '" +
            parsed.duplicate_key + "' is given twice in one object";
    return false;
  }
  if (!parsed.ok) {
    error = "manifest '" + path + "' line " + std::to_string(parsed.line) + " column " +
            std::to_string(parsed.column) + ": " + parsed.message;
    return false;
  }
  const JsonValue* meshes = document.is_object() ? document.find("meshes") : nullptr;
  if (meshes == nullptr && document.is_object()) {
    // A manifest of a derived step's entries alone (a list of clips) has no meshes to build.
    for (const char* other : other_arrays) {
      const JsonValue* array = document.find(other);
      if (array != nullptr && array->is_array()) return true;
    }
  }
  if (meshes == nullptr || !meshes->is_array()) {
    error = "manifest '" + path + "' has no \"meshes\" array";
    return false;
  }
  const std::string dir(io::parent_path(path));
  auto resolve = [&dir](std::string_view relative) {
    return io::is_absolute_path(relative) ? io::normalize_path(relative)
                                          : io::join_path(dir, relative);
  };
  // The game's categories (C62), which every entry's sidecar is read against.
  if (const JsonValue* named = document.find("categories"); named != nullptr) {
    if (!named->is_string()) {
      error = "manifest '" + path + "' has a \"categories\" that is not a path";
      return false;
    }
    const std::string file = resolve(named->as_string());
    if (categories == nullptr) {
      error = "manifest '" + path + "' names a categories file this host does not read";
      return false;
    }
    std::string body;
    std::string why;
    const io::Status read = io::read_file(file, body);
    if (read != io::Status::Ok || !categories->add_json(body, why)) {
      error = "manifest '" + path + "': categories file '" + file +
              "': " + (read != io::Status::Ok ? std::string(io::status_name(read)) : why);
      return false;
    }
  }
  for (usize i = 0; i < meshes->size(); ++i) {
    const JsonValue& entry = (*meshes)[i];
    const std::string at = "manifest '" + path + "' mesh " + std::to_string(i);
    if (!entry.is_object()) {
      error = at + " is not an object";
      return false;
    }
    const JsonValue* source = entry.find("source");
    if (source == nullptr || !source->is_string()) {
      error = at + " has no \"source\" string";
      return false;
    }
    ManifestEntry built;
    built.options = defaults;  // the command line sets what an entry does not say
    if (document.find("categories") != nullptr) built.options.categories = categories;
    built.source = resolve(source->as_string());
    if (const JsonValue* output = entry.find("output"); output != nullptr) {
      if (!output->is_string()) {
        error = at + " has an \"output\" that is not a string";
        return false;
      }
      built.output = resolve(output->as_string());
    }
    if (const JsonValue* options = entry.find("options"); options != nullptr) {
      if (!options->is_object()) {
        error = at + " has an \"options\" that is not an object";
        return false;
      }
      u64 value = 0;
      if (const JsonValue* v = options->find("max_triangles"); v != nullptr) {
        if (!v->get_u64(value)) {
          error = at + ": \"max_triangles\" is not a number";
          return false;
        }
        built.options.max_triangles = static_cast<u32>(value);
      }
      if (const JsonValue* v = options->find("max_vertices"); v != nullptr) {
        if (!v->get_u64(value)) {
          error = at + ": \"max_vertices\" is not a number";
          return false;
        }
        built.options.max_vertices = static_cast<u32>(value);
      }
      if (const JsonValue* v = options->find("page_bytes"); v != nullptr) {
        if (!v->get_u64(value)) {
          error = at + ": \"page_bytes\" is not a number";
          return false;
        }
        built.options.page_bytes = static_cast<u32>(value);
      }
      if (const JsonValue* v = options->find("weld"); v != nullptr) {
        bool weld = true;
        if (!v->get_bool(weld)) {
          error = at + ": \"weld\" is not a boolean";
          return false;
        }
        built.options.weld = weld;
      }
      if (const JsonValue* v = options->find("atlas"); v != nullptr) {
        if (!v->is_string() || !read_atlas_mode(v->as_string(), built.options.repack)) {
          error = at + ": \"atlas\" is \"keep\" or \"repack\"";
          return false;
        }
      }
      if (!options_in_range(built.options)) {
        error = at + ": max_triangles is 4..256 and max_vertices is 1..255";
        return false;
      }
    }
    out.push_back(std::move(built));
  }
  return true;
}

void run_mesh_task(void* data) {
  MeshTask& task = *static_cast<MeshTask*>(data);
  const ManifestEntry& entry = *task.entry;
  u64 source_hash = 0;
  std::string message;
  if (!source_hash_of(entry.source, source_hash, &message)) {
    task.error.rule = "source.unreadable";
    task.error.message = message;
    task.state = TaskState::Failed;
    return;
  }
  // The sidecar is read before the key, because the class is in it (C62): a source whose sidecar
  // gains a class has a new key, so the container built before is never its answer.
  SourceClasses classes;
  if (!read_source_classes(entry.source, entry.options, classes, task.error)) {
    task.state = TaskState::Failed;
    return;
  }
  const u64 key = classified_build_key(entry.source, source_hash, entry.options, classes);
  const bool to_cache = entry.output.empty();
  const std::string output = to_cache ? geometry::cluster_cache_path(*task.ddc, key) : entry.output;
  task.result.path = output;
  task.result.cached = to_cache;
  task.result.source_hash = source_hash;
  task.result.build_key = key;
  if (classes.hash != 0)  // reported for a skipped entry too: the container on disk holds it
    task.result.content_classes = content_class::classes_json(categories_of(entry.options),
                                                              classes.classes, &classes.materials);

  // Up to date when the container that is already there was built from these bytes with these
  // options. A container that records no identity — an older build, or one engine-view wrote
  // before T78 — has a zero key and is rebuilt rather than trusted.
  u64 had_hash = 0;
  u64 had_key = 0;
  if (!task.force && io::exists(output) &&
      geometry::read_cluster_file_identity(output, had_hash, had_key) && had_key != 0 &&
      had_key == key && had_hash == source_hash) {
    io::FileInfo info;
    task.result.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
    task.state = TaskState::Skipped;
    return;
  }
  // In a manifest the per-primitive jobs are not nested inside this one (`pool` is null): the
  // manifest is the coarser level and already fills the pool, and one wait per mesh keeps the
  // helper stack shallow. A single mesh run on its own hands its pool down instead.
  task.state = build_one(entry.source, output, entry.options, task.strict, task.pool, source_hash,
                         key, task.result, task.error, &classes)
                   ? TaskState::Built
                   : TaskState::Failed;
}

// ---- the texture step (docs/subsystems/texture.md) ---------------------------------------------

const char* texture_state_name(TextureState state) noexcept {
  switch (state) {
    case TextureState::Built: return "built";
    case TextureState::Skipped: return "skipped";
    case TextureState::Shared: return "shared";
    case TextureState::Missing: return "missing";
    case TextureState::Failed: return "failed";
    case TextureState::Unreadable: return "unreadable";
  }
  return "?";
}

namespace {

f64 ms_since(i64 start_ns) noexcept {
  return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
}

// A number JSON can hold: PSNR is infinite for an exact texture, and JSON has no infinity.
JsonValue finite_or_null(f64 value) {
  return std::isfinite(value) ? JsonValue(value) : JsonValue();
}

// One texture of the step on its way through: the source's bytes, owned when they came from a
// file, and what building them needs. A job writes its own task and nothing else.
struct TextureTask {
  TextureResult result;
  std::string file_bytes;
  std::span<const u8> bytes;
  texture::TextureBuildOptions options;
  jobs::JobSystem* pool = nullptr;
  i64 start_ns = 0;
  Vector<u8> class_record;  // section 6: the image's own class, empty when it has none (C62)
};

void build_texture_task(void* data) {
  TextureTask& task = *static_cast<TextureTask*>(data);
  TextureResult& out = task.result;
  const i64 start_ns = time::monotonic_ns();
  texture::TextureData built;
  std::string error;
  if (!texture::build_texture_from_encoded(task.bytes, task.options, built, task.pool, &error,
                                           &out.report)) {
    out.state = TextureState::Unreadable;
    out.error = error;
    return;
  }
  // The key the step took holds the image's class when it has one (C62), and the identity an
  // incremental build compares is that key; the class itself is section 6.
  built.build_key = out.key;
  built.content_classes = task.class_record;
  const io::Status made = io::make_directories(io::parent_path(out.path));
  if (made != io::Status::Ok || !texture::write_texture_file(out.path, built, &error)) {
    out.state = TextureState::Failed;
    out.error =
        made != io::Status::Ok
            ? std::string("cannot create the texture cache directory: ") + io::status_name(made)
            : error;
    return;
  }
  io::FileInfo info;
  out.bytes = io::stat_file(out.path, info) == io::Status::Ok ? info.size : 0;
  out.format = built.format;
  out.color_space = built.color_space;
  out.width = built.width;
  out.height = built.height;
  out.levels = built.levels.size();
  out.build_ms = ms_since(start_ns);
  out.state = TextureState::Built;
}

}  // namespace

void run_texture_step(std::span<const TextureSource* const> sources, std::span<const u32> mesh_of,
                      const std::string& ddc, jobs::JobSystem& pool, Vector<TextureResult>& out,
                      const content_class::Categories* categories) {
  static const content_class::Categories engine_only;
  const content_class::Categories& table = categories != nullptr ? *categories : engine_only;
  out.clear();
  Vector<TextureTask> tasks(static_cast<u32>(sources.size()));
  Vector<u64> seen;
  Vector<jobs::Job> job_list;
  jobs::Counter counter;
  for (u32 i = 0; i < sources.size(); ++i) {
    const TextureSource& source = *sources[i];
    TextureTask& task = tasks[i];
    TextureResult& r = task.result;
    task.start_ns = time::monotonic_ns();
    r.mesh = mesh_of[i];
    r.image = source.image;
    task.bytes = std::span<const u8>(source.bytes.data(), source.bytes.size());
    if (source.bytes.empty() && source.file.empty()) task.bytes = source.view;
    if (!source.file.empty()) {
      const io::Status status = io::read_file(source.file, task.file_bytes);
      if (status != io::Status::Ok) {
        r.state = TextureState::Missing;
        r.path = source.file;
        r.error = "cannot read '" + source.file + "': " + io::status_name(status);
        continue;
      }
      task.bytes = std::span<const u8>(reinterpret_cast<const u8*>(task.file_bytes.data()),
                                       task.file_bytes.size());
    }
    if (!texture::unpack_texture_options(source.options, task.options)) {
      r.state = TextureState::Failed;
      r.error = "the container's texture options word " + std::to_string(source.options) +
                " is not one this build reads";
      continue;
    }
    r.source_bytes = task.bytes.size();
    // A caller that hashed exactly these bytes says so; every other source is hashed here.
    r.source_hash = source.source_hash != 0 && source.file.empty()
                        ? source.source_hash
                        : hash_bytes(task.bytes.data(), task.bytes.size());
    r.key = texture::texture_cache_key(r.source_hash, task.options);
    // An image named by path is read with its sidecar (C62; texture.md, "Content classes"); an
    // embedded one has none, its class being the container's. A sidecar that cannot be read fails
    // the texture, and so the command: a class that cannot be read is never `none`, and a sidecar
    // that is there and leads nowhere is one that cannot be read (`sidecar_present`).
    if (!source.file.empty()) {
      const std::string sidecar = content_class::sidecar_path_of(source.file);
      bool present = false;
      std::string looked;
      if (!sidecar_present(sidecar, present, looked)) {
        r.state = TextureState::Failed;
        r.path = source.file;
        r.error = std::string(k_rule_content_class) + ": " + sidecar + " " + looked;
        continue;
      }
      if (present) {
        std::string text;
        std::string why;
        content_class::ContentClasses classes;
        Vector<content_class::MaterialClass> materials;
        const io::Status read = io::read_file(sidecar, text);
        if (read != io::Status::Ok ||
            !content_class::read_sidecar_classes(table, text, classes, why, &materials)) {
          r.state = TextureState::Failed;
          r.path = source.file;
          r.error = std::string(k_rule_content_class) + ": " + sidecar + ": " +
                    (read != io::Status::Ok ? std::string(io::status_name(read)) : why);
          continue;
        }
        r.key = content_class::classified_key(
            r.key, content_class::class_hash(table, classes, &materials));
        content_class::encode_classes(table, classes, &materials, task.class_record);
        if (!task.class_record.empty())
          r.content_classes = content_class::classes_json(table, classes, &materials);
      }
    }
    r.path = texture::texture_cache_path(ddc, r.key);
    bool shared = false;
    for (const u64 key : seen)
      shared = shared || key == r.key;
    if (shared) {
      r.state = TextureState::Shared;
      continue;
    }
    seen.push_back(r.key);
    // Up to date when the entry that is there was built from these bytes with these options; the
    // identity is checked rather than trusted from the file name, the way a container's is.
    u64 had_hash = 0;
    u64 had_key = 0;
    if (io::exists(r.path) && texture::read_texture_file_identity(r.path, had_hash, had_key) &&
        had_hash == r.source_hash && had_key == r.key) {
      io::FileInfo info;
      r.bytes = io::stat_file(r.path, info) == io::Status::Ok ? info.size : 0;
      r.state = TextureState::Skipped;
      continue;
    }
    task.pool = &pool;
    r.state = TextureState::Failed;  // until the job says otherwise
    job_list.push_back(jobs::Job{build_texture_task, &task, &counter});
  }
  if (!job_list.empty()) {
    pool.schedule(jobs::Pool::Performance,
                  std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
    pool.wait(counter);
  }
  out.reserve(tasks.size());
  for (TextureTask& task : tasks)
    out.push_back(std::move(task.result));
}

JsonValue textures_json(const Vector<TextureResult>& results, u32 mesh, bool enabled, bool detail) {
  JsonValue out = JsonValue::object();
  out.set("enabled", JsonValue(enabled));
  u32 counts[6] = {0, 0, 0, 0, 0, 0};
  u64 bytes = 0;
  u64 source_bytes = 0;
  f64 build_ms = 0.0;
  JsonValue rows = JsonValue::array();
  for (const TextureResult& r : results) {
    if (mesh != ~u32{0} && r.mesh != mesh) continue;
    ++counts[static_cast<u32>(r.state)];
    if (r.state == TextureState::Built || r.state == TextureState::Skipped) {
      bytes += r.bytes;
      source_bytes += r.source_bytes;
    }
    build_ms += r.build_ms;
    if (!detail) continue;
    JsonValue row = JsonValue::object();
    row.set("image", JsonValue(r.image));
    row.set("status", JsonValue(texture_state_name(r.state)));
    row.set("path", JsonValue(r.path));
    row.set("key", JsonValue(r.key));
    row.set("source_bytes", JsonValue(r.source_bytes));
    if (r.state == TextureState::Built) {
      row.set("format", JsonValue(texture::texture_format_name(r.format)));
      row.set("color_space", JsonValue(texture::color_space_name(r.color_space)));
      row.set("width", JsonValue(r.width));
      row.set("height", JsonValue(r.height));
      row.set("levels", JsonValue(r.levels));
      row.set("bytes", JsonValue(r.bytes));
      row.set("psnr", finite_or_null(r.report.psnr));
      row.set("max_error", JsonValue(r.report.max_error));
      JsonValue ms = JsonValue::object();
      ms.set("decode", JsonValue(r.report.decode_ms));
      ms.set("mips", JsonValue(r.report.mip_ms));
      ms.set("encode", JsonValue(r.report.encode_ms));
      ms.set("total", JsonValue(r.build_ms));
      row.set("ms", std::move(ms));
    } else if (r.state == TextureState::Skipped) {
      row.set("bytes", JsonValue(r.bytes));
    }
    if (!r.error.empty()) row.set("error", JsonValue(r.error));
    rows.push_back(std::move(row));
  }
  out.set("built", JsonValue(counts[static_cast<u32>(TextureState::Built)]));
  out.set("skipped", JsonValue(counts[static_cast<u32>(TextureState::Skipped)]));
  out.set("shared", JsonValue(counts[static_cast<u32>(TextureState::Shared)]));
  out.set("missing", JsonValue(counts[static_cast<u32>(TextureState::Missing)]));
  out.set("failed", JsonValue(counts[static_cast<u32>(TextureState::Failed)]));
  out.set("unreadable", JsonValue(counts[static_cast<u32>(TextureState::Unreadable)]));
  out.set("bytes", JsonValue(bytes));
  out.set("source_bytes", JsonValue(source_bytes));
  out.set("build_ms", JsonValue(build_ms));
  if (detail) out.set("detail", std::move(rows));
  return out;
}

void texture_sources_of_container(const std::string& path, const std::string& source,
                                  Vector<TextureSource>& out) {
  out.clear();
  geometry::ClusterFileReader reader;
  geometry::ClusterFileData resident;
  std::string error;
  if (!reader.open(path, &resident, &error)) {
    ENGINE_LOG_WARN(log_content, "textures not checked", log::field("path", path),
                    log::field("error", error));
    return;
  }
  collect_texture_sources(resident, io::parent_path(source), out);
}

// ---- many meshes -------------------------------------------------------------------------------

void run_mesh_tasks(std::span<MeshTask> tasks, jobs::JobSystem& pool) {
  Vector<jobs::Job> job_list;
  job_list.reserve(static_cast<u32>(tasks.size()));
  jobs::Counter counter;
  for (MeshTask& task : tasks)
    job_list.push_back(jobs::Job{run_mesh_task, &task, &counter});
  if (job_list.empty()) return;
  pool.schedule(jobs::Pool::Performance,
                std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
  pool.wait(counter);
}

// ---- derived steps ------------------------------------------------------------------------------

bool read_derived_entries(const std::string& manifest, const DerivedStep& step,
                          Vector<DerivedEntry>& out, std::string& error) {
  out.clear();
  std::string text;
  const io::Status status = io::read_file(manifest, text);
  if (status != io::Status::Ok) {
    error = "cannot read manifest '" + manifest + "': " + io::status_name(status);
    return false;
  }
  JsonValue document;
  const JsonParseResult parsed = parse_json(text, document);
  if (!parsed.ok) {
    error = "manifest '" + manifest + "' line " + std::to_string(parsed.line) + " column " +
            std::to_string(parsed.column) + ": " + parsed.message;
    return false;
  }
  const JsonValue* array = document.is_object() ? document.find(step.manifest_array) : nullptr;
  if (array == nullptr) return true;
  if (!array->is_array()) {
    error =
        "manifest '" + manifest + "' has a \"" + step.manifest_array + "\" that is not an array";
    return false;
  }
  const std::string dir(io::parent_path(manifest));
  auto resolve = [&dir](std::string_view relative) {
    return io::is_absolute_path(relative) ? io::normalize_path(relative)
                                          : io::join_path(dir, relative);
  };
  for (usize i = 0; i < array->size(); ++i) {
    const JsonValue& entry = (*array)[i];
    const std::string at =
        "manifest '" + manifest + "' " + step.manifest_array + " entry " + std::to_string(i);
    if (!entry.is_object()) {
      error = at + " is not an object";
      return false;
    }
    const JsonValue* source = entry.find("source");
    if (source == nullptr || !source->is_string()) {
      error = at + " has no \"source\" string";
      return false;
    }
    DerivedEntry built;
    built.source = resolve(source->as_string());
    if (const JsonValue* output = entry.find("output"); output != nullptr) {
      if (!output->is_string()) {
        error = at + " has an \"output\" that is not a string";
        return false;
      }
      built.output = resolve(output->as_string());
    }
    out.push_back(std::move(built));
  }
  return true;
}

void run_derived_task(void* data) {
  DerivedTask& task = *static_cast<DerivedTask*>(data);
  const DerivedStep& step = *task.step;
  const DerivedEntry& entry = *task.entry;
  std::string bytes;
  const io::Status status = io::read_file(entry.source, bytes);
  if (status != io::Status::Ok) {
    task.error.rule = k_rule_source_unreadable;
    task.error.message = "cannot read '" + entry.source + "': " + io::status_name(status);
    task.state = TaskState::Failed;
    return;
  }
  // The identity first, whatever the destination: it addresses the cache entry, and the output
  // records it so that a later build can tell it is still the answer.
  const std::span<const u8> source(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  task.source_bytes = bytes.size();
  task.source_hash = hash_bytes(source.data(), source.size());
  task.build_key = step.build_key(task.source_hash);
  task.path = entry.output.empty() ? step.cache_path(*task.ddc, task.build_key) : entry.output;

  io::FileInfo info;
  if (!task.force) {
    u64 recorded_source = 0;
    u64 recorded_key = 0;
    if (step.identity(task.path, recorded_source, recorded_key) && recorded_key != 0 &&
        recorded_source == task.source_hash && recorded_key == task.build_key) {
      if (io::stat_file(task.path, info) == io::Status::Ok) task.bytes = info.size;
      task.state = TaskState::Skipped;
      return;
    }
  }
  if (entry.output.empty()) {
    const io::Status made = io::make_directories(io::parent_path(task.path));
    if (made != io::Status::Ok) {
      task.error.rule = k_rule_derived_build;
      task.error.message = "cannot create '" + std::string(io::parent_path(task.path)) +
                           "': " + io::status_name(made);
      task.state = TaskState::Failed;
      return;
    }
  }
  const i64 start_ns = time::monotonic_ns();
  std::string message;
  task.report = JsonValue::object();
  if (!step.build(source, task.source_hash, task.build_key, task.path, task.report, message)) {
    task.error.rule = k_rule_derived_build;
    task.error.message = entry.source + ": " + message;
    task.state = TaskState::Failed;
    return;
  }
  task.build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  if (io::stat_file(task.path, info) == io::Status::Ok) task.bytes = info.size;
  task.state = TaskState::Built;
  ENGINE_LOG_DEBUG(log_content, "derived output built", log::field("step", step.name),
                   log::field("source", entry.source), log::field("path", task.path),
                   log::field("bytes", task.bytes));
}

void run_derived_tasks(std::span<DerivedTask> tasks, jobs::JobSystem& pool) {
  Vector<jobs::Job> job_list;
  job_list.reserve(static_cast<u32>(tasks.size()));
  jobs::Counter counter;
  for (DerivedTask& task : tasks)
    job_list.push_back(jobs::Job{run_derived_task, &task, &counter});
  if (job_list.empty()) return;
  pool.schedule(jobs::Pool::Performance,
                std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
  pool.wait(counter);
}

}  // namespace engine::content_build
