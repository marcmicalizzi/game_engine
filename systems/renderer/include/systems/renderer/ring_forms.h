#pragma once

// **A placement's form by ring** (G197; renderer.md, "A placement's form by ring"; world.md, "A
// placement's form by ring"). A streamed world places a generator's resident mesh in every ring it
// holds; past the first ring the camera is never nearer than the ring's floor to a placement on one
// of its tiles, so the mesh need not be placed as itself there. This is the table a load makes of
// what each such mesh is drawn as in each ring, and the one read the world's placements consumer
// makes of it per placement.
//
// **The table keys on the mesh, never on who placed it.** Every input is what any mesh has or can
// be measured for: its DAG (a form cut from its top for the ring's floor, `extract_lod_form`, which
// selects exactly what the mesh selects from that far), its size (its projected diameter at the
// ring's floor at the host's projection, below which it is drawn as nothing), and a form it
// **offers** — a far form its container carries (G198's, or an author's), drawn where its projected
// diameter is at most the size the form says it serves. An imported mesh scattered by a scene file
// gets the same three; a mesh with none of the third gets the first two.
//
// **What a ring draws is the cheapest form that serves it**, in pairs: the ring's cut, or an
// offered form whose size covers the ring's floor. A placement scaled past what a ring's form
// serves draws the ring before's form, down to the mesh itself, so a large placement is never drawn
// coarser than its size allows; one scaled small enough that it is under `min_px` at the ring's
// floor draws nothing there, its footprint still handed to the entries after it.
//
// **The read is pure**: `pick_ring_form` reads a table the load filled and changes nothing, so the
// consumer may ask it from any thread and gets the same answer on every one.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>

