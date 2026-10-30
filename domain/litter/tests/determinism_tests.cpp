#include <domain/litter/litter.h>

#include <doctest/doctest.h>

#include <cstdio>
using namespace engine;
TEST_CASE("litter: complete containers agree across toolchains") {
  // G103 initial pins, litter v1. A replacement records its reason and commit here.
  // Twig and tangle (4, 5) were 5040b12877ff4316 / 9c20f5d4d6a409e3 until G104's deadwood v2
  // (3b4abda4: centred cut UVs, coherent bark contours, k_deadwood_version 2 in the wood kinds'
  // source identity) changed the wood geometry litter reuses; the four deadwood-free kinds did not
  // move. The new values are the same on MSVC debug and release and on Linux Clang debug and GCC
  // release (not a build-configuration difference); with domain/deadwood put back at 31607f4f the
  // old two return.
  constexpr u64 expected[] = {0x341e406800665f85ull, 0xabfbd865a0676d1dull, 0xdefb0dd9440f730aull,
                              0xf94b5a4d69cd7d2cull, 0xd18a644ef32ae659ull, 0xe945763ffe1039d8ull};
  for (u32 k = 0; k < 6; ++k) {
    litter::Recipe p;
    p.kind = static_cast<litter::Kind>(k);
    p.seed = 17;
    geometry::ClusterFileData out;
    litter::Report report;
    std::string error;
    CAPTURE(k);
    REQUIRE_MESSAGE(litter::build_container(p, {}, {}, 65536, out, report, error), error);
    const u64 actual = geometry::cluster_file_hash(out);
    std::printf("litter %u: hash=0x%016llx triangles=%u clusters=%u vertices=%u\n", k,
                static_cast<unsigned long long>(actual), report.triangles,
                out.mesh.mesh.clusters.size(), out.mesh.mesh.vertices.size());
    CHECK(actual == expected[k]);
    const auto* generator = assets::MeshGeneratorRegistry::global().find("litter");
    REQUIRE(generator != nullptr);
    const auto json = litter::canonical_json(p);
    geometry::ClusterFileData registered;
    REQUIRE(generator->build({reinterpret_cast<const u8*>(json.data()), json.size()}, {}, {}, 65536,
                             registered, error));
    CHECK(geometry::cluster_file_hash(registered) == actual);
  }
}
