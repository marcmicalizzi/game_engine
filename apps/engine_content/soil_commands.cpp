#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_SOIL
#include <core/json/json.h>
#include <domain/soil/soil.h>

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#endif
namespace engine::content {
int soil_command(int argc, char** argv) {
#if !ENGINE_CONTENT_SOIL
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "soil-cook requires the soil capability\n");
  return 1;
#else
  if (argc != 8) {
    std::fprintf(stderr,
                 "usage: engine-content soil-cook <recipe.soil.json> <heightfield.json> "
                 "<new-out-dir> <x> <y> <z>\n");
    return 2;
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "soil-cook: %s\n", e.c_str());
    return 1;
  };
  WorldPos origin;
  f64* coords[] = {&origin.x, &origin.y, &origin.z};
  for (u32 i = 0; i < 3; ++i) {
    const std::string_view s = argv[5 + i];
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *coords[i]);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(*coords[i]))
      return fail("invalid origin coordinate");
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::absolute(argv[4], ec);
  if (ec) return fail(ec.message());
  for (auto p = dir; !p.empty();) {
    if (fs::exists(p / "AGENTS.md", ec))
      return fail("generated terrain belongs outside a repository");
    const auto parent = p.parent_path();
    if (parent == p) break;
    p = parent;
  }
  if (fs::exists(dir, ec))
    return fail("output directory already exists; choose a new cook directory");
  std::ifstream input(argv[2], std::ios::binary);
  if (!input) return fail("cannot read recipe");
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  soil::SoilFile file;
  std::string error;
  tileset::Heightfield field;
  u64 source = 0;
  if (!soil::read_file(text, file, error) ||
      !tileset::read_heightfield(argv[3], field, source, error) ||
      !soil::cook(file, field, origin, error))
    return fail(error);
  assets::MeshData mesh;
  soil::Report report;
  if (!soil::solve(file, mesh, report, error)) return fail(error);
  if (!fs::create_directories(dir, ec) || ec)
    return fail("cannot create output directory: " + ec.message());
  const auto write = [&](const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
    return static_cast<bool>(out);
  };
  if (!tileset::write_heightfield((dir / "ground.json").string(), field, error)) return fail(error);
  if (!write(dir / "feature.soil.json", soil::canonical_json(file)))
    return fail("cannot write cooked recipe");
  JsonValue scene;
  const auto parsed = parse_json(R"({"format":"engine.scene.v1","name":"soil study",
    "terrain":{"provider":"tiles","tiles":{"heightfield":"ground.json","ddc":"derived","far_levels":1},"size":257,"extent":16,
      "materials":{"layers":[{"name":"soil","tint":[0.14,0.075,0.035],"roughness_factor":0.95}]}},
    "meshes":[{"name":"soil","path":"feature.soil.json"}],
    "instances":[{"mesh":0,"name":"cooked soil feature","translation":[0,0,0]}]})",
                                 scene);
  if (!parsed.ok) return fail(parsed.message);
  auto& terrain = scene.as_object()["terrain"].as_object();
  terrain["size"] = field.width;
  terrain["extent"] =
      static_cast<f64>(field.width - 1) * static_cast<f64>(field.spacing_mm) / 2000.0;
  auto& xyz = scene.as_object()["instances"].as_array()[0].as_object()["translation"].as_array();
  xyz[0] = origin.x;
  xyz[1] = origin.y;
  xyz[2] = origin.z;
  // The finite terrain grid is centred at zero. A distant or offset source needs the
  // existing world ground path so the review scene follows the feature's world anchor.
  if (field.origin_x_mm != -static_cast<i64>(field.width - 1) * field.spacing_mm / 2 ||
      field.origin_z_mm != -static_cast<i64>(field.height - 1) * field.spacing_mm / 2 ||
      field.width != field.height || field.width > 4097 || field.height > 4097) {
    // World ground replaces this finite carrier; keep its material grid within scene limits.
    terrain["size"] = 257;
    terrain["extent"] = 16.0;
    JsonValue world;
    const auto world_parsed = parse_json(R"({"tile_size":8,"rings":[{"radius":2,"ground_cells":64},
      {"radius":4,"ground_cells":32},{"radius":8,"ground_cells":16}],"simulated":1,"ground":true})",
                                         world);
    if (!world_parsed.ok) return fail(world_parsed.message);
    scene.as_object()["world"] = std::move(world);
    // The tile source's filtered far chain must match the outer ring's 8/16 m lattice.
    terrain["tiles"].as_object()["ring_spacing_mm"] = 500;
    terrain["tiles"].as_object()["far_levels"] = 6;
  }
  // Write the scene last: its presence means both halves of the cook are available.
  JsonValue provenance = JsonValue::object();
  provenance.as_object()["source_heightfield_hash"] = source;
  provenance.as_object()["soil_source_hash"] = soil::source_hash(file);
  provenance.as_object()["solver_version"] = soil::k_soil_version;
  provenance.as_object()["source_triangles"] = report.triangles;
  provenance.as_object()["origin"] =
      scene.as_object()["instances"].as_array()[0].as_object()["translation"];
  if (!write(dir / "provenance.json", write_json(provenance)) ||
      !write(dir / "scene.json", write_json(scene)))
    return fail("cannot finish cook metadata");
  std::printf("%s\n", write_json(provenance, JsonWriteOptions{.pretty = false}).c_str());
  return 0;
#endif
}
}  // namespace engine::content
