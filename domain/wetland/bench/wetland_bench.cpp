#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/wetland/wetland.h>
#include <foundation/bench/bench.h>
using namespace engine;
ENGINE_BENCH_ARGS(solve_plants, "wetland.solve", 0, 1, 2, 3, 4, 5, 6, 7) {
  wetland::Plant p;
  p.species =
      geometry::flora_species_info(wetland::profiles()[static_cast<u32>(state.arg())].species).name;
  assets::MeshData mesh;
  flora::Motion motion;
  wetland::Report report;
  std::string error;
  state.set_items(1);
  while (state.keep_running()) {
    const bool ok = wetland::solve(p, mesh, motion, report, error);
    bench::keep(ok);
    bench::keep(report.triangles);
  }
}
ENGINE_BENCH(patch_tile, "wetland.tile") {
  wetland::Patch p;
  p.meshes.push_back("sedge.wetland.json");
  p.radius_x_m = p.radius_z_m = 32;
  p.spacing_m = .5f;
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("wetland");
  scene_gen::Context context;
  void* entry = nullptr;
  std::string error;
  if (!gen || !gen->open(schema::to_json(p), context, &entry, &error)) return;
  scene_gen::Placements placements;
  state.set_items(1);
  while (state.keep_running()) {
    const bool ok = gen->tile(entry, {0, 0}, context, placements, &error);
    bench::keep(ok);
    bench::keep(placements.instances.size());
  }
  gen->close(entry);
}
