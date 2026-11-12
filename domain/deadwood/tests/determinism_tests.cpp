#include <domain/deadwood/deadwood.h>
#include <domain/geometry/cluster_file.h>

#include <doctest/doctest.h>

using namespace engine;

TEST_CASE("deadwood: complete container bytes agree across toolchains") {
  // G101 (dc2581d5), v1: 237cacf9e023dabf / 76a3cc6c196c3032.
  // G104, v2: centred cut UVs, coherent bark contours, three exposed roles and embedded
  // colour/normal/roughness maps. Living flora pins stay unchanged.
  // G124: oak source taper and shape identity change; spruce remains byte-identical.
  // G133: 1363173e6482228d was an earlier G129 prototype's oak; the final taper and the leader fix
  // (k_branch_taper_version 2 in the source key) move the oak only.
  // G201: the source tree's skirt moves both: the oak log (cut from 0.3 m, above it) by
  // k_flora_version 12 in its source key alone, the fallen spruce (cut from 0) by that and the band
  // of skirt between the origin and its collar ring; they were 0x89525862dfd8a785 and
  // 0xe1109ed169374c96.
  const u64 goldens[] = {0x56c21256a950bc9bull, 0x6ca6cd692992eaeeull};
  for (u32 i = 0; i < 2; ++i) {
    deadwood::DeadwoodFile p;
    p.tree.species = i == 0 ? "oak" : "spruce";
    p.tree.tree.height_m = 6;
    p.tree.tree.seed = 17;
    p.tree.tree.triangle_budget = 60000;
    p.seed = 17;
    p.kind = i == 0 ? deadwood::RemainKind::log : deadwood::RemainKind::fallen;
    if (i == 0) {
      p.start_m = .3f;
      p.end_m = 3;
      p.hollow = .7f;
      p.collapse = .65f;
      p.bark = .1f;
    } else {
      p.roll_deg = 37;
    }
    geometry::ClusterFileData data;
    deadwood::Report report;
    std::string error;
    CAPTURE(i);
    REQUIRE_MESSAGE(deadwood::build_container(p, {}, {}, 65536, data, report, error), error);
    CHECK(geometry::cluster_file_hash(data) == goldens[i]);
  }
}
