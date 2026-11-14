// A resident mesh's far form, offered to the rings' rule (G198; far_tile.h; ring_forms.h,
// `OfferedForm`; renderer.md, "The far tile").
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/far_form.h>

#include <algorithm>
#include <renderer_log.h>
#include <scene_far.h>

namespace engine::renderer {

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
  u32 forms = 0;
  u64 form_clusters = 0;
  for (u32 m = 0; m < meshes; ++m) {
    if (subject[m] == 0 || resolved.meshes[m].empty() || resolved.mesh_info[m].fit.active())
      continue;
    if (!dags[m].mesh.skin.empty() || !dags[m].mesh.morph_channels.empty() ||
        sources[m].has_motion() || !sources[m].lod_coverage.empty() ||
        dags[m].mesh.clusters.empty()) {
      continue;
    }
    // The form the container carried (its parts are its materials, as `adopt_container` numbers
    // them), or the same bake over the mesh's parts.
    geometry::FarForm form = sources[m].far_form;
    if (form.empty()) {
      const Vector<u32>& parts = sources[m].part_of_cluster;
      if (parts.size() != dags[m].mesh.clusters.size()) continue;
      std::string why;
      if (!geometry::bake_far_form(dags[m], std::span<const u32>(parts.data(), parts.size()), form,
                                   &why)) {
        continue;
      }
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
      if (!far_form_level_mesh(form, l, resolved.lod, dag, part_of_cluster, error)) {
        error = resolved.mesh_info[m].name + ": its far form: " + error;
        return false;
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
  if (forms > 0) {
    ENGINE_LOG_INFO(log_renderer, "far forms offered", log::field("forms", forms),
                    log::field("clusters", form_clusters));
  }
  return true;
}

}  // namespace engine::renderer
