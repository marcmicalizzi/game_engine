// What a decision costs where nothing is classified, which is every asset this repository has
// (docs/subsystems/content_class.md, "What it costs"): `decide` on an all-`none` class, against
// `agent-default`, a mesh at a time. And the classified path beside it, for scale.

#include <foundation/bench/bench.h>
#include <foundation/content_class/content_class.h>

using namespace engine;
using namespace engine::content_class;

namespace {
constexpr u32 k_meshes = 4096;
}

ENGINE_BENCH(content_class_decide_none, "content_class.decide.none") {
  const ViewPolicy policy = agent_default();
  static ContentClasses classes[k_meshes] = {};
  while (state.keep_running()) {
    u32 admitted = 0;
    for (u32 i = 0; i < k_meshes; ++i)
      admitted += decide(classes[i], policy).outcome == Outcome::admit ? 1u : 0u;
    bench::keep(admitted);
  }
  state.set_items(k_meshes);
}

ENGINE_BENCH(content_class_decide_classified, "content_class.decide.classified") {
  const ViewPolicy policy = agent_default();
  static ContentClasses classes[k_meshes] = {};
  for (u32 i = 0; i < k_meshes; ++i)
    classes[i].levels[k_engine_categories + (i % 4)] = static_cast<u8>(i % 3);
  while (state.keep_running()) {
    u32 omitted = 0;
    for (u32 i = 0; i < k_meshes; ++i)
      omitted += decide(classes[i], policy).outcome == Outcome::omit ? 1u : 0u;
    bench::keep(omitted);
  }
  state.set_items(k_meshes);
}
