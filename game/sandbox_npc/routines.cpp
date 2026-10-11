// 200 lines of the kind of code a content routine holds, the C++ twin of the 200-line script E7's
// Luau arm compiles (scripting_bench.cpp, script_200_lines): small functions over a view, loops
// and conditionals. E7's `rebuild.200_lines` edits this file and times the module's rebuild.
// Exactly 200 lines, like the script; keep it that length when editing.

#include <foundation/hotload/module_abi.h>

using namespace engine;
using namespace engine::hotload;

namespace {

f64 routine_0(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 0) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_1(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 1) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_2(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 2) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_3(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 3) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_4(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 4) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_5(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 5) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_6(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 6) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_7(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 7) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_8(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 8) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_9(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 9) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_10(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 10) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_11(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 11) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_12(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 12) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_13(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 13) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_14(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 14) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

f64 routine_15(ModuleContext* c, View npc, FieldRef health) {
  Value v;
  read(c, npc, health, v);
  f64 score = 0.0;
  for (int i = 1; i <= 8; ++i)
    score += v.f > static_cast<f64>(i * 15) ? i : -1.0;
  const f64 weight = score * (1.0 / 60.0);
  return score > 0.0 && weight < 1000.0 ? weight : -weight;
}

}  // namespace

// All sixteen, so none is unused and the optimizer keeps every one: the rebuild row has to
// compile and link the whole body, as the Luau row compiles the whole script. The routines
// read through views like the rest of the module and return a number instead of building a
// table; the host never inspects it.
//
// One function, called from sandbox_npc.cpp's `routines` export.
f64 run_routines(ModuleContext* context, View npc, FieldRef health) {
  f64 total = 0.0;
  total += routine_0(context, npc, health);
  total += routine_1(context, npc, health);
  total += routine_2(context, npc, health);
  total += routine_3(context, npc, health);
  total += routine_4(context, npc, health);
  total += routine_5(context, npc, health);
  total += routine_6(context, npc, health);
  total += routine_7(context, npc, health);
  total += routine_8(context, npc, health);
  total += routine_9(context, npc, health);
  total += routine_10(context, npc, health);
  total += routine_11(context, npc, health);
  total += routine_12(context, npc, health);
  total += routine_13(context, npc, health);
  total += routine_14(context, npc, health);
  total += routine_15(context, npc, health);
  return total;
}
