// The sandbox NPC game module: the same gameplay feature as E7's Luau arm, in C++, hot-reloadable
// (docs/experiments/e7-luau-spike.md, "The C++ hot-reload arm"; docs/subsystems/hotload.md).
//
// It mirrors the Luau fixtures in foundation/scripting/bench/scripting_bench.cpp one for one:
// `empty`, `echo`, `base`, `ten`, `mixed`, and `on_tick(self, dt)`, which reads the same
// properties of a bound character, branches the same way and returns nothing on the common path.
// Every read goes through the host's views (HostApi::read), resolved once in on_load, because a
// game module links no engine library and may not assume the layout of a host type: the host's
// descriptor says where a field is, at run time.

#include <foundation/hotload/module_state.h>

#include <schemas/sandbox_npc.h>

using namespace engine;
using namespace engine::hotload;
using Brain = sandbox_npc::NpcBrain;

f64 run_routines(ModuleContext* context, View npc, FieldRef health);  // routines.cpp

namespace {

constexpr const char* k_character = "engine.hotload_test.Character";
constexpr const char* k_ten = "engine.hotload_test.Ten";

struct Fields {
  FieldRef alive, health, faction, position, name, strength;
  FieldRef a, b, c, d, e, f, g, h, i, j;
  i64 ironbound = 0;
  i64 drifters = 0;
} g_fields;

i32 on_load(ModuleContext* context) {
  Fields& f = g_fields;
  resolve(context, k_character, "alive", f.alive);
  resolve(context, k_character, "health", f.health);
  resolve(context, k_character, "faction", f.faction);
  resolve(context, k_character, "position", f.position);
  resolve(context, k_character, "name", f.name);
  resolve(context, k_character, "attributes.strength", f.strength);
  resolve(context, k_ten, "a", f.a);
  resolve(context, k_ten, "b", f.b);
  resolve(context, k_ten, "c", f.c);
  resolve(context, k_ten, "d", f.d);
  resolve(context, k_ten, "e", f.e);
  resolve(context, k_ten, "f", f.f);
  resolve(context, k_ten, "g", f.g);
  resolve(context, k_ten, "h", f.h);
  resolve(context, k_ten, "i", f.i);
  resolve(context, k_ten, "j", f.j);
  enum_value(context, f.faction, "Ironbound", f.ironbound);
  enum_value(context, f.faction, "Drifters", f.drifters);
  return 0;  // a path that did not resolve refuses the load by itself
}

void empty(ModuleContext*) {}

f64 echo(ModuleContext*, f64 x) { return x; }

f64 base(ModuleContext*, View) { return 0.0; }

// o.a + o.b + ... + o.j over ten scalar fields of a `Ten`.
f64 ten(ModuleContext* context, View o) {
  const Fields& f = g_fields;
  Value v;
  f64 sum = 0.0;
  read(context, o, f.a, v);
  sum += v.f;
  read(context, o, f.b, v);
  sum += v.f;
  read(context, o, f.c, v);
  sum += v.f;
  read(context, o, f.d, v);
  sum += static_cast<f64>(v.i);
  read(context, o, f.e, v);
  sum += static_cast<f64>(v.u);
  read(context, o, f.f, v);
  sum += v.b ? 1.0 : 0.0;
  read(context, o, f.g, v);
  sum += v.f;
  read(context, o, f.h, v);
  sum += v.f;
  read(context, o, f.i, v);
  sum += static_cast<f64>(v.i);
  read(context, o, f.j, v);
  sum += static_cast<f64>(v.u);
  return sum;
}

// #npc.name, npc.faction == "Drifters", npc.position.x, npc.attributes.strength, npc.alive.
f64 mixed(ModuleContext* context, View npc) {
  const Fields& f = g_fields;
  Value v;
  read(context, npc, f.name, v);
  f64 n = static_cast<f64>(v.str.length);
  read(context, npc, f.faction, v);
  if (v.i == f.drifters) n += 1.0;
  read(context, npc, f.position, v);
  n += static_cast<f64>(v.v[0]);
  read(context, npc, f.strength, v);
  n += static_cast<f64>(v.i);
  read(context, npc, f.alive, v);
  if (v.b) n += 1.0;
  return n;
}

// The Luau routine:
//   if not self.alive then return nil end
//   local threat = self.health < 30 and self.faction ~= "Ironbound"
//   if threat and self.position.y < 0 then return { action = "flee" } end
//   return nil
// The rare result is counted in the state instead of returned as a table.
void on_tick(ModuleContext* context, View self, f32 dt) {
  (void)dt;
  const Fields& f = g_fields;
  Value v;
  read(context, self, f.alive, v);
  if (!v.b) return;
  read(context, self, f.health, v);
  bool threat = v.f < 30.0;
  if (threat) {
    read(context, self, f.faction, v);
    threat = v.i != f.ironbound;
  }
  if (threat) {
    read(context, self, f.position, v);
    if (v.v[1] < 0.0f) state<Brain>(context).flee_requests += 1;
  }
}

f64 routines(ModuleContext* context, View npc) {
  state<Brain>(context).routine_calls += 1;
  return run_routines(context, npc, g_fields.health);
}

}  // namespace

ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<VoidFn>("empty", &empty),
      export_function<NumberFn>("echo", &echo),
      export_function<ViewNumberFn>("base", &base),
      export_function<ViewNumberFn>("ten", &ten),
      export_function<ViewNumberFn>("mixed", &mixed),
      export_function<TickFn>("on_tick", &on_tick),
      export_function<ViewNumberFn>("routines", &routines),
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "sandbox_npc";
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  describe_state<Brain>(out);
  return 0;
}
