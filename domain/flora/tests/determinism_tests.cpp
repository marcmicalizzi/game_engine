#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/flora.h>
#include <domain/flora/regeneration.h>
#include <domain/flora/sward.h>
#include <domain/flora/trees.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <bit>
#include <cstring>

using namespace engine;

TEST_CASE("regeneration: every cooked section has a cross-compiler pin") {
  // All 39 serialized sections, folded in section order; first MSVC pins, juvenile version 1.
  constexpr u64 pins[] = {14638356608917655802ull, 924883813919887783ull,  3543767975535335084ull,
                          17023047174142128883ull, 9673179043758828519ull, 12990953087382750004ull,
                          613653092068049302ull};
  u32 n = 0;
  for (const auto& profile : flora::regeneration_profiles()) {
    flora::FloraFile recipe;
    recipe.species = geometry::flora_species_info(profile.species).name;
    recipe.regeneration.emplace();
    recipe.regeneration->height_m = .7f;
    recipe.regeneration->leader_loss = .3f;
    recipe.regeneration->recovery = .8f;
    geometry::ClusterFileData data;
    std::string error;
    REQUIRE_MESSAGE(flora::build_flora_container(recipe, {}, 65536, data, nullptr, error), error);
    test::TempDir temp("regeneration_sections");
    const auto path = temp.file("plant.clusters");
    REQUIRE(geometry::write_cluster_file(path, data, &error));
    std::string bytes;
    REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
    geometry::ClusterFileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    REQUIRE(header.section_count == 39u);
    u64 combined = 0;
    for (u32 i = 0; i < header.section_count; ++i) {
      geometry::ClusterFileSection section;
      std::memcpy(&section, bytes.data() + sizeof(header) + i * sizeof(section), sizeof(section));
      combined =
          hash_combine(combined, hash_bytes(bytes.data() + section.offset,
                                            u64{section.element_size} * section.element_count));
    }
    INFO(recipe.species);
    CHECK(combined == pins[n++]);
    geometry::ClusterFileData back;
    REQUIRE(geometry::read_cluster_file(path, back, &error));
    CHECK(geometry::cluster_file_hash(data) == geometry::cluster_file_hash(back));
    for (const f32 distance : {5.0f, 20.0f, 60.0f, 200.0f}) {
      geometry::LodFormCut cut;
      cut.distance = distance;
      cut.proj_scale = 1080;
      geometry::ClusterLodMesh form;
      geometry::LodFormMap map;
      Vector<u32> coverage;
      REQUIRE(geometry::extract_lod_form(data.mesh, {}, cut, form, coverage, map, &error));
      CHECK_FALSE(form.mesh.clusters.empty());
      for (const u32 v : map.vertices)
        CHECK(v < data.motion_vertices.size());
      for (const u32 c : map.clusters)
        CHECK(c < data.cluster_material.size());
    }
  }
}

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

