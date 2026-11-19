// What a streamed world's authored entry costs as a tree of parameters (W150; renderer.md, "What
// the load reads"): `stream_authored` hands the file's rigid instances to the "authored" generator
// as `engine.scene.AuthoredSet` written into a `JsonValue` (`schema::to_json`), which `load_scene`
// copied into `SceneData::streamed` until W150 and freed again at its end, and which the world
// reads back (`schema::from_json`, the generator's `open`). The forest staging hands it about
// 465,000 instances over 3,849 meshes' form sets of 7 rings; a smoke run hands it a hundredth.
//
// Rows: the tree written, copied (what the load no longer does), read back, and freed.
#include <core/base/types.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <foundation/bench/bench.h>

#include <schemas/scene.h>

using namespace engine;

namespace {

constexpr u32 k_staging_streamed = 465'000;
constexpr u32 k_staging_meshes = 3'849;
constexpr u32 k_rings = 7;

const scene::AuthoredSet& staging_set() {
  static scene::AuthoredSet set = [] {
    scene::AuthoredSet s;
    s.tile_size = 64.0f;
    const u32 meshes = bench::smoke_mode() ? k_staging_meshes / 100 : k_staging_meshes;
    const u32 count = bench::smoke_mode() ? k_staging_streamed / 100 : k_staging_streamed;
    for (u32 m = 0; m < meshes; ++m) {
      scene::AuthoredForms forms;
      for (u32 r = 0; r < k_rings; ++r)
        forms.rings.push_back(m * k_rings + r);
      s.forms.push_back(std::move(forms));
    }
    u64 state = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < count; ++i) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      scene::AuthoredInstance a;
      a.forms = static_cast<u32>(state % meshes);
      a.translation = WorldPos{static_cast<f64>(state % 3'000'000) / 1000.0 - 1500.0,
                               static_cast<f64>((state >> 20) % 400'000) / 1000.0,
                               static_cast<f64>((state >> 40) % 10'000'000) / 1000.0 - 1800.0};
      a.scale = Vec3{0.9f, 0.9f, 0.9f};
      a.id = i + 1;
      s.instances.push_back(a);
    }
    return s;
  }();
  return set;
}

}  // namespace

ENGINE_BENCH(authored_params_write, "renderer.authored.params.write") {
  const scene::AuthoredSet& set = staging_set();
  while (state.keep_running()) {
    JsonValue params = schema::to_json(set);
    bench::keep(params);
  }
  state.set_items(set.instances.size());
}

ENGINE_BENCH(authored_params_copy, "renderer.authored.params.copy") {
  const scene::AuthoredSet& set = staging_set();
  const JsonValue params = schema::to_json(set);
  while (state.keep_running()) {
    JsonValue copy = params;
    bench::keep(copy);
    state.pause_timing();
    copy = JsonValue();  // the free is the next row's
    state.resume_timing();
  }
  state.set_items(set.instances.size());
}

ENGINE_BENCH(authored_params_free, "renderer.authored.params.free") {
  const scene::AuthoredSet& set = staging_set();
  const JsonValue params = schema::to_json(set);
  while (state.keep_running()) {
    state.pause_timing();
    JsonValue copy = params;
    state.resume_timing();
    copy = JsonValue();
    bench::keep(copy);
  }
  state.set_items(set.instances.size());
}

ENGINE_BENCH(authored_params_read, "renderer.authored.params.read") {
  const scene::AuthoredSet& set = staging_set();
  const JsonValue params = schema::to_json(set);
  while (state.keep_running()) {
    scene::AuthoredSet back;
    schema::ReadContext ctx;
    ENGINE_VERIFY(schema::from_json(back, params, ctx) && ctx.ok(), "authored bench: no read");
    bench::keep(back.instances.size());
  }
  state.set_items(set.instances.size());
}
