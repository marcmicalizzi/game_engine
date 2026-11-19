// Content classes decided when a scene is loaded (C64; docs/content-classes.md, "What decided at
// instantiation means"; renderer.md, "Content classes at load").
#include "scene_admission.h"

#include "scene_flora.h"

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/admission.h>
#include <systems/renderer/scene.h>

#include <algorithm>
#include <renderer_log.h>
#include <schemas/provenance.h>
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

ReadClass read_sidecar_class(const std::string& source, const content_class::Categories& categories,
                             Vector<content_class::MaterialClass>* materials) {
  ReadClass out;
  const std::string sidecar = content_class::sidecar_path_of(source);
  if (!io::exists(sidecar)) return out;
  std::string text;
  std::string error;
  if (io::read_file(sidecar, text) != io::Status::Ok) {
    error = "the sidecar could not be read";
    out.classes = content_class::unreadable(categories);
    out.unreadable = true;
  } else if (!content_class::read_sidecar_classes(categories, text, out.classes, error,
                                                  materials)) {
    out.unreadable = true;  // `out.classes` is the unreadable class: the reader wrote it
  }
  if (out.unreadable) {
    ENGINE_LOG_WARN(log_renderer, "a class that cannot be read decides as every category's top",
                    log::field("sidecar", sidecar), log::field("error", error));
  }
  return out;
}

