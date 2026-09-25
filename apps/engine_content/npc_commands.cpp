// engine-content npcs: the resident generator on the command line (docs/subsystems/npc.md, "The
// generator"; docs/subsystems/apps.md). Compiled against systems/npc when this build has the
// capability (ENGINE_CONTENT_NPC); without it the command is refused with a sentence, as `ruins`
// is, and `msvc-minimal` is the proof that the rest of the app builds.
//
//   npcs <params.json> --out <document dir> [--jobs <n>]
//
// Writes a partitioned layer of places and residents into the document at <document dir> — created
// when there is none, the layer added as its edit layer when there is — and prints one JSON line:
// the places, residents and occupied tiles, the layer, and the milliseconds the generation and the
// save took. Derived data: never committed; a test writes it under its own scratch directory.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_NPC
#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <foundation/io/vfs.h>
#include <systems/npc/generator.h>

#include <charconv>
#include <memory>
#endif

namespace engine::content {

namespace {

[[maybe_unused]] constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
[[maybe_unused]] constexpr int k_exit_usage = 2;

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content npcs: %s\n", message.c_str());
  return k_exit_error;
}

[[maybe_unused]] int usage(const char* message) {
  std::fprintf(stderr,
               "engine-content npcs: %s\n"
               "usage: engine-content npcs <params.json> --out <document dir> [--jobs <n>]\n",
               message);
  return k_exit_usage;
}

#if ENGINE_CONTENT_NPC
int npcs(int argc, char** argv) {
  std::string params_path;
  std::string out;
  u32 workers = 0;
  bool have_jobs = false;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      out = argv[++i];
    } else if (a == "--jobs" && i + 1 < argc) {
      const std::string_view v = argv[++i];
      if (std::from_chars(v.data(), v.data() + v.size(), workers).ec != std::errc{})
        return usage("--jobs is a number of workers");
      have_jobs = true;
    } else if (a == "--help") {
      return usage("the resident generator");
    } else if (!a.empty() && a[0] != '-' && params_path.empty()) {
      params_path = std::string(a);
    } else {
      return usage("unknown option");
    }
  }
  if (params_path.empty() || out.empty()) return usage("a params file and --out are required");
  std::string text;
  if (io::read_file(params_path, text) != io::Status::Ok)
    return failed("cannot read " + params_path);
  JsonValue json;
  if (!parse_json(text, json).ok) return failed(params_path + " is not JSON");
  npc::GeneratorParams params;
  std::string error;
  if (!npc::parse_params(json, params, error)) return failed(error);

  std::unique_ptr<jobs::JobSystem> pool;
  if (!have_jobs || workers > 0) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 0;
    pool = std::make_unique<jobs::JobSystem>(config);
  }
  const time::Stopwatch clock;
  io::Vfs vfs;
  npc::GeneratorStats stats;
  if (!npc::generate_document(vfs, out, params, pool.get(), error, &stats)) return failed(error);
  JsonValue line = JsonValue::object();
  line.set("layer", JsonValue(params.layer));
  line.set("places", JsonValue(static_cast<u64>(stats.places)));
  line.set("residents", JsonValue(static_cast<u64>(stats.residents)));
  line.set("tiles", JsonValue(static_cast<u64>(stats.tiles)));
  line.set("seed", JsonValue(params.seed));
  line.set("generate_ms", JsonValue(stats.ms));
  line.set("total_ms", JsonValue(static_cast<f64>(clock.elapsed_ns()) / 1.0e6));
  std::printf("%s\n", write_json(line, JsonWriteOptions{.pretty = false}).c_str());
  return k_exit_ok;
}
#endif

}  // namespace

int npcs_command(int argc, char** argv) {
#if ENGINE_CONTENT_NPC
  return npcs(argc, argv);
#else
  (void)argc;
  (void)argv;
  return failed(
      "this engine-content was built without the npc capability (ENGINE_WITH_NPC=OFF, "
      "ENGINE_WITH_ECS=OFF or ENGINE_MINIMAL=ON); configure with it on to generate residents");
#endif
}

}  // namespace engine::content
