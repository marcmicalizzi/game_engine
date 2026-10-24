// A streamed world's own instances (renderer.md, "Authored instances per tile"; authored.md): the
// reader hands the file's rigid instances to the `authored` generator with a distance form of each
// mesh per ring, keeps what deforms or reaches farther than a tile in the prefix, and every tile
// the generator answers in the innermost ring is the whole read's instances on it, field for field.
// And the pair accounting's sentence. No device: everything here is the load and the CPU's
// placement.
//
// Compiled with the flora capability (the trees) and the authored one (the generator), as a host
// links them; without either there is nothing to stream and the cases are not compiled.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <fstream>
#include <schemas/scene.h>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A small broadleaf with its card levels and a small spruce with none, so a debug build solves both
// in a moment; sixty of each scattered over 160 m, a giant oak that reaches past a tile, and an oak
// with an animation block.
constexpr const char* k_oak =
    R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
    R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
constexpr const char* k_spruce =
    R"({"species":"conifer","conifer":{"seed":5,"height_m":6,"trunk_radius_m":0.1,)"
    R"("whorls":6,"branches_per_whorl":4,"crown_radius_m":1.5,"triangle_budget":8000}})";

std::string scene_text(bool world) {
  std::string text =
      R"({"format":"engine.scene.v1","name":"authored-test",)"
      R"("terrain":{"size":65,"extent":256,"seed":7,"dune_height":0.2,"dune_wavelength":40,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"oak","path":"oak.flora.json"},{"name":"spruce","path":"spruce.flora.json"}],)"
      R"("instances":[{"mesh":0,"translation":[3,0,4],"ground":true},)"
      R"({"mesh":0,"name":"giant","translation":[40,0,-30],"ground":true,"scale":[6,6,6]},)"
      R"({"mesh":0,"name":"walker","translation":[-20,0,10],"ground":true,"animation":{}}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":5,"radius_max":80,"count":60,"seed":11,)"
      R"("scale_min":0.8,"scale_max":1.2},)"
      R"({"mesh":1,"center":[0,0,0],"radius_min":5,"radius_max":80,"count":60,"seed":12}])";
  if (world) {
    text += R"(,"world":{"tile_size":32,"rings":[{"radius":1.5},{"radius":4},{"radius":9}]})";
  }
  return text + "}";
}

struct Loaded {
  SceneDesc desc;
  SceneData data;
};

bool load(const test::TempDir& tmp, bool world, Loaded& out, std::string& error) {
  const std::string path = tmp.file(world ? "streamed.json" : "whole.json");
  if (!write_text(path, scene_text(world))) return false;
  if (!read_scene_file(path, out.desc, error)) return false;
  out.desc.ddc = tmp.file("ddc");
  out.desc.cache = true;
  return load_scene(out.desc, out.data, error);
}

bool generator_present() {
  return scene_gen::GeneratorRegistry::global().find_placement("authored") != nullptr;
}

}  // namespace

#endif

TEST_CASE("authored scene: the pair accounting names its numbers") {
  PairAccount a;
  a.instances = 16675;
  a.prefix_pairs = 27'000'000;
  a.slot_pairs = 400'000;
  a.tail_floor = 0;
  std::string error;
  CHECK_FALSE(pairs_fit(a, &error));
  CHECK(error.find("16675 loaded instances hold 27000000 pairs") != std::string::npos);
  CHECK(error.find("the terrain rings' slots 400000") != std::string::npos);
  CHECK(error.find("27400000 of the 16777216 the visibility id names") != std::string::npos);
  CHECK(error.find("--world") != std::string::npos);
  a.prefix_pairs = 1'000'000;
  a.tail_floor = 50'000;
  CHECK(pairs_fit(a, &error));
  a.limit = 1'400'000;
  a.named = "the ray tracing chain's geometry indices name over the views";
  CHECK_FALSE(pairs_fit(a, &error));
  CHECK(error.find("one tile of its streamed instances 50000") != std::string::npos);
  CHECK(error.find("of the 1400000 the ray tracing chain's") != std::string::npos);
}

#if ENGINE_RENDERER_TESTS_FLORA

