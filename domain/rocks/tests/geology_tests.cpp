#include "rock_checks.h"

#include <domain/rocks/rocks.h>

#include <doctest/doctest.h>

#include <limits>

using namespace engine;
using namespace engine::rocks;
using namespace engine::rocks::checks;

namespace {
assets::MeshData body(Boulder b) {
  RockFile file;
  file.boulder = b;
  assets::MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(solve_rock(file, mesh, nullptr, &error), error);
  return mesh;
}
constexpr RockType k_types[] = {RockType::Granite,   RockType::Basalt,    RockType::Obsidian,
                                RockType::Sandstone, RockType::Limestone, RockType::Shale,
                                RockType::Slate,     RockType::Schist,    RockType::Gneiss,
                                RockType::Marble};
}  // namespace

TEST_CASE("rocks: lithology families and all quality tiers produce closed distinct bodies") {
  CHECK(rock_family(RockType::Obsidian) == RockFamily::Igneous);
  CHECK(rock_family(RockType::Shale) == RockFamily::Sedimentary);
  CHECK(rock_family(RockType::Gneiss) == RockFamily::Metamorphic);
  std::vector<u64> hashes;
  for (RockType type : k_types) {
    CAPTURE(std::string(rock_type_name(type)));
    u32 previous = 0;
    for (RockQuality quality : {RockQuality::Low, RockQuality::Medium, RockQuality::High}) {
      Boulder b;
      b.type = type;
      b.quality = quality;
      b.triangle_budget = 20000;
      const auto mesh = body(b);
      check_closed(mesh, 1);
      CHECK(mesh.indices.size() / 3 <= b.triangle_budget);
      CHECK(mesh.indices.size() > previous);
      previous = mesh.indices.size();
      CHECK(mesh.materials[0].metallic == 0.0f);
      CHECK(mesh_hash(body(b)) == mesh_hash(mesh));
      if (quality == RockQuality::High) {
        const u64 hash = mesh_hash(mesh);
        CHECK(std::find(hashes.begin(), hashes.end(), hash) == hashes.end());
        hashes.push_back(hash);
      }
    }
  }
}

TEST_CASE("rocks: group quality keeps every part and respects even the minimum viable budget") {
  for (RockShape shape : {RockShape::Outcrop, RockShape::Scree}) {
    u32 previous = 0;
    for (RockQuality q : {RockQuality::Low, RockQuality::Medium, RockQuality::High}) {
      RockFile file;
      file.shape = shape;
      if (shape == RockShape::Outcrop) {
        file.outcrop = Outcrop{};
        file.outcrop->type = RockType::Gneiss;
        file.outcrop->count = 4;
        file.outcrop->quality = q;
        file.outcrop->triangle_budget = 20000;
      } else {
        file.scree = Scree{};
        file.scree->type = RockType::Slate;
        file.scree->count = 4;
        file.scree->quality = q;
        file.scree->triangle_budget = 20000;
      }
      assets::MeshData mesh;
      RockReport report;
      std::string error;
      REQUIRE_MESSAGE(solve_rock(file, mesh, &report, &error), error);
      check_closed(mesh, 4);
      CHECK(report.triangles > previous);
      CHECK(report.triangles <= 20000);
      previous = report.triangles;
      if (file.outcrop) file.outcrop->triangle_budget = 1000;
      if (file.scree) file.scree->triangle_budget = 1000;
      REQUIRE_MESSAGE(solve_rock(file, mesh, &report, &error), error);
      CHECK(report.triangles == 768);
      CHECK(report.parts == 4);
    }
  }
}

