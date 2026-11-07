// `engine-content channels` (docs/subsystems/apps.md, "engine-content"; docs/subsystems/channels.md,
// "The water's description"): the network a scene's `channels` placements make over its terrain,
// written as the water's channel description — `engine.scene.WaterChannel` blocks under
// `water_channels` — for the scene's author to paste into the scene the water draws. It calls the
// one function the placement generator calls (`channels::plan_channels`), over the ground provider
// the scene's terrain names, found through the same registry the renderer finds it through.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_CHANNELS
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <schemas/scene.h>

#include <charconv>
#include <cmath>
#include <string>
#endif

namespace engine::content {

int channels_command(int argc, char** argv) {
#if !ENGINE_CONTENT_CHANNELS
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "channels requires the channels capability (ENGINE_WITH_CHANNELS)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content channels <scene.json> <out.json> [--ddc <dir>] [--min-area-m2 <m2>]\n"
      "                               [--stage <fraction>] [--entry <placement index>]\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[2], out_path = argv[3];
  std::string ddc;
  f64 min_area = 50000.0;
  f64 stage = .5;
  i64 only = -1;
  for (int i = 4; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
      return 2;
    }
    const std::string_view value = argv[++i];
    auto number = [&](f64& out) {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), out);
      return r.ec == std::errc{} && r.ptr == value.data() + value.size() && std::isfinite(out);
    };
    bool ok = true;
    if (flag == "--ddc") {
      ddc = std::string(value);
    } else if (flag == "--min-area-m2") {
      ok = number(min_area) && min_area >= 0;
    } else if (flag == "--stage") {
      ok = number(stage) && stage > 0 && stage <= 1;
    } else if (flag == "--entry") {
      f64 v = 0;
      ok = number(v) && v >= 0 && v == std::floor(v);
      only = static_cast<i64>(v);
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "channels: invalid %.*s\n%s", static_cast<int>(flag.size()),
                   flag.data(), usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "channels: %s\n", e.c_str());
    return 1;
  };
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok) return fail("cannot read " + scene_path);
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message);
  const JsonValue* terrain_json = json.find("terrain");
  if (terrain_json == nullptr) return fail(scene_path + " has no terrain to drain");
  scene::Terrain terrain;
  schema::ReadContext reader;
  if (!schema::from_json(terrain, *terrain_json, reader) || !reader.ok()) {
    std::string message = scene_path + ": terrain";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return fail(message);
  }
  // The provider the renderer would draw this terrain with (renderer::terrain_provider's rule).
  const std::string provider =
      !terrain.provider.empty()
          ? terrain.provider
          : (terrain.generator == scene::TerrainGenerator::Dunes ? "dunes" : "waves");
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const scene_gen::GroundProviderDesc* found = registry.find_ground(provider);
  if (found == nullptr) return fail("the terrain " + registry.unknown_ground(provider));
  const std::string dir(io::parent_path(scene_path));
  scene_gen::Context context;
  context.world_seed = terrain.seed;
  context.dir = dir;
  scene_gen::GroundProvider ground;
  std::string error;
  if (!found->make(terrain, context, ground, &error) || !ground.valid())
    return fail(error.empty() ? "the ground provider \"" + provider + "\" refused the terrain"
                              : error);
  (void)ddc;  // the network writes nothing; the flag is accepted for symmetry with `build`
  const JsonValue* placements = json.find("placements");
  JsonValue all = JsonValue::array();
  u32 entries = 0, heads = 0, links = 0;
  f64 metres = 0, area = 0;
  if (placements != nullptr && placements->is_array()) {
    for (u32 k = 0; k < placements->as_array().size(); ++k) {
      const JsonValue& entry = (*placements)[k];
      const JsonValue* generator = entry.find("generator");
      if (generator == nullptr || !generator->is_string() || generator->as_string() != "channels")
        continue;
      if (only >= 0 && static_cast<i64>(k) != only) continue;
      const JsonValue* params = entry.find("params");
      channels::ChannelLayer layer;
      schema::ReadContext layer_reader;
      if (params == nullptr || !schema::from_json(layer, *params, layer_reader) ||
          !layer_reader.ok()) {
        std::string message = scene_path + ": placements " + std::to_string(k) +
                              ": engine.channels.ChannelLayer";
        for (const auto& d : layer_reader.diagnostics)
          message += "; " + d.path + ": " + d.message;
        return fail(message);
      }
      channels::ChannelNetwork network;
      if (!channels::plan_channels(layer, ground.view(), context.world_seed, nullptr, network,
                                   error))
        return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
      const JsonValue blocks =
          channels::water_channels_json(layer, network, min_area, static_cast<f32>(stage));
      for (const JsonValue& b : blocks.as_array())
        all.push_back(b);
      ++entries;
      heads += network.heads.size();
      links += network.links.size();
      metres += network.length_m;
      area += network.area_m2();
    }
  }
  if (entries == 0) return fail(scene_path + " has no \"channels\" placement to run");
  JsonValue out = JsonValue::object();
  const u32 written = all.as_array().size();
  out["water_channels"] = std::move(all);
  if (io::write_file_atomic(out_path, write_json(out)) != io::Status::Ok)
    return fail("cannot write " + out_path);
  JsonValue line = JsonValue::object();
  line["command"] = JsonValue("channels");
  line["scene"] = JsonValue(scene_path);
  line["out"] = JsonValue(out_path);
  line["entries"] = JsonValue(entries);
  line["heads"] = JsonValue(heads);
  line["links"] = JsonValue(links);
  line["channel_m"] = JsonValue(std::round(metres));
  line["region_km2"] = JsonValue(std::round(area / 1e3) / 1e3);
  line["km_per_km2"] = JsonValue(area > 0 ? std::round(metres / area * 1e3 * 100) / 100 : 0.0);
  line["water_channels"] = JsonValue(written);
  JsonWriteOptions compact;
  compact.pretty = false;
  std::printf("%s\n", write_json(line, compact).c_str());
  return 0;
#endif
}

}  // namespace engine::content
