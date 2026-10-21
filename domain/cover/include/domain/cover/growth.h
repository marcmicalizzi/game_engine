#pragma once

// The cover solver (docs/subsystems/cover.md, "The cover"; roadmap G78): moss, crustose and
// fruticose lichen, algae's green film and fungi, solved over a host's surface from the exposure
// terms (host.h) by the ecology's rules (docs/experiments/vine-cover.md).
//
// **(a) The mask**: per host vertex, a coverage in [0, 1] for each kind. Each holding triangle's
// suitability for each kind is a product of its terms — moss wants damp, shade, an upward or
// north face, a crevice, the base, a rough substrate; crustose lichen wants light and is beaten by
// moss where it is damp; algae want damp shade on any substrate; fungi want dead wood, damp and
// shade — scaled by how far the kind has come in the surface's age (1 - exp(-age / tau)), moss
// taking its share first and lichen and algae what it leaves; then carried to the welded vertices
// by area, smoothed along the surface and sharpened. Written per **host** vertex into the
// companion's container (section 40), never into the host.
//
// **(b) The shaped cover**: moss cushions, fruticose lichen tufts, bracket fungi and mushrooms
// where the mask is dense, each a copy of one of a few kit shapes (flora/companion.h) on the
// surface and turned to it, their count per triangle its area times the kind's density times the
// mask there, drawn with a seeded fraction so the total follows the mask.
//
// **(c) The stand-in film** for the smooth cover, until the blended cover layer (roadmap R90)
// draws it: every holding triangle whose strongest smooth kind (moss, lichen, algae) averages
// `film_threshold` or more over its corners is drawn again a few millimetres off the surface in
// that kind's material — a coarse, per-triangle picture of the mask that the renderer draws today,
// since no material reads a vertex colour (assets.md, "Not yet").
//
// The cover is a **companion** like the vine: part 0 a root with no geometry, everything else on
// part 1 with weight 0 and no structural flags, so it neither sways nor collides.

#include <core/base/types.h>
#include <domain/assets/gltf.h>
#include <domain/cover/host.h>
#include <domain/flora/motion.h>

#include <schemas/cover.h>
#include <string>

namespace engine::cover {

enum CoverKind : u32 { k_moss = 0, k_lichen = 1, k_algae = 2, k_fungus = 3, k_cover_kinds = 4 };

// The years each kind takes to come to 1 - 1/e of its cover (vine-cover.md, "The constants").
inline constexpr f64 k_tau_years[k_cover_kinds] = {8.0, 40.0, 2.0, 3.0};

struct CoverReport {
  f64 mean[k_cover_kinds] = {0.0, 0.0, 0.0, 0.0};  // area-weighted over the holding surface
  f64 holding_area_m2 = 0.0;
  u32 film_triangles = 0;
  u32 cushions = 0;
  u32 tufts = 0;
  u32 brackets = 0;
  u32 mushrooms = 0;
  u32 triangles = 0;
  f32 film_threshold = 0.0f;  // the threshold the budget left
  f64 density_scale = 1.0;    // and the share of the shaped pieces
};

bool cover_in_range(const CoverGrowth& params, std::string& error);

// The suitability of one triangle for each kind, before age and competition (cover.md's table).
struct Suitability {
  f64 kind[k_cover_kinds] = {0.0, 0.0, 0.0, 0.0};
  f64 damp = 0.0;
};
Suitability cover_suitability(const Exposure& e, f64 roughness, const HostSite& site,
                              const CoverGrowth& params);

// The mask: `k_cover_kinds` floats per welded vertex (`surface.welded`), in [0, 1].
void solve_cover_mask(const HostSurface& surface, const Vector<Exposure>& exposure,
                      const HostSite& site, const CoverGrowth& params, Vector<f32>& out);

// The mask per host vertex, four unorm8 a word (moss, lichen, algae, fungus from the low byte):
// section 40's layout.
void pack_cover_mask(const HostSurface& surface, const Vector<f32>& welded, Vector<u32>& out);

// The whole cover: the mask, the film and the shaped pieces. `out` and `motion` are replaced;
// `mask`, when given, receives the packed per-host-vertex mask.
bool solve_cover(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
                 const CoverGrowth& params, assets::MeshData& out, flora::Motion& motion,
                 Vector<u32>* mask, CoverReport* report, std::string* error);

}  // namespace engine::cover
