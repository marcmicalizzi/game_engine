// A resident mesh's far form, offered to the rings' rule (G198; far_tile.h; ring_forms.h,
// `OfferedForm`; renderer.md, "The far tile").
#include <core/hash/hash.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/far_form.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/far_tile.h>

#include <algorithm>
#include <renderer_log.h>
#include <scene_far.h>

namespace engine::renderer {
namespace {

// Names an offered level's derived-data entries apart from every other kind keyed over the same
// source identity.
constexpr u64 k_offered_cache_tag = 0x6f66666572656431ull;

// One offered level as a container: its DAG, its part map as the cluster-to-material map (one
// default material a part, so the map reads back), and the far form it came from, so a warm load
// needs neither the bake nor the level's build. A cache that cannot be written costs the next load
// the build and nothing else.
void write_offered_level(const std::string& ddc, u64 key, u64 source_hash,
                         const geometry::FarForm& form, const geometry::ClusterLodMesh& dag,
                         const Vector<u32>& part_of_cluster) {
  geometry::ClusterFileData data;
  data.mesh = dag;
  data.cluster_material = part_of_cluster;
  u32 parts = 1;
  for (const u32 p : part_of_cluster)
    parts = std::max(parts, p + 1);
  data.materials.resize(parts);
  data.far_form = form;
  data.source_hash = source_hash;
  data.build_key = key;
  const std::string path = geometry::cluster_cache_path(ddc, key);
  std::string error;
  const io::Status made = io::make_directories(io::parent_path(path));
  if (made != io::Status::Ok || !geometry::write_cluster_file(path, data, &error)) {
    ENGINE_LOG_WARN(
        log_renderer, "offered far form not cached", log::field("path", path),
        log::field("error", made != io::Status::Ok ? std::string(io::status_name(made)) : error));
  }
}

// What a resident mesh's bytes are, for its offered levels' key: the load's identity of its source
// (a glTF's file hash), or else the identity its container records of what it was built from (a
// generator's or a tool's container named outright); 0 when neither says, and then nothing is
// cached.
u64 source_identity(const SourceMesh& source) {
  if (source.source_hash != 0) return source.source_hash;
  if (source.container.empty()) return 0;
  u64 hash = 0, build_key = 0;
  std::string why;
  if (!geometry::read_cluster_file_identity(source.container, hash, build_key, &why)) return 0;
  return hash == 0 && build_key == 0 ? 0 : hash_combine(hash, build_key);
}

}  // namespace

bool offer_far_forms(SceneDesc& resolved, Vector<SourceMesh>& sources,
                     Vector<geometry::ClusterLodMesh>& dags, bool paged, SceneData& out,
                     Vector<OfferedForm>& offered, std::string& error) {
  offered.clear();
  if (!resolved.world.enabled || paged || resolved.streamed.empty()) return true;
  const u32 meshes = resolved.meshes.size();
  if (resolved.mesh_info.size() != meshes || dags.size() != meshes || sources.size() != meshes)
    return true;
  const f32 threshold =
      resolved.authored.threshold_px > 0.0f ? resolved.authored.threshold_px : 1.0f;
  Vector<u8> subject(meshes, u8{0});
  for (const StreamedPlacements& entry : resolved.streamed) {
    if (entry.own_forms) continue;
    for (const u32 m : entry.meshes)
      if (m < meshes) subject[m] = 1;
  }
  if (out.form_source.size() < meshes) {
    const u32 had = out.form_source.size();
    out.form_source.resize(meshes);
    for (u32 m = had; m < meshes; ++m)
      out.form_source[m] = m;
  }
  if (out.form_rigid.size() < meshes) out.form_rigid.resize(meshes, u8{0});
  u32 forms = 0, baked = 0, read = 0;
  u64 form_clusters = 0;
  const bool cache = resolved.cache && !resolved.ddc.empty();
  const i64 start_ns = time::monotonic_ns();
  for (u32 m = 0; m < meshes; ++m) {
    if (subject[m] == 0 || resolved.meshes[m].empty() || resolved.mesh_info[m].fit.active())
      continue;
    if (!dags[m].mesh.skin.empty() || !dags[m].mesh.morph_channels.empty() ||
        sources[m].has_motion() || !sources[m].lod_coverage.empty() ||
        dags[m].mesh.clusters.empty()) {
      continue;
    }
    // **Kept in the derived-data cache** (renderer.md, "What path 1 costs a load"): each offered
    // level's DAG and part map, with the far form it came from, under a key over the mesh's source
    // identity, the load's LOD options and the versions of what makes them. A mesh with no source
    // identity (a generator's container named outright) is baked every load.
    const u64 identity = cache ? source_identity(sources[m]) : 0;
    const bool keyed = identity != 0;
    const u64 base =
        keyed ? hash_combine(geometry::cluster_cache_key(identity, resolved.lod, false, 0),
                             hash_combine(hash_combine(geometry::k_far_form_version,
                                                       k_far_tile_build_version),
                                          k_offered_cache_tag))
              : 0;
    geometry::ClusterFileData cached[geometry::k_far_form_levels];
    bool hit[geometry::k_far_form_levels] = {};
    geometry::FarForm form = sources[m].far_form;
    for (u32 l = 0; keyed && l < geometry::k_far_form_levels; ++l) {
      const u64 key = hash_combine(base, l);
      const std::string path = geometry::cluster_cache_path(resolved.ddc, key);
      std::string why;
      hit[l] = io::exists(path) && geometry::read_cluster_file(path, cached[l], &why) &&
               cached[l].build_key == key && !cached[l].far_form.empty();
      if (hit[l] && form.empty()) form = cached[l].far_form;
    }
    if (form.empty()) {
      const Vector<u32>& parts = sources[m].part_of_cluster;
      if (parts.size() != dags[m].mesh.clusters.size()) continue;
      std::string why;
      if (!geometry::bake_far_form(dags[m], std::span<const u32>(parts.data(), parts.size()), form,
                                   &why)) {
        continue;
      }
      ++baked;
    }
    // The rule's own diameter (ring_forms.h, `choose_ring_forms`): the clusters' spheres about the
    // origin, which is the frame a mesh with no fit is placed in.
    f32 most = 0.0f;
    for (const geometry::ClusterDesc& c : dags[m].mesh.clusters)
      most = std::max(most, length(c.center) + c.radius);
    const f32 diameter = 2.0f * most;
    for (u32 l = 0; l < form.levels.size(); ++l) {
      const f32 err = geometry::FarForm::level_error(form.levels[l]);
      if (!(err > 0.0f)) continue;
      // A level that keeps half the mesh or more would lose to the DAG's own cut at any floor it
      // serves (the cut keeps the top of a DAG whose levels halve): not built, so a load pays the
      // far form only for a mesh it can help.
      if (u64{form.levels[l].triangles} * 2 >= form.header.source_triangles) continue;
      geometry::ClusterLodMesh dag;
      Vector<u32> part_of_cluster;
      if (hit[l] && cached[l].cluster_material.size() == cached[l].mesh.mesh.clusters.size()) {
        dag = std::move(cached[l].mesh);
        part_of_cluster = std::move(cached[l].cluster_material);
        ++read;
      } else {
        if (!far_form_level_mesh(form, l, resolved.lod, dag, part_of_cluster, error)) {
          error = resolved.mesh_info[m].name + ": its far form: " + error;
          return false;
        }
        if (keyed)
          write_offered_level(resolved.ddc, hash_combine(base, l), identity, form, dag,
                              part_of_cluster);
      }
      const u32 pairs = dag.mesh.clusters.size();
      SourceMesh copy = sources[m];
      copy.part_of_cluster = std::move(part_of_cluster);
      copy.lod_coverage.clear();
      copy.motion_vertices.clear();
      copy.motion_parts.clear();
      copy.far_form = geometry::FarForm{};
      const u32 drawn = resolved.meshes.size();
      std::string path = resolved.meshes[m];  // a copy: never push a Vector's own element
      resolved.meshes.push_back(std::move(path));
      SceneMeshInfo info = resolved.mesh_info[m];
      info.name += " (far form " + std::to_string(l) + ")";
      info.origin = "form";
      resolved.mesh_info.push_back(std::move(info));
      sources.push_back(std::move(copy));
      dags.push_back(std::move(dag));
      out.form_source.push_back(m);
      out.form_rigid.push_back(1);
      OfferedForm offer;
      offer.source = m;
      offer.mesh = drawn;
      offer.pairs = pairs;
      offer.max_px = diameter / err * threshold;
      offered.push_back(offer);
      ++forms;
      form_clusters += pairs;
    }
  }
  // What path 1 cost this load (renderer.md, "What path 1 costs a load"): the bakes and each
  // offered level's DAG, on the load's thread.
  ENGINE_LOG_INFO(log_renderer, "far forms offered", log::field("forms", forms),
                  log::field("baked", baked), log::field("from_cache", read),
                  log::field("clusters", form_clusters),
                  log::field("ms", static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6));
  return true;
}

}  // namespace engine::renderer
