#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/trees.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstring>

using namespace engine;

namespace {
// G103's first committed MSVC container pins, ground-plants version 2. No accepted pin moves.
// The uncommitted version-1 capture used generic decimation; version 2 preserves every pinna.
// Raw geometry/motion pins remain unchanged (ground-plants.md, Regression evidence).
constexpr u64 k_sections[2][37] = {
    {11310923702420204082ull, 2374871155925766736ull,  6674441434776263015ull,
     5627886797980378692ull,  11679846499717473743ull, 15465540917729557462ull,
     3378653961892416844ull,  11206847717578288037ull, 11155168886135922632ull,
     11286133854226296554ull, 11286133854226296554ull, 2042293585932550210ull,
     2835959889007852657ull,  3275646347912829173ull,  753080197162691681ull,
     10298470454730462158ull, 11391325010373076050ull, 2992554174751281139ull,
     11286133854226296554ull, 2178056141641829995ull,  11286133854226296554ull,
     11286133854226296554ull, 11286133854226296554ull, 11286133854226296554ull,
     11286133854226296554ull, 11286133854226296554ull, 11286133854226296554ull,
     11286133854226296554ull, 618043750719974539ull,   11286133854226296554ull,
     2178056141641829995ull,  11286133854226296554ull, 6378639261567507038ull,
     1439397161573665168ull,  16519257363485727640ull, 3407803047516121549ull,
     9143977360768788356ull},
    {11695310453101302807ull, 8948275318947549224ull,  11172662914119295462ull,
     13471198707307536154ull, 13269522004480330839ull, 5247603925858529505ull,
     5030362213942153173ull,  8458975983816247766ull,  6521252549080255575ull,
     11286133854226296554ull, 11286133854226296554ull, 8671652604495936604ull,
     17169040077552859611ull, 3275646347912829173ull,  1506228763664403473ull,
     2651759468463117830ull,  11286133854226296554ull, 2992554174751281139ull,
     11286133854226296554ull, 2178056141641829995ull,  11286133854226296554ull,
     11286133854226296554ull, 11286133854226296554ull, 11286133854226296554ull,
     11286133854226296554ull, 11286133854226296554ull, 11286133854226296554ull,
     11286133854226296554ull, 618043750719974539ull,   11286133854226296554ull,
     2178056141641829995ull,  11286133854226296554ull, 6064714319750809941ull,
     3466735397584794423ull,  6466140477633518095ull,  3525963009511710957ull,
     7001542934745036560ull}};
}  // namespace

TEST_CASE("ground plants: cooked sections and distance forms are deterministic") {
  constexpr const char* recipes[] = {
      R"({"species":"fern","fern":{"arching":true,"rhizome_length_m":0.8,"dead_fronds":4}})",
      R"({"species":"herb","herb":{"rosette":true,"petiole_length_m":0.12,"leaf_segments":8,"leaf_droop_deg":45,"dead_leaves":6}})"};
  for (u32 n = 0; n < 2u; ++n) {
    flora::FloraFile recipe;
    geometry::ClusterFileData data;
    std::string error;
    REQUIRE(flora::read_flora_file(recipes[n], recipe, error));
    REQUIRE_MESSAGE(flora::build_flora_container(recipe, {}, 65536, data, nullptr, error), error);
    test::TempDir temp("ground_plant_sections");
    const auto path = temp.file("plant.clusters");
    REQUIRE(geometry::write_cluster_file(path, data, &error));
    std::string bytes;
    REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
    geometry::ClusterFileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    REQUIRE(header.section_count == 37u);
    for (u32 i = 0; i < header.section_count; ++i) {
      geometry::ClusterFileSection section;
      std::memcpy(&section, bytes.data() + sizeof(header) + i * sizeof(section), sizeof(section));
      const u64 hash = hash_bytes(bytes.data() + section.offset,
                                  u64{section.element_size} * section.element_count);
      INFO(n, ": section ", section.kind);
      CHECK(section.kind > 0u);
      CHECK(hash == k_sections[n][i]);
    }
    geometry::ClusterFileData back;
    REQUIRE(geometry::read_cluster_file(path, back, &error));
    CHECK(geometry::cluster_file_hash(data) == geometry::cluster_file_hash(back));
    CHECK(back.structural_parts.size() == back.motion_parts.size());
    for (const f32 distance : {5.0f, 20.0f, 60.0f}) {
      geometry::LodFormCut cut;
      cut.distance = distance;
      cut.proj_scale = 1080;
      geometry::ClusterLodMesh form;
      geometry::LodFormMap map;
      Vector<u32> coverage;
      REQUIRE(geometry::extract_lod_form(data.mesh, {}, cut, form, coverage, map, &error));
      CHECK_FALSE(form.mesh.clusters.empty());
      CHECK(map.vertices.size() == form.mesh.vertices.size());
      for (const u32 v : map.vertices)
        CHECK(v < data.motion_vertices.size());
      for (const u32 c : map.clusters)
        CHECK(c < data.cluster_material.size());
    }
  }
}

TEST_CASE("ground plants: the far fern retains every pinna and motion part") {
  for (const u32 pinnae : {3u, 16u, 24u, 40u}) {
    for (const u32 budget : {500u, 4000u}) {
      flora::FloraFile recipe;
      recipe.species = "fern";
      recipe.fern.arching = true;
      recipe.fern.dead_fronds = 4;
      recipe.fern.pinnae = pinnae;
      recipe.fern.triangle_budget = budget;
      geometry::ClusterFileData data;
      flora::TreeReport report;
      std::string error;
      REQUIRE_MESSAGE(
          flora::build_flora_container(recipe, {}, 65536, data, nullptr, error, nullptr, &report),
          error);
      REQUIRE(data.mesh.level_cluster_counts.size() == 2u);
      Vector<u32> selected;
      geometry::select_lod_raw(data.mesh, 1000000.0f, selected);
      Vector<u32> pinna_triangles(data.motion_parts.size(), 0u);
      u32 triangles = 0;
      for (const u32 id : selected) {
        const auto& cluster = data.mesh.mesh.clusters[id];
        CHECK(data.mesh.lod[id].level == 1u);
        triangles += cluster.triangle_count;
        for (u32 t = 0; t < cluster.triangle_count; ++t) {
          const u32 packed = data.mesh.mesh.triangles[cluster.triangle_offset + t];
          const auto& a = data.motion_vertices[cluster.vertex_offset +
                                               geometry::ClusterMesh::unpack(packed, 0)];
          const auto& b = data.motion_vertices[cluster.vertex_offset +
                                               geometry::ClusterMesh::unpack(packed, 1)];
          const auto& c = data.motion_vertices[cluster.vertex_offset +
                                               geometry::ClusterMesh::unpack(packed, 2)];
          CHECK(a.part == b.part);
          CHECK(a.part == c.part);
          if (a.weight == b.weight && b.weight == c.weight) ++pinna_triangles[a.part];
        }
      }
      const u32 rows = std::max(2u, 2u * report.cards_per_side / 3u);
      const u32 coarse_rachis = 2u * ((rows + 3u) / 4u) - 1u;
      CHECK(triangles == report.sprays * (coarse_rachis + 4u * report.cards_per_side));
      CHECK(triangles <= budget);
      CHECK(pinna_triangles[0] == 0u);
      for (u32 part = 1; part < pinna_triangles.size(); ++part)
        CHECK(pinna_triangles[part] == 4u * report.cards_per_side);
    }
  }
}