TEST_CASE("rocks: weathering and fabric controls alter geometry without changing topology") {
  Boulder b;
  b.type = RockType::Gneiss;
  b.triangle_budget = 80000;
  b.geology.bed_m = 0.3f;
  b.geology.fold = 0.0f;
  const u64 straight = mesh_hash(body(b));
  b.geology.fold = 0.8f;
  auto folded = body(b);
  CHECK(mesh_hash(folded) != straight);
  check_closed(folded, 1);
  b.geology.dip_deg = 65.0f;
  b.geology.azimuth_deg = 45.0f;
  CHECK(mesh_hash(body(b)) != mesh_hash(folded));
  b.geology.weathering = 0.0f;
  auto fresh = body(b);
  b.geology.weathering = 1.0f;
  auto worn = body(b);
  CHECK(mesh_hash(fresh) != mesh_hash(worn));
  CHECK(fresh.indices.size() == worn.indices.size());
  check_closed(worn, 1);
  for (RockType type : k_types) {
    b.type = type;
    b.geology.dissolution = 1.0f;
    b.geology.flaking = 1.0f;
    b.geology.granular = 1.0f;
    b.geology.irregularity = 1.0f;
    b.triangle_budget = 4000;
    check_closed(body(b), 1);
  }
}

TEST_CASE("rocks: invalid geology and enum values fail at the public boundary") {
  RockFile file;
  file.boulder = Boulder{};
  std::string error;
  file.boulder->geology.weathering = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(rock_file_in_range(file, error));
  CHECK(error.find("weathering") != std::string::npos);
  file.boulder->geology = RockGeology{};
  file.boulder->geology.bed_m = 0.001f;
  CHECK_FALSE(rock_file_in_range(file, error));
  file.boulder->geology.bed_m = 0.0f;
  CHECK(rock_file_in_range(file, error));
  file.boulder->type = static_cast<RockType>(255);
  CHECK_FALSE(rock_file_in_range(file, error));
  file.boulder->type = RockType::Granite;
  file.boulder->quality = static_cast<RockQuality>(255);
  CHECK_FALSE(rock_file_in_range(file, error));
}

TEST_CASE("rocks: PBR scalar overrides survive texturing and source identity") {
  RockFile file;
  std::string error;
  REQUIRE_MESSAGE(read_rock_file(R"({"shape":"Boulder","boulder":{"type":"Obsidian",
    "triangle_budget":2000,"material":{"base_color":"veined.png","normal":"n.png",
    "roughness":"r.png","tint":[0.3,0.4,0.5],"roughness_factor":0.23}}})",
                                 file, error),
                  error);
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(build_rock_container(file, {}, 0, data, nullptr, error), error);
  REQUIRE(data.materials.size() == 1);
  CHECK(data.materials[0].base_color.x == doctest::Approx(0.3f));
  CHECK(data.materials[0].roughness == doctest::Approx(0.23f));
  CHECK(data.materials[0].base_color_image >= 0);
  CHECK(data.materials[0].normal_image >= 0);
  const u64 first = rock_source_hash(file);
  file.boulder->material.roughness_factor = 0.5f;
  CHECK(rock_source_hash(file) != first);
  file.boulder->material.roughness_factor = -1.0f;
  CHECK_FALSE(rock_file_in_range(file, error));
}

TEST_CASE("rocks: new cliff fabrics stay closed with dipping folded beds") {
  for (RockType type : {RockType::Shale, RockType::Slate, RockType::Schist, RockType::Gneiss,
                        RockType::Marble, RockType::Obsidian}) {
    CAPTURE(std::string(rock_type_name(type)));
    Cliff cliff;
    cliff.type = type;
    cliff.height = 5;
    cliff.talus = 0;
    cliff.talus_blocks = 0;
    cliff.triangle_budget = 20000;
    cliff.geology.dip_deg = 35;
    cliff.geology.fold = 0.7f;
    CliffPoint a;
    CliffPoint b;
    b.x = 8;
    cliff.points.push_back(a);
    cliff.points.push_back(b);
    assets::MeshData mesh;
    CliffReport report;
    std::string error;
    REQUIRE_MESSAGE(solve_cliff(cliff, mesh, &report, &error), error);
    check_closed(mesh, 1);
    CHECK(report.triangles <= cliff.triangle_budget);
  }
}
