#include <domain/deadwood/deadwood.h>
#include <domain/geometry/cluster_file.h>

#include <doctest/doctest.h>

using namespace engine;

TEST_CASE("deadwood: complete container bytes agree across toolchains") {
  // G101 (dc2581d5), v1: 237cacf9e023dabf / 76a3cc6c196c3032.
  // G104, v2: centred cut UVs, coherent bark contours, three exposed roles and embedded
  // colour/normal/roughness maps. Living flora pins stay unchanged.
  const u64 goldens[] = {0xd2e2c9f84d2b4f59ull, 0xe1109ed169374c96ull};
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