// ---- the sward's form decision (G138)
// -------------------------------------------------------------
//
// The stature rule decides a cell's form from its slope, and the slope is an arctangent: these pin
// the decision, taken on MSVC, so the Linux containers prove GCC and Clang at v3 and v2 decide the
// same forms (flora.md, "Growth forms by site"; AGENTS.md: a builder asks the C library for no
// decision).
namespace {
struct SwardMask {
  scene_gen::GroundMaskInfo info;
  Vector<u32> words;
};
const scene_gen::GroundMaskOps k_sward_masks{
    .present = [](const void*, i32, i32) noexcept { return true; },
    .info =
        [](const void* state, scene_gen::GroundMaskInfo& out) noexcept {
          out = static_cast<const SwardMask*>(state)->info;
          return true;
        },
    .tile =
        [](const void* state, i32, i32, std::span<u32> out) noexcept {
          const auto& g = *static_cast<const SwardMask*>(state);
          for (u32 i = 0; i < g.words.size(); ++i)
            out[i] = g.words[i];
          return true;
        }};
// A ground whose slope rises from 5 degrees at x = 0 to 40 at x = 64 m (a gradient linear in x), so
// one tile's cells cross both of the rule's thresholds, 15 and 30 degrees, and every slope between.
f64 sward_rising(const void*, i64 x_mm, i64) noexcept {
  const f64 x = static_cast<f64>(x_mm) * .001;
  return 0.08748866352592401 * x + 0.0058719606847762 * x * x;
}
flora::SwardFamily sward_family(flora::SwardForm form, const char* name) {
  flora::SwardFamily f;
  f.form = form;
  f.near.push_back(std::string(name) + ".near.clusters");
  f.cards.push_back(std::string(name) + ".cards.clusters");
  return f;
}
// The ID, mesh (so the form) and position of every patch of one 64 m tile of 1 m cells, read whole.
// The turn is left out: quat_from_axis_angle is the C library's sine (core/math), not a decision.
u64 sward_pin(const flora::SwardPlacements& p) {
  SwardMask ground;
  ground.info.cells = 64;
  ground.info.spacing_mm = 1000;
  ground.info.palette = 2;
  ground.info.tile_max_x = 0;
  ground.info.tile_max_z = 0;
  ground.words.resize(scene_gen::mask_tile_words(64), 0);
  ground.words[0] = 0xffffff01u;  // the grass, palette 1, in channel 0
  const u32 points = scene_gen::mask_points(64);
  for (u32 i = 0; i < points; ++i)
    ground.words[i + 1] = 255u;
  scene_gen::Context ctx;
  ctx.tile_size = 64;
  ctx.world_seed = 1138;
  ctx.ground.context = &ground;
  ctx.ground.surface_fn = &sward_rising;
  ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
    out = {&k_sward_masks, s};
    return true;
  };
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
  REQUIRE(gen != nullptr);
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(gen->open(schema::to_json(p), ctx, &state, &error), error);
  scene_gen::Placements out;
  const bool ok = gen->tile(state, {0, 0}, ctx, out, &error);
  gen->close(state);
  REQUIRE_MESSAGE(ok, error);
  REQUIRE(out.instances.size() == 4096u);
  u64 pin = k_hash_seed;
  u32 used = 0;
  for (const auto& placed : out.instances) {
    used |= 1u << placed.mesh;
    pin = hash_combine(pin, placed.id);
    pin = hash_combine(pin, placed.mesh);
    pin = hash_combine(pin, hash_bytes(&placed.position, sizeof(placed.position)));
  }
  // A patch's mesh is 2 * (form - 1): the tile holds tall forms and short ones both.
  CHECK((used & 0x5u) != 0u);
  CHECK((used & 0x50u) != 0u);
  return hash_combine(pin, out.instances.size());
}
flora::SwardPlacements sward_four_forms() {
  flora::SwardPlacements p;
  p.layer = 1;
  p.families.push_back(sward_family(flora::SwardForm::tall_sod, "ts"));
  p.families.push_back(sward_family(flora::SwardForm::tall_tussock, "tt"));
  p.families.push_back(sward_family(flora::SwardForm::short_sod, "ss"));
  p.families.push_back(sward_family(flora::SwardForm::short_tussock, "st"));
  return p;
}
}  // namespace

TEST_CASE("sward: the slope's arctangent and the forms it decides have a cross-compiler pin") {
  // The arctangent's bits over the slopes a ground has and the reduction's four ranges.
  u64 bits = k_hash_seed;
  for (u32 i = 0; i <= 8192; ++i) {
    const f64 x = static_cast<f64>(i) * (1.0 / 1024.0) + static_cast<f64>(i % 7) * 1e-9;
    bits = hash_combine(bits, std::bit_cast<u64>(flora::sward_atan(x)));
  }
  CHECK(bits == 17562082363965585281ull);
  // The forms of a tile whose slope crosses both thresholds. With a band of zero, G137's hard line:
  // the pin taken at G138's first commit (61d3ced2), before the band existed, to the bit.
  auto line = sward_four_forms();
  line.stature.band_m = 0;
  CHECK(sward_pin(line) == 8287572821809745209ull);
  // With the default band (3 m) the forms near each boundary are drawn by the cells' hashes; the
  // slope's thresholds and the band's draws both decide here. Re-pinned by G139: the slope draws
  // these boundaries, and the band now reads its gradient as well as the noise's, so it widens.
  CHECK(sward_pin(sward_four_forms()) == 16211441857905139045ull);
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
