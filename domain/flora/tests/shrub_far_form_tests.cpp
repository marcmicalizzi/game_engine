// The shrub's far form (G123; flora.md, "The shrub";
// docs/experiments/shrub-far-form-2026-11-03.md): where a shrub's whole-shrub star replaces its
// near mesh, and how much of the shrub's silhouette the star draws. The fixture is the forest proof
// of concept's `understory-v22-dogwood-2` (red-osier dogwood, seed 220164, standard detail), whose
// star the owner saw doubled at 10 to 12.5 m.

#include <core/math/math.h>
#include <domain/flora/flora.h>
#include <domain/flora/impostor.h>
#include <domain/flora/shrubs.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>

#include <cmath>
#include <span>
#include <string>

using namespace engine;

namespace {

flora::FloraFile dogwood() {
  flora::FloraFile file;
  std::string error;
  const bool ok = flora::read_flora_file(
      R"({"species":"red_osier_dogwood","shrub_growth":{"seed":220164,"detail_tier":"standard"}})",
      file, error);
  REQUIRE_MESSAGE(ok, error);
  return file;
}

// The basal stems' mean diameter, from the motion rows the solver writes (a stem is a part whose
// parent is the root).
f64 mean_stem_m(const flora::Motion& motion) {
  f64 sum = 0.0;
  u32 stems = 0;
  for (u32 i = 1; i < motion.parts.size(); ++i)
    if (motion.parts[i].parent == 0) {
      sum += 2.0 * static_cast<f64>(motion.structure[i].base_radius);
      ++stems;
    }
  return stems == 0 ? 0.0 : sum / stems;
}

}  // namespace

TEST_CASE("shrub far form: the dogwood's star waits until its stems are sub-pixel") {
  const flora::FloraFile file = dogwood();
  geometry::ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(flora::build_flora_container(file, {}, 0, data, nullptr, error), error);
  REQUIRE(!data.impostors.empty());
  const geometry::ClusterFileImpostor& star = data.impostors.back();
  const geometry::ClusterLodMesh& lod = data.mesh;
  const std::span<const u32> coverage(data.lod_coverage.data(), data.lod_coverage.size());
  const Vec4 sphere = geometry::authored_lod_sphere(lod, star.error);
  REQUIRE(sphere.w > 0.0f);
  const Vec3 centre{sphere.x, sphere.y, sphere.z};

  assets::MeshData mesh;
  flora::Motion motion;
  REQUIRE(flora::solve_shrub_growth(file.species, file.shrub_growth, mesh, motion, nullptr, error));
  const f64 stem_m = mean_stem_m(motion);
  MESSAGE("star: error " << star.error << " m, coverage " << star.coverage << " m (difference "
                         << star.difference << "), sphere radius " << sphere.w
                         << " m, mean basal stem " << stem_m << " m");
  // The coverage error is the stems' floor: at the default foliage threshold it projects to it
  // exactly where a mean stem projects to one pixel.
  CHECK(static_cast<f64>(star.coverage) >= stem_m * geometry::k_default_foliage_px * 0.999);

  // The owner's view: 55 degrees of vertical field at 1080 and 2160 rows, the default thresholds,
  // the camera on the owner's bearing (yaw 232.7 degrees in the scene; the shrub stands at its own
  // yaw there, which turns the bearing and not the distance).
  for (const f32 rows : {1080.0f, 2160.0f}) {
    geometry::LodView view;
    view.znear = 0.05f;
    view.proj_scale = 1.0f / std::tan(radians(55.0f) * 0.5f) * rows * 0.5f;
    view.threshold_px = 1.0f;
    view.foliage_px = geometry::k_default_foliage_px;
    const auto star_drawn = [&](f32 distance) {
      const f32 yaw = radians(232.7f);
      view.camera = centre + Vec3{std::cos(yaw) * distance, 0.0f, std::sin(yaw) * distance};
      Vector<u32> cut;
      geometry::select_lod(lod, view, coverage, cut);
      u32 star_clusters = 0;
      for (const u32 c : cut)
        star_clusters += data.cluster_material[c] == star.material ? 1u : 0u;
      return star_clusters > 0u;
    };
    // Where the owner saw it doubled: the near mesh draws, not the star.
    for (const f32 at : {10.0f, 12.5f}) {
      CAPTURE(rows);
      CAPTURE(at);
      CHECK_FALSE(star_drawn(at));
    }
    // Where it switches: by the coverage floor, from the sphere's edge, where a mean stem is a
    // pixel.
    const f32 switch_m = star.coverage * view.proj_scale / view.foliage_px + sphere.w;
    const f32 stem_px_at = static_cast<f32>(stem_m) * view.proj_scale / (switch_m - sphere.w);
    MESSAGE(rows << " rows: the star switches in at " << switch_m
                 << " m from the shrub's centre, a "
                 << "mean stem " << stem_px_at << " px there");
    CHECK(stem_px_at <= 1.001f);
    CHECK_FALSE(star_drawn(switch_m * 0.99f));
    CHECK(star_drawn(switch_m * 1.01f));
  }
}

TEST_CASE("shrub far form: every shrub's star draws its silhouette, no more and not much less") {
  // Before G123 a shrub's star was baked with the plain mask, and its four directions each carry
  // the whole shrub: over the coverage bake's ring it drew 1.35 (the dogwood) to 2.06 (the
  // sagebrush) times the geometry's silhouette — the dense far form the owner saw.
  for (const flora::ShrubProfile& profile : flora::shrub_profiles()) {
    const std::string name(geometry::flora_species_info(profile.species).name);
    CAPTURE(name);
    flora::FloraFile file = dogwood();
    file.species = name;
    if (profile.species != geometry::FloraSpecies::red_osier_dogwood)
      file.shrub_growth.detail_tier = {};
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeLevels levels;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_shrub_growth(file.species, file.shrub_growth, mesh, motion,
                                              nullptr, error, &levels),
                    error);
    REQUIRE(!levels.levels.empty());
    flora::TreeLevel star = levels.levels.back();
    CHECK(star.coverage_dither);
    CHECK(star.coverage_floor_m > 0.0f);
    flora::BakeLook look;
    look.bark = mesh.materials[0].base_color;
    look.leaf = mesh.materials[1].base_color;
    flora::BakedAtlas atlas;
    flora::LevelMeasure m;
    const f32 d = flora::bake_matched(mesh, levels, star, look, atlas, &m);
    MESSAGE(name << ": the star draws " << m.kept << " of the geometry's coverage at scale "
                 << star.coverage_scale << "/256 (D " << d << ")");
    CHECK(m.kept < 1.1);
    CHECK(m.kept > 0.85);
  }
}
