#pragma once

// The load's two admission passes (C64; renderer.md, "Content classes at load"): what `load_scene`
// calls before its meshes are read and after, so the door (`admit_instance`, admission.h) is the
// one place an instance is decided and the load's own body only asks it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <foundation/content_class/content_class.h>
#include <systems/renderer/admission.h>

#include <string>

namespace engine::renderer {

struct SceneDesc;
struct SceneData;

// What the passes carry between them, for one load. Cold, and sized by meshes and raises, never
// kept past the load.
struct LoadAdmission {
  Vector<ReadClass> mesh;     // per mesh: its own class (section 54, a sidecar, a pack's sets)
  Vector<u64> mesh_hash;      // per mesh: a glTF's class hash, which its cache key holds; 0 none
  Vector<Vector<u8>> record;  // per mesh: a glTF's section 54, written if the load builds it
  Vector<ReadClass> raises;   // per `SceneDesc::raises`
  Vector<u8> load;            // per mesh: 1 when its geometry is read (something may draw it)
  // Per mesh and image: the class hash a texture key holds (empty inner: none of its images has
  // one). What `prepare_scene_textures` moves each classified image's key by.
  Vector<Vector<u64>> image_hash;
  // The outcome for the instances the load makes itself after the merge (the implicit one, the
  // grid's), all of mesh 0 with no raise, and which mesh a proxied instance of each mesh draws.
  content_class::Outcome mesh0 = content_class::Outcome::admit;
  Vector<u32> proxy_of;
  bool classified = false;  // anything at all above `none`: false and the second pass is free
  // Per mesh: 1 for the scene's ground (an empty path with the terrain on), whose class is its
  // heightfield source's and its layers' images' sidecars'. `ground_refused`: the policy refused
  // it, so it is not built, and the load leaves the ground out whole (`load_scene`).
  Vector<u8> ground;
  bool ground_refused = false;
  // Per mesh, for a container named outright: what its probe saw, and the classes section its
  // whole read found (`loaded`, with `read_whole` 1 once the mesh job read it), which the second
  // pass decides it by when the two differ (`container_class_as_loaded`; C64).
  // Per mesh: 1 when something other than the file's instances places it after the load (C64).
  Vector<u8> placed;
  Vector<ContainerProbe> probe;
  Vector<Vector<u8>> loaded;
  Vector<u8> read_whole;
};

// Before any mesh is read: the scene's policy, every mesh's own class (its ladders learned into
// `out.categories`; the ground's from the sidecars beside its heightfield source and its layers'
// images, before any of them is read), the policy resolved and `out.decisions` made under it, the
// raises read, and which meshes are read at all: one an instance would draw under the policy
// (admitted or proxied), or one no instance names (a prop, a generator's resident mesh) whose own
// class admits; the ground only when the policy admits it. False, with `error`, for an instance
// naming a raise the scene has not.
bool admission_before_meshes(const SceneDesc& desc, SceneData& out, LoadAdmission& state,
                             std::string& error);

// After the meshes are read and before their textures: every image named by path probed for its
// sidecar, then every instance decided through the door on the join of its mesh's class, its
// textures' and its raise. A mesh no admitted instance draws has its geometry replaced by its
// proxy box and its source emptied (no image, no container, no motion), so nothing of it reaches
// the scene; a mesh drawn by some and proxied for others gets a proxy mesh of its own after the
// scene's meshes; a proxied instance is pointed at its proxy and an omitted one marked
// `k_instance_omitted`, and in a streamed world both are kept from the tail. Free when nothing
// is classified.
void admission_after_meshes(SceneDesc& desc, SceneData& out, LoadAdmission& state,
                            Vector<geometry::ClusterLodMesh>& dags,
                            Vector<geometry::ClusterPages>& tables);

}  // namespace engine::renderer
