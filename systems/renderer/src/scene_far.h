#pragma once

// The far form's seams inside the renderer (G198; far_tile.h; renderer.md, "The far tile"):
// far_tile.cpp's, for the load.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/far_form.h>
#include <systems/renderer/ring_forms.h>
#include <systems/renderer/scene.h>

#include <string>

namespace engine::renderer {

// One level of a far form as a DAG of its own, one part a material it draws, in the order its
// parts name them; `part_of_cluster` is each cluster's material as the far form numbers it (the
// source's part).
bool far_form_level_mesh(const geometry::FarForm& form, u32 level,
                         const geometry::ClusterLodOptions& lod, geometry::ClusterLodMesh& out,
                         Vector<u32>& part_of_cluster, std::string& error);

// **A resident mesh's far form, offered to the rings' rule** (ring_forms.h, `OfferedForm`): every
// rigid, unfitted resident mesh of a streamed entry that does not pick its own forms gets each
// level of its far form (carried, or baked from its finest level) as a scene mesh of its own after
// every other — its source's materials and textures, its own DAG — offered with the projected
// diameter it serves: a level of honest error `err` serves a source of diameter `d` (the rule's own
// measure, the clusters' spheres about the origin) while that diameter projects under
// `d / err * threshold_px`. `offered` is replaced.
bool offer_far_forms(SceneDesc& resolved, Vector<SourceMesh>& sources,
                     Vector<geometry::ClusterLodMesh>& dags, bool paged, SceneData& out,
                     Vector<OfferedForm>& offered, std::string& error);

}  // namespace engine::renderer