ReadClass read_container_class(const std::string& path, content_class::Categories& categories) {
  ReadClass out;
  geometry::ClusterFileReader reader;
  std::string error;
  // The header and the section table only: the load reads the container again, whole, when it
  // reads its geometry, and fails there with its own sentence if it cannot.
  if (!reader.open(path, nullptr, &error)) return out;
  const u64 count = reader.element_count(geometry::ClusterSection::ContentClasses);
  if (count != 0) {
    u64 offset = 0;
    u64 bytes = 0;
    Vector<u8> record;
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
  // A container named outright is joined with a sidecar beside it (content-classes.md, "Two
  // limits, stated").
  return join(out, read_sidecar_class(path, categories));
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
                             std::string& error) {
  out.instantiated_under = desc.view_policy;
  out.categories = content_class::Categories();
  out.held = content_class::ContentClasses{};
  const u32 meshes = desc.meshes.size();
  state.mesh.assign(meshes, ReadClass{});
  state.mesh_hash.assign(meshes, 0ull);
  state.record.resize(meshes);
  state.load.assign(meshes, u8{0});
  const materials::Pack* pack = materials::pack_at(desc.materials);
  for (u32 m = 0; m < meshes; ++m) {
    const std::string& path = desc.meshes[m];
    if (path.empty()) continue;  // the terrain and the procedural scenes: the engine's own, `none`
    if (is_generated_mesh(path)) {
      // A generator's output is the join of what it baked: its recipe's sidecar and every set of
      // the pack it falls back to (content_build.h, `read_source_classes`).
      content_build::MeshOptions options;
      options.materials = pack;
      options.categories = &out.categories;
      content_build::SourceClasses classes;
      content_build::Diagnostic why;
      state.mesh[m].unreadable = !content_build::read_source_classes(path, options, classes, why);
      state.mesh[m].classes = classes.classes;
    } else if (io::extension(path) == ".clusters") {
      state.mesh[m] = read_container_class(path, out.categories);
    } else {
      Vector<content_class::MaterialClass> materials;
      state.mesh[m] = read_sidecar_class(path, out.categories, &materials);
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
  return true;
}

void admission_after_meshes(SceneDesc& desc, SceneData& out, LoadAdmission& state,
                            Vector<geometry::ClusterLodMesh>& dags,
                            Vector<geometry::ClusterPages>& tables) {
  const u32 meshes = state.mesh.size();
  // 1. Every image named by path of a mesh that is read: its sidecar probed, once a distinct file.
  Vector<ReadClass> textures(meshes, ReadClass{});
  state.image_hash.resize(meshes);
  HashMap<u64, u32> probed;  // path hash to its index in `seen`
  Vector<std::pair<std::string, ReadClass>> seen;
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
      if (found != nullptr && seen[*found].first == path) {
        index = *found;
      } else {
        // A hash two paths share keeps the first; the second is probed every time it is met.
        index = seen.size();
        ReadClass read = read_sidecar_class(path, out.categories);
        ++out.load.image_sidecars_probed;
        if (!content_class::is_none(read.classes)) ++out.load.image_sidecars_found;
        if (found == nullptr) probed.try_emplace(key, index);
        seen.push_back({path, read});
      }
      const ReadClass read = seen[index].second;
      if (content_class::is_none(read.classes)) continue;
      state.classified = true;
      textures[m] = join(textures[m], read);
      if (state.image_hash[m].empty()) state.image_hash[m].assign(source.data.images.size(), 0ull);
      state.image_hash[m][i] = content_class::class_hash(out.categories, read.classes);
    }
  }
  state.proxy_of.assign(meshes, ~0u);
  if (!state.classified) return;  // nothing above `none` anywhere: the load is what it was

  // 2. Every instance through the door.
  Vector<content_class::Outcome> outcome(desc.instances.size(), content_class::Outcome::admit);
  Vector<u32> admitted(meshes, 0u);
  Vector<u8> proxied(meshes, u8{0});
  for (u32 i = 0; desc.grid_instances <= 1 && i < desc.instances.size(); ++i) {
    const SceneInstance& instance = desc.instances[i];
    if (instance.mesh >= meshes) continue;
    outcome[i] = admit_instance(out, state.mesh[instance.mesh], textures[instance.mesh],
                                raise_of(state, instance.content_classes))
                     .outcome;
    if (outcome[i] == content_class::Outcome::admit) ++admitted[instance.mesh];
    if (outcome[i] == content_class::Outcome::proxy) proxied[instance.mesh] = 1;
  }
  if (made_by_load(desc) && meshes > 0) {
    // The instances the load makes after the merge (the implicit one, or the grid's), all of mesh
    // 0 with no raise: each through the door, all deciding alike.
    const u32 count = desc.grid_instances > 1 ? desc.grid_instances * desc.grid_instances : 1u;
    for (u32 k = 0; k < count; ++k)
      state.mesh0 = admit_instance(out, state.mesh[0], textures[0], ReadClass{}).outcome;
    if (state.mesh0 == content_class::Outcome::admit) admitted[0] += count;
    if (state.mesh0 == content_class::Outcome::proxy) proxied[0] = 1;
  }
  // A mesh no instance names (a prop, a generator's resident mesh) is admitted by its own class.
  Vector<u8> named(meshes, u8{0});
  for (u32 i = 0; desc.grid_instances <= 1 && i < desc.instances.size(); ++i)
    if (desc.instances[i].mesh < meshes) named[desc.instances[i].mesh] = 1;
  if (made_by_load(desc) && meshes > 0) named[0] = 1;
  for (u32 m = 0; m < meshes; ++m) {
    if (named[m] != 0) continue;
    const content_class::Decision d = admit_instance(out, state.mesh[m], textures[m], ReadClass{});
    if (d.outcome == content_class::Outcome::admit) ++admitted[m];
  }

  // 3. A mesh nothing admitted draws becomes its proxy box: its geometry, images, container and
  // motion leave the scene before the texture pass and the merge.
  for (u32 m = 0; m < meshes; ++m) {
    if (admitted[m] != 0) continue;
    Vec3 lo;
    Vec3 hi;
    leaf_bounds(dags[m], lo, hi);  // a mesh that was never read: the unit box, which nothing draws
    geometry::ClusterLodMesh box;
    std::string why;
    if (!build_proxy_box(lo, hi, desc.lod, box, &why))
      ENGINE_LOG_WARN(log_renderer, "proxy box", log::field("error", why));
    dags[m] = std::move(box);
    tables[m] = geometry::ClusterPages{};
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
    dags.push_back(std::move(box));
    tables.push_back(geometry::ClusterPages{});
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