TEST_CASE("authored scene: a streamed world's own instances become the generator's entry") {
  if (!generator_present()) {
    MESSAGE("no authored generator in this build");
    return;
  }
  const test::TempDir tmp("renderer_authored_scene");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  std::string error;
  Loaded whole;
  REQUIRE_MESSAGE(load(tmp, false, whole, error), error);
  Loaded streamed;
  REQUIRE_MESSAGE(load(tmp, true, streamed, error), error);

  // Read whole, nothing moves: every instance is the prefix's, no form, nothing streamed.
  CHECK(whole.desc.authored_count == 123);
  CHECK(whole.data.streamed.empty());
  CHECK(whole.data.authored.streamed == 0);
  CHECK(whole.data.parts.size() == 3);  // the two trees and the terrain
  CHECK(whole.data.instances.size() == 124);
  CHECK_FALSE(whole.data.dynamic);

  // The 32 m tile leaves margin for the three-plane foliage bounds; the sixfold giant still
  // exceeds it. Streamed: the giant and the walker (an animation block) stay in
  // the prefix with the terrain; the rest are the entry's, last among the streamed entries.
  const AuthoredStats& stats = streamed.data.authored;
  CHECK(stats.streamed == 121);
  CHECK(stats.kept_reach == 1);
  CHECK(stats.kept_deformed == 1);
  CHECK(streamed.data.instances.size() == 3);
  CHECK(streamed.data.dynamic);
  REQUIRE(streamed.data.streamed.size() == 1);
  const StreamedPlacements& entry = streamed.data.streamed.back();
  CHECK(entry.generator == "authored");
  // The oak has card levels and the spruce a decimated chain: both get smaller forms past the first
  // ring, each placed as its source is and on its source's grid.
  CHECK(stats.forms >= 2);
  REQUIRE(streamed.data.form_source.size() == streamed.data.parts.size());
  u32 forms = 0;
  for (u32 m = 0; m < streamed.data.parts.size(); ++m) {
    const u32 src = streamed.data.form_source[m];
    if (src == m) continue;
    ++forms;
    CAPTURE(m);
    CHECK(streamed.data.parts[m].cluster_count < streamed.data.parts[src].cluster_count);
    CHECK(streamed.data.parts[m].quant_scale == streamed.data.parts[src].quant_scale);
    CHECK(streamed.data.parts[m].quant_origin.y == streamed.data.parts[src].quant_origin.y);
    for (u32 k = 0; k < 4; ++k) {
      const Vec4 a = streamed.data.mesh_fit[m].c[k];
      const Vec4 b = streamed.data.mesh_fit[src].c[k];
      CHECK((a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w));
    }
  }
  CHECK(forms == stats.forms);
  CHECK(streamed.data.tail_floor_pairs > 0);
  CHECK(streamed.data.tail_floor_pairs <= stats.whole_pairs);

  // Every tile the generator answers in the innermost ring is the whole read's instances on it:
  // the same mesh, the same place in f64, the same turn and scale, in the order of their ids —
  // which are their places in the file's own list, plus one.
  const scene_gen::PlacementGeneratorDesc* g =
      scene_gen::GeneratorRegistry::global().find_placement("authored");
  scene_gen::Context context;
  context.tile_size = 32.0f;
  void* state = nullptr;
  REQUIRE_MESSAGE(g->open(entry.params, context, &state, &error), error);
  scene::AuthoredSet set;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(set, entry.params, ctx));
  u32 matched = 0;
  for (i32 z = -6; z <= 6; ++z) {
    for (i32 x = -6; x <= 6; ++x) {
      scene_gen::Placements tile;
      context.ring = 0;
      REQUIRE(g->tile(state, scene_gen::TileCoord{x, z}, context, tile, &error));
      for (const scene_gen::Placement& p : tile.instances) {
        REQUIRE(p.id >= 1);
        const SceneInstance& w = whole.desc.instances[whole.desc.authored_first + p.id - 1];
        CHECK(entry.meshes[p.mesh] == w.mesh);
        CHECK(p.position.x == w.origin.x);
        CHECK(p.position.y == w.origin.y);
        CHECK(p.position.z == w.origin.z);
        CHECK(p.rotation.w == w.transform.rotation.w);
        CHECK(p.scale.x == w.transform.scale.x);
        ++matched;
      }
      // Farther rings draw the forms, and the ring past the last the last one's.
      scene_gen::Placements outer;
      context.ring = 2;
      REQUIRE(g->tile(state, scene_gen::TileCoord{x, z}, context, outer, &error));
      REQUIRE(outer.instances.size() == tile.instances.size());
      for (u32 i = 0; i < outer.instances.size(); ++i) {
        const u32 drawn = entry.meshes[outer.instances[i].mesh];
        CHECK(streamed.data.form_source[drawn] == entry.meshes[tile.instances[i].mesh]);
      }
    }
  }
  g->close(state);
  CHECK(matched == 121);

  // A reload names everything the same: the entry's set, byte for byte.
  Loaded again;
  REQUIRE_MESSAGE(load(tmp, true, again, error), error);
  REQUIRE(again.data.streamed.size() == 1);
  CHECK(write_json(again.data.streamed[0].params) == write_json(entry.params));
  CHECK(again.data.parts.size() == streamed.data.parts.size());

  // `--world-instances whole`: the same streamed world keeps its instances in the prefix.
  SceneDesc kept;
  SceneFileOptions options;
  options.world_instances = false;
  REQUIRE(read_scene_file(tmp.file("streamed.json"), options, kept, error));
  kept.ddc = tmp.file("ddc");
  SceneData kept_data;
  REQUIRE_MESSAGE(load_scene(kept, kept_data, error), error);
  CHECK(kept_data.authored.streamed == 0);
  CHECK(kept_data.instances.size() == 124);
}

#endif
