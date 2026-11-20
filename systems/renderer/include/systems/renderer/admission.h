#pragma once

// **Content classes decided when a scene is loaded** (roadmap C64; docs/content-classes.md, "What
// decided at instantiation means"; renderer.md, "Content classes at load"; ADR-0073 decision 5).
//
// An instance reaches a loaded scene only through `admit_instance`, the one door: it joins the
// class of everything the instance draws with (its mesh's own, its textures', its raise), decides
// it through the scene's decision table under the policy the scene was loaded with, and records
// what it decided. `omit` makes nothing of the instance; `proxy` makes the engine's box at its
// mesh's bounds, with one neutral material. Nothing here touches the GPU: no struct, shader or
// per-frame work changes, so a frame with nothing classified costs what it cost.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_lod.h>
#include <foundation/content_class/content_class.h>

#include <string>

namespace engine::renderer {

struct SceneData;

// The policy a load runs under when its caller names none: `agent-default`, compiled in
// (ADR-0033 decision 7, ADR-0073 decision 4). What `SceneDesc::view_policy` and
// `PlacementMeshOptions::view_policy` start as.
content_class::NamedPolicy agent_default_view_policy();

// One category's substitutions at a load, by the category's name: the instances it omitted and
// the ones it put a box in place of. Only a category that decided a substitution has a row.
struct SubstitutedCount {
  std::string category;
  u32 omitted = 0;
  u32 proxied = 0;
};

// A class as the load read it, and whether it could be read at all. A class that could not be read
// is `content_class::unreadable` (every known category at its top), so it fails closed whatever a
// caller does with the flag; the flag is what the report counts apart.
struct ReadClass {
  content_class::ContentClasses classes;
  bool unreadable = false;
};

// The join of two reads: the per-category maximum, unreadable when either is.
ReadClass join(const ReadClass& a, const ReadClass& b);

// **The one door into a loaded scene.** Decides an instance on the join of its mesh's own class,
// its textures' and its raise, through `scene.decisions` (so the table records the class the first
// time it is met), and accounts for it: an admitted class joins `scene.held`; a substitution is
// counted in `scene.load.substituted` under the category that decided it, or apart
// (`unreadable_omitted`, `unreadable_proxied`) when any of the three could not be read. The load
// calls it once for every instance it makes; whatever admits an instance after the load (a world
// tile's placement, a placement's form, a game module's object; C67) calls it the same way before
// it makes one, with the class its mesh's container or sidecar gives (`read_container_class`,
// `read_sidecar_class`). An all-`none` join is one branch and no allocation.
content_class::Decision admit_instance(SceneData& scene, const ReadClass& mesh,
                                       const ReadClass& textures, const ReadClass& raise);

// **The door's form for the scene's ground** (the terrain; C64): decided as an instance is,
// through `scene.decisions`, on its class, the join of what the sidecars beside its heightfield
// source and every layer's images declare. Anything but `admit` leaves the ground out whole, since
// a box at a ground's bounds stands for nothing (the outcome returned is then `omit`), and is
// counted in `scene.load.grounds_omitted`, apart from the instances. An all-`none` ground is one
// branch.
content_class::Decision admit_ground(SceneData& scene, const ReadClass& ground);

// What a `.clusters` container says it is: its section 54, looked up in the section table and read
// only when present (a few dozen bytes), its ladders learned into `categories`; joined with a
// sidecar beside it (`<stem>.provenance.json`) when there is one. A container that cannot be
// opened reads as `none` here, because its load fails on its own with a sentence of its own.
ReadClass read_container_class(const std::string& path, content_class::Categories& categories);

// What a provenance sidecar beside `source` declares (`content_class::sidecar_path_of`), read as
// the content build reads it (`content_build::read_source_classes`, through the module's one
// reader): `none` only when nothing of that name is there (one look at the name, a link not
// followed), and the unreadable class for anything there that cannot be read, a link or junction
// that leads nowhere included. `materials`, when given, receives each material's own class.
ReadClass read_sidecar_class(const std::string& source, const content_class::Categories& categories,
                             Vector<content_class::MaterialClass>* materials = nullptr);

// **The proxy** (ADR-0033's `proxy`): the engine's own box, its corners at `lo` and `hi` in the
// mesh's space, so an instance's transform and its mesh's fit place it where the mesh stood. Six
// faces of two triangles each, counter-clockwise seen from outside, a vertex per face corner so
// each face has its own normal; built by the same LOD builder as every mesh, one cluster. Geometry
// any asset has, since every mesh has bounds: nothing about it depends on what the mesh was. An
// extent under a millimetre is widened to one, so a flat mesh's box is not flat. False, with
// `error`, only when the builder refuses.
bool build_proxy_box(const Vec3& lo, const Vec3& hi, const geometry::ClusterLodOptions& options,
                     geometry::ClusterLodMesh& out, std::string* error = nullptr);

// The proxy's one material: a mid grey (`k_proxy_albedo`), rough, not metallic, sampling nothing.
inline constexpr f32 k_proxy_albedo = 0.5f;
assets::Material proxy_material();

// The box round a mesh's finest clusters' spheres, in its own space: where its proxy stands. A
// mesh with no clusters gives the unit box about the origin.
void leaf_bounds(const geometry::ClusterLodMesh& lod, Vec3& lo, Vec3& hi);

// The load's own mark of an instance its decision omitted, in `SceneInstance::content_classes`
// between the decision and the instance table, which leaves it out. Never a file's: a scene file's
// raises are numbered from 1 and refused past `k_max_raises`.
inline constexpr u16 k_instance_omitted = 0xFFFFu;
inline constexpr u32 k_max_raises = 0xFFFEu;

}  // namespace engine::renderer
