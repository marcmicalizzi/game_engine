// A test game module, first build: a counter in its state, a getter, and one view read.
#include <foundation/hotload/module_state.h>

#include <schemas/counter.h>

using namespace engine;
using namespace engine::hotload;
using State = hotload_counter::CounterState;

namespace {

FieldRef g_health;

i32 on_load(ModuleContext* context) {
  resolve(context, "engine.hotload_test.Character", "health", g_health);
  return 0;
}

void bump(ModuleContext* context) { state<State>(context).count += 1; }

// count * scale, plus the argument; the build is told apart by `build`.
f64 get(ModuleContext* context, f64 x) {
  const State& s = state<State>(context);
  return static_cast<f64>(s.count) * static_cast<f64>(s.scale) + x;
}

f64 build(ModuleContext*, f64) { return 1.0; }

f64 read_health(ModuleContext* context, View npc) {
  Value v;
  if (read(context, npc, g_health, v) != Result::Ok) return -1.0;
  return v.f;
}

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
  out->on_load = &on_load;
  describe_state<State>(out);
  return 0;
}
