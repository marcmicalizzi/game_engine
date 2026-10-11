// A test game module whose state retyped `count` from u32 to vec3: the host refuses it and keeps
// the build it has.
#include <foundation/hotload/module_state.h>

#include <schemas/counter.h>

using namespace engine;
using namespace engine::hotload;
using State = hotload_counter::CounterState;

namespace {

void bump(ModuleContext* context) { state<State>(context).count.x += 1.0f; }

f64 get(ModuleContext* context, f64 x) {
  return static_cast<f64>(state<State>(context).count.x) + x;
}

f64 build(ModuleContext*, f64) { return 3.0; }

f64 read_health(ModuleContext*, View) { return 0.0; }

}  // namespace

ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<VoidFn>("bump", &bump),
      export_function<NumberFn>("get", &get),
      export_function<NumberFn>("build", &build),
      export_function<ViewNumberFn>("read_health", &read_health),
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "counter";
  out->exports = exports;
  out->export_count = 4;
  describe_state<State>(out);
  return 0;
}
