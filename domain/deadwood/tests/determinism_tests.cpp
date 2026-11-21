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
  // 0xe1109ed169374c96. G201 stage 2: `k_deadwood_version` 3 (a standing remain keeps its skirt)
  // moves both by the source key alone, neither being a standing remain; they were
  // 0x56c21256a950bc9b and 0x6ca6cd692992eaee. G203's third leg (2026-11-15, MSVC only so far):
  // a dead conifer's key takes the conifers' card version (`flora::flora_needle_card_version`)
  // and the needle cards it keeps are folded, so the fallen spruce moved (it was
  // 0x2d9e4afaa3af1347; the key and the cards were not told apart) and the oak log did not.
  // R164 (MSVC first): `k_flora_version` 13 in the source key moves both by that alone (with the
  // optics zeroed and the version 12 both read 0x60060207b3a4241d and 0x05ba38fe44097faa again).
  // R164-3 (MSVC first): the optics folded into every flora key, the tree's key the remain's
  // source key carries, move both by that alone; they were 0x2bc69b39495bdb36 and
  // 0xa11d29e8e8356046. R202's second step: the bakes' convention left the tree's shape key the
  // remain's source key carries (`k_deadwood_version` 4 had already moved both); they were
  // 0xe2e1fe4a15ec05b8 and 0x6befabef27a83dd0.
  const u64 goldens[] = {0x7db4189afc5a1164ull, 0x69a1f4ddfbc0a24eull};
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
