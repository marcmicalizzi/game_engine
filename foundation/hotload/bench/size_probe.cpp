// Binary-size probe for E7's C++ arm (docs/experiments/e7-luau-spike.md, "The C++ hot-reload
// arm"): one small program built twice, with and without a module host, linking the same engine
// modules otherwise, so the difference in executable size is what the hotload host adds to a game
// executable — the counterpart of foundation/scripting/bench/size_probe.cpp, which measures the
// Luau VM and compiler. The module itself is a separate file and is measured as one.
//
// Built only with the benches (foundation/hotload/CMakeLists.txt); never run by the build or the
// tests. `engine_hotload_size_probe_with <module>` exits 0 when the module loaded and answered;
// game/sandbox_npc refuses to load here, because this host registers none of the record types it
// reads, which does not matter to a size.

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/schema/type_info.h>
#include <foundation/io/vfs.h>

#if ENGINE_SIZE_PROBE_WITH_HOTLOAD
#include <foundation/hotload/module_host.h>
#endif

#include <string>

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_probe, "probe");
}

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  using namespace engine;
  // What both variants do: read the registry, the file system and the log the host uses, so those
  // modules are linked into both and cancel out of the difference.
  const usize types = schema::Registry::global().all().size();
  io::FileInfo info;
  const bool exists = argc > 1 && io::stat_file(argv[1], info) == io::Status::Ok;
  JsonValue value = JsonValue::object();
  value.set("types", JsonValue(static_cast<u64>(types)));
  value.set("exists", JsonValue(exists));
  const std::string text = write_json(value);
  ENGINE_LOG_INFO(log_probe, "probe", log::field("bytes", static_cast<u64>(text.size())));
#if ENGINE_SIZE_PROBE_WITH_HOTLOAD
  if (argc < 2) return 1;
  hotload::ModuleHost host;
  if (host.load(argv[1]) != hotload::Status::Ok) return 1;
  hotload::FunctionId fn;
  if (host.find<hotload::VoidFn>("empty", fn) != hotload::Status::Ok) return 1;
  host.function<hotload::VoidFn>(fn)(host.context());
#endif
  return 0;
}
