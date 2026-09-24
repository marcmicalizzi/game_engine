// Binary-size probe for experiment E7 (docs/experiments/e7-luau-spike.md, "What it adds to a
// binary"): one small program built twice, with and without a script call. Both link the same
// engine modules the scripting library itself uses and touch each of them, so the difference in
// executable size is the scripting library and Luau's VM and compiler, and nothing else.
//
// Built only with the benches (foundation/scripting/CMakeLists.txt); never run by the build or
// the tests. `engine_scripting_size_probe_with` exits 0 when the script answered, the other
// always does.

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/schema/type_info.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#if ENGINE_SIZE_PROBE_WITH_SCRIPTING
#include <foundation/scripting/script_context.h>
#endif

#include <string>

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_probe, "probe");
}

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  using namespace engine;
  // What both variants do: read the registries and file system the scripting library reads, so
  // those modules are linked into both and cancel out of the difference.
  const usize types = schema::Registry::global().all().size();
  const tunables::Tunable* tunable = tunables::find("probe.none");
  io::FileInfo info;
  const bool exists = argc > 1 && io::stat_file(argv[1], info) == io::Status::Ok;
  JsonValue value = JsonValue::object();
  value.set("types", JsonValue(static_cast<u64>(types)));
  value.set("exists", JsonValue(exists));
  const std::string text = write_json(value);
  ENGINE_LOG_INFO(log_probe, "probe", log::field("hash", hash_bytes(text.data(), text.size())),
                  log::field("tunable", tunable != nullptr));
#if ENGINE_SIZE_PROBE_WITH_SCRIPTING
  scripting::ScriptContext context;
  scripting::ScriptId id;
  if (context.load("probe.luau", "function f(x) return x + 1 end", id) != scripting::Status::Ok)
    return 1;
  const scripting::Arg args[] = {scripting::Arg::number(static_cast<f64>(argc))};
  JsonValue out;
  if (context.call(id, "f", args, &out) != scripting::Status::Ok) return 1;
  return out == JsonValue(static_cast<i64>(argc + 1)) ? 0 : 1;
#else
  return 0;
#endif
}