namespace engine::renderer {

// The rings a table holds a form for: the world's `k_max_rings`, mirrored (the renderer may not
// depend on the world; its placement tests hold the two equal).
inline constexpr u32 k_ring_forms_max = 7;
// What `pick_ring_form` answers for a placement drawn as nothing in its ring. Not `~0u`, which a
// query's tile hands as the mesh of a placement whose tile mesh it never builds: that one is
// answered as itself, like any mesh past the table.
inline constexpr u32 k_no_ring_form = 0xFFFFFFFEu;

// Which form a ring's entry is, for a summary and the tests.
enum class RingFormKind : u8 {
  mesh =
      0,  // the mesh itself (ring 0, and a ring whose cut keeps every cluster the ring before did)
  cut = 1,      // a distance form cut from the top of the mesh's DAG for the ring's floor
  offered = 2,  // a form the mesh offers (`OfferedForm`): a far form its container carries
};

// One ring's entry of one mesh. 16 bytes; a mesh's record is kept per mesh, not per placement, so
// it is not a hot type (the renderer's size table is an exemption,
// tools/size-table-exemptions.txt).
struct RingForm {
  u32 mesh = k_no_ring_form;  // the scene mesh the ring draws a placement of the mesh as
  // The largest placement scale (the largest axis's) this entry serves; a placement scaled more
  // draws the ring before's entry. The mesh itself serves every scale.
  f32 max_scale = 0.0f;
  // A placement scaled less than this is under `RingFormRules::min_px` at the ring's floor and
  // draws nothing in the ring. 0: every scale draws.
  f32 none_below_scale = 0.0f;
  RingFormKind kind = RingFormKind::mesh;
  u8 pad[3] = {};
};
static_assert(sizeof(RingForm) == 16, "RingForm is 16 bytes");

// Every ring's entry of one mesh. `active` false: the mesh draws itself in every ring (it is not a
// streamed placement's resident mesh, it deforms, or the rule is off).
struct MeshRingForms {
  RingForm ring[k_ring_forms_max] = {};
  bool active = false;
  // The first ring whose entry stands still for a mesh that sways (G204; sway_forms.h): from it
  // outward the forms carry no motion contract, and each serves the placement scales that stand
  // still seen from its ring's floor. 0: the mesh does not sway, or sways in every ring.
  u8 still_from = 0;
  u8 pad[2] = {};
};

// The load's table (`SceneData::ring_forms`): one record per scene mesh the load made, or empty.
// A mesh past the end (a tile's mesh, which arrives after the load) draws itself.
struct RingFormTable {
  u32 rings = 0;                       // the world's rings the entries are for; 0: no table
  f32 floor_m[k_ring_forms_max] = {};  // each ring's floor: the nearest a camera is to a placement
  Vector<MeshRingForms> meshes;        // by scene mesh
  // What the load made, for a summary: the meshes with a table, the cut forms made as meshes of
  // their own and their clusters, and the offered forms a ring chose.
  u32 subjects = 0;
  u32 cuts = 0;
  u64 cut_clusters = 0;
  u32 offered = 0;
  // ... and of them (G204; sway_forms.h): the subjects that sway, and the cut forms that stand
  // still (the rest of a swaying subject's cuts sway as it does).
  u32 swaying = 0;
  u32 stills = 0;
};

// The scene mesh a placement of `mesh` at `scale` (its rotation's axes scaled: the largest of the
// three counts) is drawn as on a tile of ring `ring`, or `k_no_ring_form` for nothing. A pure read:
// no allocation, no state, safe from any thread. Ring 0, a mesh past the table, an inactive mesh
// and an empty table answer `mesh`.
u32 pick_ring_form(const RingFormTable& table, u32 mesh, u32 ring, Vec3 scale) noexcept;

// Whether two rings draw every mesh of `meshes` alike, for every scale: a tile moving between them
// need not be built again for its forms (the world's `PlacementTiles::redraws`).
bool same_ring_forms(const RingFormTable& table, std::span<const u32> meshes, u32 a,
                     u32 b) noexcept;

// ---- the rule, at the load ----------------------------------------------------------------------

// A form a mesh offers for distance (G198's far form carried in its container, or any): the scene
// mesh it is, the scene mesh it stands in for, its pairs a placement, and the largest projected
// diameter of its source, in pixels at the host's projection, it serves. Whoever reads the form
// appends its mesh to the scene and hands this to the load's rule; a mesh that offers none is
// drawn by its cuts.
struct OfferedForm {
  u32 source = 0;
  u32 mesh = 0;
  u32 pairs = 0;
  f32 max_px = 0.0f;
};

// The rule's knobs: the host's projection (`LodView::proj_scale`, the forms' own), the scale a
// ring's cut serves, and the projected diameter below which a placement is drawn as nothing.
// Tunables `renderer.forms.enabled`, `renderer.forms.cut_scale` and `renderer.forms.min_px`
// (`ring_form_rules`).
struct RingFormRules {
  bool enabled = true;
  f32 proj_scale = 0.0f;
  f32 cut_scale = 1.5f;
  f32 min_px = 1.0f;
};
// The rules from the tunables, with the host's projection.
RingFormRules ring_form_rules(f32 proj_scale) noexcept;

// One ring's cut of a mesh, as the load made it: the scene mesh it is (the mesh itself, or the ring
// before's, when the cut kept every cluster that one did) and its clusters.
struct RingCut {
  u32 mesh = k_no_ring_form;
  u32 pairs = 0;
};

// The rule for one mesh: what each ring draws it as. `diameter_m` is the mesh's bounding diameter
// at scale 1, metres, fit applied; `cuts[r]` the ring's cut (ring 0's is the mesh); `floor_m[r]`
// the ring's floor; `offered` the forms the mesh offers. A pure function of its arguments.
MeshRingForms choose_ring_forms(u32 mesh, u32 pairs, f32 diameter_m, std::span<const f32> floor_m,
                                std::span<const RingCut> cuts, std::span<const OfferedForm> offered,
                                const RingFormRules& rules) noexcept;

}  // namespace engine::renderer
