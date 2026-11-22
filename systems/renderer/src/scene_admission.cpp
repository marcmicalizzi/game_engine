// Content classes decided when a scene is loaded (C64; docs/content-classes.md, "What decided at
// instantiation means"; renderer.md, "Content classes at load").
#include "scene_admission.h"

#include "scene_flora.h"

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/profiling/startup_trace.h>  // stages only: timing, never logic
#include <core/time/time.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/admission.h>
#include <systems/renderer/scene.h>

#include <algorithm>
#include <renderer_log.h>
#include <schemas/provenance.h>
#include <span>
#include <string>
#include <utility>

namespace engine::renderer {

content_class::NamedPolicy agent_default_view_policy() {
  // `agent-default` is the first policy of every set, compiled in (content_class.md).
  const content_class::ViewPolicies set;
  return set.at(0);
}

ReadClass join(const ReadClass& a, const ReadClass& b) {
  ReadClass out;
  out.classes = content_class::join(a.classes, b.classes);
  out.unreadable = a.unreadable || b.unreadable;
  return out;
}

content_class::Decision admit_instance(SceneData& scene, const ReadClass& mesh,
                                       const ReadClass& textures, const ReadClass& raise) {
  const ReadClass all = join(join(mesh, textures), raise);
  // The one branch an unclassified instance costs: the table's first entry is this class, admitted.
  if (content_class::is_none(all.classes)) return content_class::Decision{};
  const content_class::Decision decision = scene.decisions.decide(all.classes);
  if (decision.outcome == content_class::Outcome::admit) {
    scene.held = content_class::join(scene.held, all.classes);
    return decision;
  }
  const bool omitted = decision.outcome == content_class::Outcome::omit;
  SceneLoadReport& report = scene.load;
  if (all.unreadable) {
    ++(omitted ? report.unreadable_omitted : report.unreadable_proxied);
    return decision;
  }
  const std::string& name = scene.categories.name(decision.category);
  SubstitutedCount* row = nullptr;
  for (SubstitutedCount& existing : report.substituted)
    if (existing.category == name) row = &existing;
  if (row == nullptr) {
    report.substituted.push_back(SubstitutedCount{name, 0, 0});
    row = &report.substituted.back();
  }
  ++(omitted ? row->omitted : row->proxied);
  return decision;
}

content_class::Decision admit_ground(SceneData& scene, const ReadClass& ground) {
  if (content_class::is_none(ground.classes)) return content_class::Decision{};
  content_class::Decision decision = scene.decisions.decide(ground.classes);
  if (decision.outcome == content_class::Outcome::admit) {
    scene.held = content_class::join(scene.held, ground.classes);
    return decision;
  }
  decision.outcome = content_class::Outcome::omit;  // never a box: the ground is left out whole
  ++scene.load.grounds_omitted;
  ENGINE_LOG_INFO(log_renderer, "the scene's ground left out by its policy",
                  log::field("unreadable", ground.unreadable));
  return decision;
}

namespace {

ReadClass read_sidecar_class_counted(const std::string& source,
                                     const content_class::Categories& categories,
                                     Vector<content_class::MaterialClass>* materials,
                                     const content_build::HostReads* reads);

}  // namespace

ReadClass read_sidecar_class(const std::string& source, const content_class::Categories& categories,
                             Vector<content_class::MaterialClass>* materials) {
  return read_sidecar_class_counted(source, categories, materials, nullptr);
}

namespace {

// `read_sidecar_class`, its read counted through `reads` (C72); `reads` holds no recipe of
// `source`'s path, since `source` names no generator, so nothing else of it is consulted.
ReadClass read_sidecar_class_counted(const std::string& source,
                                     const content_class::Categories& categories,
                                     Vector<content_class::MaterialClass>* materials,
                                     const content_build::HostReads* reads) {
  // Through the content build's own reader, so the load and the build agree on what a sidecar says
  // and on the key it moves: nothing of that name is `none`, and anything else there that cannot
  // be read (a link or a junction that leads nowhere, a look that fails, text the classes module
  // refuses) is the unreadable class, never `none` (content_build.md, "Content classes"). An
  // existence probe that follows a link once read a dangling one as no sidecar, and the load then
  // served the classless entry. No pack is named, so nothing but the sidecar is joined in.
  content_build::MeshOptions options;
  options.categories = &categories;
  content_build::SourceClasses read;
  content_build::Diagnostic why;
  ReadClass out;
  out.unreadable = !content_build::read_source_classes(source, options, read, why, reads);
  out.classes = read.classes;  // the unreadable class on a refusal: the reader wrote it
  if (materials != nullptr) *materials = std::move(read.materials);
  if (out.unreadable) {
    ENGINE_LOG_WARN(log_renderer, "a class that cannot be read decides as every category's top",
                    log::field("sidecar", content_class::sidecar_path_of(source)),
                    log::field("error", why.message));
  }
  return out;
}

// A container's section 54 alone, its ladders learned into `categories`: the first round of the
// load's teachers (C64), read before any sidecar is. Counted in `counts` when given (C72).
ReadClass container_section(const std::string& path, content_class::Categories& categories,
                            ContainerProbe* probe, content_build::ClassReads* counts = nullptr) {
  ReadClass out;
  const i64 start_ns = time::monotonic_ns();
  bool opened = false;
  Vector<u8> record;
  struct Counted {
    content_build::ClassReads* counts;
    i64 start;
    const bool& opened;
    const Vector<u8>& record;
    ~Counted() {
      if (counts != nullptr)
        counts->add(content_build::ClassRead::container_section, opened, record.size(),
                    time::monotonic_ns() - start);
    }
  } counted{counts, start_ns, opened, record};
  geometry::ClusterFileReader reader;
  std::string error;
  // The header and the section table only: the load reads the container again, whole, when it
  // reads its geometry, and decides it again there by what that read found (C64).
  if (!reader.open(path, nullptr, &error)) return out;
  opened = true;
  if (probe != nullptr) probe->opened = true;
  const u64 count = reader.element_count(geometry::ClusterSection::ContentClasses);
  if (count != 0) {
    u64 offset = 0;
    u64 bytes = 0;
    bool read = reader.range(geometry::ClusterSection::ContentClasses, 1u, 0, count, offset, bytes);
    if (read) {
      record.resize(static_cast<u32>(bytes));
      u64 got = 0;
      read = reader.file().read_at(offset, record.data(), bytes, got) == io::Status::Ok &&
             got == bytes;
    }
    if (!read) error = "section 54 could not be read";
    if (!read ||
        !content_class::decode_classes(
            categories, std::span<const u8>(record.data(), record.size()), out.classes, error)) {
      out.classes = content_class::unreadable(categories);
      out.unreadable = true;
      ENGINE_LOG_WARN(log_renderer, "a container's class cannot be read", log::field("path", path),
                      log::field("error", error));
    }
  }
  if (probe != nullptr) probe->section = record;
  return out;
}

// A container named outright is joined with a sidecar beside it (content-classes.md, "Two limits,
// stated").
ReadClass with_container_sidecar(const std::string& path, const ReadClass& section,
                                 const content_class::Categories& categories, ContainerProbe* probe,
                                 const content_build::HostReads* reads = nullptr) {
  const ReadClass sidecar = read_sidecar_class_counted(path, categories, nullptr, reads);
  if (probe != nullptr) probe->sidecar = sidecar;
  return join(section, sidecar);
}

}  // namespace

ReadClass read_container_class(const std::string& path, content_class::Categories& categories,
                               ContainerProbe* probe) {
  ContainerProbe local;
  ContainerProbe& seen = probe != nullptr ? *probe : local;
  const ReadClass section = container_section(path, categories, &seen);
  // One the probe cannot open is `none` here, sidecar unread, and decided again by its whole read
  // (`container_class_as_loaded`).
  return seen.opened ? with_container_sidecar(path, section, categories, &seen) : section;
}

ReadClass container_class_as_loaded(const ContainerProbe& probe, const ReadClass& probed,
                                    std::span<const u8> loaded,
                                    content_class::Categories& categories) {
  if (probe.opened && loaded.size() == probe.section.size() &&
      std::equal(loaded.begin(), loaded.end(), probe.section.begin()))
    return probed;
  ReadClass out;
  std::string error;
  if (!content_class::decode_classes(categories, loaded, out.classes, error)) {
    out.classes = content_class::unreadable(categories);
    out.unreadable = true;
  }
  ENGINE_LOG_WARN(log_renderer, "a container decided again by the bytes the load read",
                  log::field("probe_opened", probe.opened), log::field("error", error));
  return join(out, probe.sidecar);
}

namespace {

Vec3 axis(u32 a, f32 s) noexcept {
  return Vec3{a == 0 ? s : 0.0f, a == 1 ? s : 0.0f, a == 2 ? s : 0.0f};
}
f32 component(const Vec3& v, u32 a) noexcept { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

}  // namespace

bool build_proxy_box(const Vec3& lo, const Vec3& hi, const geometry::ClusterLodOptions& options,
                     geometry::ClusterLodMesh& out, std::string* error) {
  constexpr f32 k_min_extent = 0.001f;  // a millimetre: a flat mesh's box is not flat
  const Vec3 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
  const Vec3 half{std::max(hi.x - lo.x, k_min_extent) * 0.5f,
                  std::max(hi.y - lo.y, k_min_extent) * 0.5f,
                  std::max(hi.z - lo.z, k_min_extent) * 0.5f};
  // Each face: its normal's axis and sign, and two axes u and v with u x v the normal, so its
  // corners (-u-v, +u-v, +u+v, -u+v) run counter-clockwise seen from outside.
  struct Face {
    u32 n;
    f32 sign;
    u32 u;
    u32 v;
  };
  constexpr Face k_faces[6] = {{0, 1.0f, 1, 2},  {0, -1.0f, 2, 1}, {1, 1.0f, 2, 0},
                               {1, -1.0f, 0, 2}, {2, 1.0f, 0, 1},  {2, -1.0f, 1, 0}};
  constexpr f32 k_corner_s[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
  constexpr f32 k_corner_t[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  positions.reserve(24);
  normals.reserve(24);
  uvs.reserve(24);
  indices.reserve(36);
  for (const Face& face : k_faces) {
    const u32 base = positions.size();
    const Vec3 normal = axis(face.n, face.sign);
    for (u32 c = 0; c < 4; ++c) {
      const Vec3 offset = axis(face.n, face.sign * component(half, face.n)) +
                          axis(face.u, k_corner_s[c] * component(half, face.u)) +
                          axis(face.v, k_corner_t[c] * component(half, face.v));
      positions.push_back(center + offset);
      normals.push_back(normal);
      uvs.push_back(Vec2{(k_corner_s[c] + 1.0f) * 0.5f, (k_corner_t[c] + 1.0f) * 0.5f});
    }
    for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
      indices.push_back(base + i);
  }
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(normals.data(), normals.size());
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  std::string build_error;
  if (!geometry::build_cluster_lod(positions, indices, options, out, &build_error, attributes)) {
    if (error != nullptr) *error = "the proxy box could not be built: " + build_error;
    return false;
  }
  return true;
}

assets::Material proxy_material() {
  assets::Material material;
  material.name = "proxy";
  material.base_color = Vec4{k_proxy_albedo, k_proxy_albedo, k_proxy_albedo, 1.0f};
  material.metallic = 0.0f;
  material.roughness = 1.0f;
  return material;
}

void leaf_bounds(const geometry::ClusterLodMesh& lod, Vec3& lo, Vec3& hi) {
  lo = Vec3{1e30f, 1e30f, 1e30f};
  hi = Vec3{-1e30f, -1e30f, -1e30f};
  bool any = false;
  for (u32 i = 0; i < lod.lod.size() && i < lod.mesh.clusters.size(); ++i) {
    if (lod.lod[i].level != 0) continue;
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
              std::min(lo.z, c.center.z - c.radius)};
    hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
              std::max(hi.z, c.center.z + c.radius)};
    any = true;
  }
  if (!any) {
    lo = Vec3{-0.5f, -0.5f, -0.5f};
    hi = Vec3{0.5f, 0.5f, 0.5f};
  }
}

// ---- the load's passes ------------------------------------------------------------------------

namespace {

// The load makes the instances itself (the implicit one, or a grid that replaces the file's), all
// of mesh 0 with no raise.
bool made_by_load(const SceneDesc& desc) noexcept {
  return desc.instances.empty() || desc.grid_instances > 1;
}

ReadClass raise_of(const LoadAdmission& state, u16 raise) {
  return raise == 0 || raise > state.raises.size() ? ReadClass{} : state.raises[raise - 1u];
}

// A box's page table when the scene streams (C64), so a substitution keeps a streamed scene on
// the streaming path, and keeps the gate the authored instances stream by where it was; none when
// the scene does not stream, as a mesh read whole has none.
geometry::ClusterPages proxy_pages(const SceneDesc& desc, geometry::ClusterLodMesh& box) {
  geometry::ClusterPages pages;
  if (!desc.stream) return pages;
  std::string why;
  if (!geometry::build_cluster_pages(box, geometry::ClusterPagesOptions{}, pages, &why)) {
    ENGINE_LOG_WARN(log_renderer, "proxy box pages", log::field("error", why));
    pages = geometry::ClusterPages{};
  }
  return pages;
}

// The ground's class (C64): the join of what the sidecars beside its heightfield source (a tile
// set's manifest or heightfield header), beside every image its layers name and beside every
// image its masks are painted from declare, each read
// as the content build reads one, so a sidecar there that cannot be read refuses the ground.
ReadClass ground_class(const TerrainDesc& terrain, const content_class::Categories& categories,
                       content_build::ClassReads* counts) {
  ReadClass out;
  // Its sidecars counted apart, as the ground's inputs (C72).
  content_build::ClassReads own;
  content_build::HostReads reads;
  reads.counts = &own;
  const auto beside = [&](const std::string& input) {
    if (!input.empty())
      out = join(out, read_sidecar_class_counted(input, categories, nullptr, &reads));
  };
  struct Counted {
    content_build::ClassReads* counts;
    const content_build::ClassReads& own;
    ~Counted() {
      if (counts == nullptr) return;
      content_build::ClassReadCount& ground = (*counts)[content_build::ClassRead::ground_inputs];
      for (const content_build::ClassReadCount& c : own.kinds) {
        ground.asked += c.asked;
        ground.found += c.found;
        ground.bytes += c.bytes;
        ground.ns += c.ns;
      }
    }
  } counted{counts, own};
  if (terrain.tiles.has_value()) {
    beside(terrain.tiles->manifest);
    beside(terrain.tiles->heightfield);
  }
  if (terrain.materials.has_value()) {
    for (const scene::TerrainMaterialLayer& layer : terrain.materials->layers)
      for (const std::string* image :
           {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask, &layer.height})
        beside(*image);
  }
  // And the weight maps its mask level is composited from, which are read, cooked and drawn as
  // the layers' placement.
  if (terrain.masks.has_value()) {
    for (const scene::TerrainMaskPaint& paint : terrain.masks->paint)
      beside(paint.image);
    if (terrain.masks->index.has_value()) beside(terrain.masks->index->image);
  }
  return out;
}

// **The class read's measurement** (C72): one line, typed fields, for each kind of read how many
// were asked for, found a file, the bytes and the milliseconds, beside the distinct paths and the
// stage's own milliseconds.
void log_class_reads(const SceneLoadReport& load) {
  constexpr log::Level k_level = log::Level::Info;
  if (!log_renderer.enabled(k_level)) return;
  // A procedural scene with no ground reads nothing, and says nothing.
  if (load.class_paths == 0 && load.class_reads[content_build::ClassRead::ground_inputs].asked == 0)
    return;
  constexpr u32 k_per_kind = 4;
  constexpr u32 k_fields = 2 + k_per_kind * content_build::k_class_read_kinds;
  // The keys outlive the emit: string literals, `<kind>.<what>`.
#define ENGINE_CLASS_READ_KEYS(kind) kind ".asked", kind ".found", kind ".bytes", kind ".ms"
  constexpr const char* k_keys[k_per_kind * content_build::k_class_read_kinds] = {
      ENGINE_CLASS_READ_KEYS("container_section"), ENGINE_CLASS_READ_KEYS("container_sidecar"),
      ENGINE_CLASS_READ_KEYS("recipe_file"),       ENGINE_CLASS_READ_KEYS("recipe_sidecar"),
      ENGINE_CLASS_READ_KEYS("pack_sets"),         ENGINE_CLASS_READ_KEYS("host_probe"),
      ENGINE_CLASS_READ_KEYS("host_record"),       ENGINE_CLASS_READ_KEYS("gltf_sidecar"),
      ENGINE_CLASS_READ_KEYS("ground_inputs")};
#undef ENGINE_CLASS_READ_KEYS
  log::Field fields[k_fields];
  fields[0] = log::field("paths", load.class_paths);
  fields[1] = log::field("ms", load.classes_ms);
  for (u32 k = 0; k < content_build::k_class_read_kinds; ++k) {
    const content_build::ClassReadCount& c = load.class_reads.kinds[k];
    fields[2 + k * k_per_kind + 0] = log::field(k_keys[k * k_per_kind + 0], c.asked);
    fields[2 + k * k_per_kind + 1] = log::field(k_keys[k * k_per_kind + 1], c.found);
    fields[2 + k * k_per_kind + 2] = log::field(k_keys[k * k_per_kind + 2], c.bytes);
    fields[2 + k * k_per_kind + 3] =
        log::field(k_keys[k * k_per_kind + 3], static_cast<f64>(c.ns) / 1.0e6);
  }
  // `ENGINE_LOG_INFO` takes its fields as arguments; these are an array, so the emit is direct.
  log::emit(log_renderer, k_level, __FILE__, static_cast<u32>(__LINE__), "content classes read",
            std::span<const log::Field>(fields, k_fields));
}

// The source a proxy box stands in with: one material, every cluster its one part, no image, no
// container to page from, no motion, no seat, no far form.
SourceMesh proxy_source(const geometry::ClusterLodMesh& box) {
  SourceMesh source;
  source.data.materials.push_back(proxy_material());
  source.part_material.push_back(0);
  source.part_of_cluster = Vector<u32>(box.mesh.clusters.size(), 0u);
  return source;
}

}  // namespace

bool admission_before_meshes(const SceneDesc& desc, SceneData& out, LoadAdmission& state,
                             std::string& error, jobs::JobSystem* pool) {
  // **A stage of its own** (C72; profiling.md, "The trace is live"), its items the distinct paths
  // read, so a trace names the class read's time.
  ENGINE_STARTUP_STAGE("content classes");
  const i64 start_ns = time::monotonic_ns();
  state.reads = content_build::ClassReads{};
  out.instantiated_under = desc.view_policy;
  out.categories = content_class::Categories();
  out.held = content_class::ContentClasses{};
  const u32 meshes = desc.meshes.size();
  state.mesh.assign(meshes, ReadClass{});
  state.mesh_hash.assign(meshes, 0ull);
  state.record.resize(meshes);
  state.load.assign(meshes, u8{0});
  const materials::Pack* pack = materials::pack_at(desc.materials);
  // One read a distinct path: a scene naming one container ten thousand times opens it once (the
  // per-mesh read cost 0.46 ms a mesh in `msvc-debug` before this, measured on such a scene).
  // `first[m]` is the first mesh of `m`'s path, `m` itself for the first and for the ground.
  Vector<u32> first(meshes, 0u);
  u32 distinct = 0;
  {
    HashMap<u64, u32> first_of_path;
    for (u32 m = 0; m < meshes; ++m) {
      const std::string& path = desc.meshes[m];
      first[m] = m;
      if (path.empty()) continue;
      const u64 path_hash = hash_bytes(path.data(), path.size());
      if (const u32* at = first_of_path.find_value(path_hash); at == nullptr)
        first_of_path.try_emplace(path_hash, m);
      else if (desc.meshes[*at] == path)
        first[m] = *at;
      if (first[m] == m) ++distinct;
    }
  }
  profiling::trace_items(distinct);
  state.ground.assign(meshes, u8{0});
  state.probe.resize(meshes);
  state.loaded.resize(meshes);
  state.read_whole.assign(meshes, u8{0});
  // **Every ladder is learned before any class is read** (C64), so what a scene admits, leaves out
  // and counts, and every key, are the same in whichever order it lists its meshes, wherever no two
  // teachers disagree. Round one: every container the scene lists, in the order listed, teaches
  // the table from its own section 54 (decided by it; its sidecar is read with the classes below).
  // Round two: every recipe's container hosts, the whole chain, in the order the recipes are
  // listed, through the same `decode_classes`, whole or not at all, so a host the scene does not
  // list teaches only a category no listed container taught, and one whose ladder differs from
  // one learned is refused as a second listed container is, its recipe unreadable. What round two
  // reads is kept, and the class read below takes it from there, opening and hashing nothing
  // twice.
  const content_build::MeshOptions recipe_options = [&] {
    content_build::MeshOptions options;
    options.materials = pack;
    options.categories = &out.categories;
    return options;
  }();
  for (u32 m = 0; m < meshes; ++m) {
    const std::string& path = desc.meshes[m];
    if (first[m] == m && !path.empty() && !is_generated_mesh(path) &&
        io::extension(path) == ".clusters")
      state.mesh[m] = container_section(path, out.categories, &state.probe[m], &state.reads);
  }
  content_build::HostReads host_reads;
  host_reads.counts = &state.reads;
  // **The reads leave the serial loop** (C72): every distinct recipe's file (or its handed bytes)
  // read and its generator's hash run under the probe's hooks on the load's pool, each into its own
  // slot, and the hosts they name in waves; the round below then walks in listed order exactly as
  // before, taking each answer from those slots, so what it learns and refuses is the same at any
  // thread count. On the forest's scene this was 63 of the pass's 67 s, one recipe after another.
  {
    Vector<content_build::RecipeAsk> asks;
    for (u32 m = 0; m < meshes; ++m) {
      const std::string& path = desc.meshes[m];
      if (first[m] != m || path.empty() || !is_generated_mesh(path)) continue;
      const MeshRecipe* handed = handed_recipe(desc, path);
      asks.push_back(content_build::RecipeAsk{&path, handed != nullptr ? &handed->bytes : nullptr});
    }
    content_build::probe_recipes_ahead(
        std::span<const content_build::RecipeAsk>(asks.data(), asks.size()), recipe_options,
        host_reads, pool);
  }
  for (u32 m = 0; m < meshes; ++m) {
    const std::string& path = desc.meshes[m];
    // A refusal here is the class read's below, by the same reads, which says why. A recipe the
    // scene hands as bytes is probed from them, the bytes the load builds it from.
    if (first[m] != m || path.empty() || !is_generated_mesh(path)) continue;
    const MeshRecipe* handed = handed_recipe(desc, path);
    (void)content_build::learn_source_hosts(path, recipe_options, out.categories, host_reads,
                                            handed != nullptr ? &handed->bytes : nullptr);
  }
  // The table is finished: the pack's sets are read against it once, not once a recipe (C72).
  content_build::keep_pack_classes(recipe_options, host_reads);
  // Then the classes, in mesh order, against the finished table: every sidecar, pack and ground.
  for (u32 m = 0; m < meshes; ++m) {
    const std::string& path = desc.meshes[m];
    if (path.empty()) {
      // A procedural scene is the engine's own, `none`. The ground is its inputs' (C64): the join
      // of the sidecars beside its heightfield source and every layer's images, read before any of
      // them is; its cache key and the entry the load builds hold that class, as a mesh's do.
      if (!desc.terrain.enabled) continue;
      state.ground[m] = 1;
      state.mesh[m] = ground_class(desc.terrain, out.categories, &state.reads);
      if (!content_class::is_none(state.mesh[m].classes)) {
        state.mesh_hash[m] = content_class::class_hash(out.categories, state.mesh[m].classes);
        content_class::encode_classes(out.categories, state.mesh[m].classes, nullptr,
                                      state.record[m]);
        state.classified = true;
      }
      continue;
    }
    if (first[m] != m) {
      state.mesh[m] = state.mesh[first[m]];
      state.mesh_hash[m] = state.mesh_hash[first[m]];
      state.record[m] = state.record[first[m]];
      state.probe[m] = state.probe[first[m]];
      continue;
    }
    if (is_generated_mesh(path)) {
      // A generator's output is the join of what it baked: its recipe's sidecar, every set of the
      // pack it falls back to and its hosts' classes (content_build.h, `read_source_classes`), each
      // host's record taken from what round two read.
      content_build::SourceClasses classes;
      content_build::Diagnostic why;
      state.mesh[m].unreadable =
          !content_build::read_source_classes(path, recipe_options, classes, why, &host_reads);
      state.mesh[m].classes = classes.classes;
      // What its cache key holds and the entry the load builds carries, as the content build's.
      state.mesh_hash[m] = classes.hash;
      state.record[m] = std::move(classes.record);
    } else if (io::extension(path) == ".clusters") {
      // Its section 54 was read in round one; one the probe could not open stays `none` here.
      if (state.probe[m].opened)
        state.mesh[m] = with_container_sidecar(path, state.mesh[m], out.categories, &state.probe[m],
                                               &host_reads);
    } else {
      Vector<content_class::MaterialClass> materials;
      state.mesh[m] = read_sidecar_class_counted(path, out.categories, &materials, &host_reads);
      // What a classified glTF's cache entry is keyed by, and holds (C62's rule): the class by
      // name, only when there is one, so an unclassified source keeps its key.
      if (!content_class::is_none(state.mesh[m].classes)) {
        state.mesh_hash[m] =
            content_class::class_hash(out.categories, state.mesh[m].classes, &materials);
        content_class::encode_classes(out.categories, state.mesh[m].classes, &materials,
                                      state.record[m]);
      }
    }
    state.classified = state.classified || !content_class::is_none(state.mesh[m].classes);
  }
  // **What could not be read is decided with what the scene knows at the decision** (C64): the
  // read keeps the fact (`ReadClass::unreadable`), and its levels are made here, from every
  // category the scene knows now, so a ladder taught after a container round one refused is in it
  // and the outcome does not depend on the order of the meshes. What the load builds for one (a
  // ground's, a glTF's or a recipe's entry: its key's class term and its section 54) is taken
  // from the class made here, so an entry admitted records every category its decision used; a
  // container named outright builds nothing.
  for (u32 m = 0; m < meshes; ++m) {
    ReadClass& read = state.mesh[m];
    if (!read.unreadable) continue;
    read.classes = content_class::unreadable(out.categories);
    if (state.ground[m] == 0 && io::extension(desc.meshes[m]) == ".clusters") continue;
    state.mesh_hash[m] = content_class::class_hash(out.categories, read.classes);
    content_class::encode_classes(out.categories, read.classes, nullptr, state.record[m]);
  }
  // Every ladder the containers carry is in the table now: the policy's game caps resolve against
  // it, and the scene's table is made under what they resolve to.
  const content_class::ViewPolicy policy =
      content_class::resolve(out.instantiated_under, out.categories);
  out.instantiated_under.policy = policy;
  out.decisions = content_class::DecisionTable(policy);
  // The raises, read against the same table, so a raise naming a game category reads only when
  // the scene's data has brought its ladder; one that names a category nobody defined is the
  // unreadable class, and fails closed.
  state.raises.assign(desc.raises.size(), ReadClass{});
  for (u32 r = 0; r < desc.raises.size(); ++r) {
    std::string why;
    state.raises[r].unreadable =
        !content_class::read_classes(out.categories, desc.raises[r], state.raises[r].classes, why);
    state.classified = state.classified || !content_class::is_none(state.raises[r].classes);
  }
  // Every read the class read made is in: the measurement, once (C72).
  out.load.class_paths = distinct;
  out.load.class_reads = state.reads;
  out.load.classes_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  log_class_reads(out.load);
  // Which meshes are read: one an instance would draw (admitted or proxied: a proxy needs the
  // mesh's bounds), or one no instance names whose own class admits it.
  Vector<u8> named(meshes, u8{0});
  const auto drawn = [&](u32 mesh, u16 raise) {
    const ReadClass c = join(state.mesh[mesh], raise_of(state, raise));
    return content_class::decide(c.classes, policy).outcome != content_class::Outcome::omit;
  };
  if (made_by_load(desc)) {
    if (meshes > 0) {
      named[0] = 1;
      if (drawn(0, 0)) state.load[0] = 1;
    }
  }
  for (u32 i = 0; desc.grid_instances <= 1 && i < desc.instances.size(); ++i) {
    const SceneInstance& instance = desc.instances[i];
    if (instance.content_classes != 0 && instance.content_classes > desc.raises.size()) {
      error = "instance " + std::to_string(i) + " names raise " +
              std::to_string(instance.content_classes) + ", and the scene has " +
              std::to_string(desc.raises.size());
      return false;
    }
    if (instance.mesh >= meshes) continue;  // the load refuses it later, with its sentence
    named[instance.mesh] = 1;
    if (drawn(instance.mesh, instance.content_classes)) state.load[instance.mesh] = 1;
  }
  for (u32 m = 0; m < meshes; ++m)
    if (named[m] == 0 && drawn(m, 0)) state.load[m] = 1;
  // **Substitution is the placer's, not the slot's** (C64): a mesh something other than the file's
  // instances places (a streamed entry's resident mesh, a prop's, a kept authored instance's; what
  // `open_scene_meshes` marks) is read whenever its own class lets it be drawn, whatever a raised
  // instance of it decided, so those placers get the mesh, or its box when its own class says so.
  state.placed.assign(meshes, u8{0});
  for (const StreamedPlacements& entry : desc.streamed)
    for (const u32 m : entry.meshes)
      if (m < meshes) state.placed[m] = 1;
  for (const SceneProp& prop : desc.props)
    if (prop.mesh < meshes) state.placed[prop.mesh] = 1;
  for (const u32 i : desc.authored.keep)
    if (i < desc.instances.size() && desc.instances[i].mesh < meshes)
      state.placed[desc.instances[i].mesh] = 1;
  for (u32 m = 0; m < meshes; ++m)
    if (state.placed[m] != 0 && drawn(m, 0)) state.load[m] = 1;
  // The ground is built only when the policy admits it: refused, nothing of it is read (C64).
  for (u32 m = 0; m < meshes; ++m) {
    if (state.ground[m] == 0) continue;
    const bool admitted = content_class::decide(state.mesh[m].classes, policy).outcome ==
                          content_class::Outcome::admit;
    state.ground_refused = state.ground_refused || !admitted;
    state.load[m] = admitted ? 1 : 0;
  }
  return true;
}

content_class::Outcome admit_made_instance(SceneData& out, const LoadAdmission& state) {
  if (!state.ground.empty() && state.ground[0] != 0)
    return admit_ground(out, state.mesh0_own).outcome;
  return admit_instance(out, state.mesh0_own, ReadClass{}, ReadClass{}).outcome;
}

void admission_after_meshes(SceneDesc& desc, SceneData& out, LoadAdmission& state,
                            Vector<geometry::ClusterLodMesh>& dags,
                            Vector<geometry::ClusterPages>& tables) {
  const u32 meshes = state.mesh.size();
  // 0. A container named outright is decided by the bytes its whole read found, when they are not
  // the ones its probe saw (C64): replaced or finished between the two opens, it is what was
  // loaded that decides, and a mesh whose own class now refuses it becomes its box below.
  for (u32 m = 0; m < meshes && m < state.read_whole.size(); ++m) {
    if (state.read_whole[m] == 0) continue;
    state.mesh[m] = container_class_as_loaded(
        state.probe[m], state.mesh[m],
        std::span<const u8>(state.loaded[m].data(), state.loaded[m].size()), out.categories);
    state.classified = state.classified || !content_class::is_none(state.mesh[m].classes);
  }
  // 1. Every image named by path of a mesh that is read: its sidecar probed, once a distinct file.
  Vector<ReadClass> textures(meshes, ReadClass{});
  state.image_hash.resize(meshes);
  HashMap<u64, u32> probed;  // path hash to its index in `seen`
  // A probed file: its path, its class, and the hash its texture key holds, over its materials
  // too, as the content build's texture step takes it, so the `.tex` the step writes on a miss is
  // the one this key names.
  struct Probed {
    std::string path;
    ReadClass read;
    u64 hash = 0;
  };
  Vector<Probed> seen;
  for (u32 m = 0; m < meshes; ++m) {
    if (state.load[m] == 0) continue;
    const SourceMesh& source = out.sources[m];
    for (u32 i = 0; i < source.data.images.size(); ++i) {
      const assets::ImageRef& image = source.data.images[i];
      if (!image.bytes.empty() || image.uri.empty()) continue;  // embedded: the container's class
      const std::string path = io::normalize_path(
          source.image_dir.empty() ? image.uri : io::join_path(source.image_dir, image.uri));
      const u64 key = hash_bytes(path.data(), path.size());
      u32 index = ~0u;
      const u32* found = probed.find_value(key);
      if (found != nullptr && seen[*found].path == path) {
        index = *found;
      } else {
        // A hash two paths share keeps the first; the second is probed every time it is met.
        index = seen.size();
        Vector<content_class::MaterialClass> materials;
        Probed probe;
        probe.path = path;
        probe.read = read_sidecar_class(path, out.categories, &materials);
        probe.hash = content_class::class_hash(out.categories, probe.read.classes, &materials);
        ++out.load.image_sidecars_probed;
        if (!content_class::is_none(probe.read.classes)) ++out.load.image_sidecars_found;
        if (found == nullptr) probed.try_emplace(key, index);
        seen.push_back(std::move(probe));
      }
      const ReadClass read = seen[index].read;
      if (content_class::is_none(read.classes)) continue;
      state.classified = true;
      textures[m] = join(textures[m], read);
      if (state.image_hash[m].empty()) state.image_hash[m].assign(source.data.images.size(), 0ull);
      state.image_hash[m][i] = seen[index].hash;
    }
  }
  state.proxy_of.assign(meshes, ~0u);
  // Mesh 0's own class, for an instance the load makes of it after a streamed world's tail took
  // every file instance (`admit_made_instance`).
  if (meshes > 0) state.mesh0_own = join(state.mesh[0], textures[0]);
  if (!state.classified) return;  // nothing above `none` anywhere: the load is what it was

  // 2. Every instance through the door.
  Vector<content_class::Outcome> outcome(desc.instances.size(), content_class::Outcome::admit);
  Vector<u32> admitted(meshes, 0u);
  Vector<u8> proxied(meshes, u8{0});
  // The door, or for the ground its own form (left out whole, never a box).
  const auto door = [&](u32 mesh, const ReadClass& raise) {
    if (state.ground[mesh] != 0) return admit_ground(out, state.mesh[mesh]).outcome;
    return admit_instance(out, state.mesh[mesh], textures[mesh], raise).outcome;
  };
  for (u32 i = 0; desc.grid_instances <= 1 && i < desc.instances.size(); ++i) {
    const SceneInstance& instance = desc.instances[i];
    if (instance.mesh >= meshes) continue;
    outcome[i] = door(instance.mesh, raise_of(state, instance.content_classes));
    if (outcome[i] == content_class::Outcome::admit) ++admitted[instance.mesh];
    if (outcome[i] == content_class::Outcome::proxy) proxied[instance.mesh] = 1;
  }
  if (made_by_load(desc) && meshes > 0) {
    // The instances the load makes after the merge (the implicit one, or the grid's), all of mesh
    // 0 with no raise: each through the door, all deciding alike.
    const u32 count = desc.grid_instances > 1 ? desc.grid_instances * desc.grid_instances : 1u;
    for (u32 k = 0; k < count; ++k)
      state.mesh0 = door(0, ReadClass{});
    if (state.mesh0 == content_class::Outcome::admit) admitted[0] += count;
    if (state.mesh0 == content_class::Outcome::proxy) proxied[0] = 1;
  }
  // A mesh no instance names (a prop, a generator's resident mesh) is admitted by its own class.
  Vector<u8> named(meshes, u8{0});
  for (u32 i = 0; desc.grid_instances <= 1 && i < desc.instances.size(); ++i)
    if (desc.instances[i].mesh < meshes) named[desc.instances[i].mesh] = 1;
  if (made_by_load(desc) && meshes > 0) named[0] = 1;
  // Decided outside the door's accounting (C64): `substituted` counts instances, and such a mesh
  // is none. One another placer places is drawn by it, so its class enters the table and, when
  // admitted, `held`; one nothing places draws nothing, and is decided alone.
  for (u32 m = 0; m < meshes; ++m) {
    if (named[m] != 0) continue;
    const ReadClass own = join(state.mesh[m], textures[m]);
    const bool placed = m < state.placed.size() && state.placed[m] != 0;
    content_class::Outcome decided = content_class::Outcome::admit;
    if (content_class::is_none(own.classes)) {
      decided = content_class::Outcome::admit;
    } else if (placed) {
      decided = out.decisions.decide(own.classes).outcome;
      if (decided == content_class::Outcome::admit)
        out.held = content_class::join(out.held, own.classes);
    } else {
      decided = content_class::decide(own.classes, out.instantiated_under.policy).outcome;
    }
    if (decided == content_class::Outcome::admit) ++admitted[m];
  }

  // 3. A mesh becomes its proxy box when its own class (its own and its textures', never an
  // instance's raise) is refused, so every placer, the streamed ones included, places the box; or
  // when nothing admitted draws it and nothing else places it. Its geometry, images, container and
  // motion leave the scene before the texture pass and the merge. A mesh whose own class admits it
  // keeps its slot for the placers, and an instance its raise refused is left out or draws a proxy
  // mesh of its own (step 4).
  for (u32 m = 0; m < meshes; ++m) {
    const content_class::Outcome own =
        content_class::decide(join(state.mesh[m], textures[m]).classes,
                              out.instantiated_under.policy)
            .outcome;
    const bool placed = m < state.placed.size() && state.placed[m] != 0;
    if (own == content_class::Outcome::admit && (admitted[m] != 0 || placed)) continue;
    Vec3 lo;
    Vec3 hi;
    leaf_bounds(dags[m], lo, hi);  // a mesh that was never read: the unit box, which nothing draws
    geometry::ClusterLodMesh box;
    std::string why;
    if (!build_proxy_box(lo, hi, desc.lod, box, &why))
      ENGINE_LOG_WARN(log_renderer, "proxy box", log::field("error", why));
    dags[m] = std::move(box);
    tables[m] = proxy_pages(desc, dags[m]);
    out.sources[m] = proxy_source(dags[m]);
    state.image_hash[m].clear();
    state.proxy_of[m] = m;
    ENGINE_LOG_INFO(log_renderer, "a mesh substituted by its policy", log::field("mesh", m),
                    log::field("proxied", static_cast<u32>(proxied[m])));
  }
  // 4. A mesh drawn for some and proxied for others: a proxy mesh of its own, after the scene's.
  for (u32 m = 0; m < meshes; ++m) {
    if (proxied[m] == 0 || state.proxy_of[m] != ~0u) continue;
    Vec3 lo;
    Vec3 hi;
    leaf_bounds(dags[m], lo, hi);
    geometry::ClusterLodMesh box;
    std::string why;
    if (!build_proxy_box(lo, hi, desc.lod, box, &why))
      ENGINE_LOG_WARN(log_renderer, "proxy box", log::field("error", why));
    state.proxy_of[m] = desc.meshes.size();
    // Not empty: an empty path is the terrain's or a procedural scene's.
    desc.meshes.push_back("proxy of mesh " + std::to_string(m));
    if (!desc.mesh_info.empty()) {
      SceneMeshInfo info;
      info.name = desc.mesh_info[m].name + " (proxy)";
      info.fit = desc.mesh_info[m].fit;  // placed as the mesh it stands in for
      desc.mesh_info.push_back(std::move(info));
    }
    out.sources.push_back(proxy_source(box));
    tables.push_back(proxy_pages(desc, box));
    dags.push_back(std::move(box));
    state.image_hash.push_back(Vector<u64>());
  }
  // 5. The instances: a proxied one draws its proxy, an omitted one is marked for the instance
  // table to leave out, and in a streamed world neither is handed to the tail, so the tail never
  // holds what the policy withheld (the tile's own admission is the live re-decision's, C67).
  for (u32 i = 0; desc.grid_instances <= 1 && i < desc.instances.size(); ++i) {
    SceneInstance& instance = desc.instances[i];
    if (outcome[i] == content_class::Outcome::admit) continue;
    if (outcome[i] == content_class::Outcome::proxy) {
      instance.mesh = state.proxy_of[instance.mesh];
      instance.content_classes = 0;
    } else {
      instance.content_classes = k_instance_omitted;
    }
    instance.animation.play = false;  // a box plays no clip
    instance.joints = 0;
    if (desc.world.enabled) desc.authored.keep.push_back(i);
  }
}

}  // namespace engine::renderer
