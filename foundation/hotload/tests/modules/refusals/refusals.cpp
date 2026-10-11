// Test game modules that the host must refuse, one per build, chosen by a definition the CMake
// file sets (HOTLOAD_REFUSAL): no entry point, a describe that fails, a module built against
// another ABI, field paths that do not resolve, and a build that drops a function the host holds.
#include <foundation/hotload/module_abi.h>

using namespace engine;
using namespace engine::hotload;

namespace {

[[maybe_unused]] void bump(ModuleContext*) {}
[[maybe_unused]] f64 get(ModuleContext*, f64 x) { return x; }
[[maybe_unused]] f64 read_health(ModuleContext*, View) { return 0.0; }

#if HOTLOAD_REFUSAL == 4
i32 on_load(ModuleContext* context) {
  FieldRef f;
  resolve(context, "engine.hotload_test.Character", "helth", f);
  resolve(context, "engine.hotload_test.Character", "attributes.strenght", f);
  resolve(context, "engine.hotload_test.Character", "health", f);
  i64 value = 0;
  enum_value(context, f, "Ironbound", value);  // health is not an enum
  return 0;
}
#endif

}  // namespace

#if HOTLOAD_REFUSAL == 1
// No entry point: a function with the right name but C++ linkage, so the symbol is mangled and
// the host's lookup finds nothing.
ENGINE_HOTLOAD_EXPORT i32 engine_hotload_describe(const HostApi*, ModuleDesc*) { return 0; }
#else
ENGINE_HOTLOAD_DESCRIBE(api, out) {
  static const Export full[] = {
      export_function<VoidFn>("bump", &bump),
      export_function<NumberFn>("get", &get),
      export_function<NumberFn>("build", &get),
      export_function<ViewNumberFn>("read_health", &read_health),
  };
  [[maybe_unused]] static const Export bump_only[] = {export_function<VoidFn>("bump", &bump)};
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "refusal";
  out->exports = full;
  out->export_count = 4;
  i32 rc = 0;
#if HOTLOAD_REFUSAL == 2
  fail(api, "describe: this build refuses on purpose");
  rc = 7;
#elif HOTLOAD_REFUSAL == 3
  out->abi_version = k_abi_version + 41;
#elif HOTLOAD_REFUSAL == 4
  out->on_load = &on_load;
#elif HOTLOAD_REFUSAL == 5
  out->exports = bump_only;
  out->export_count = 1;
#endif
  (void)api;
  return rc;
}
#endif
