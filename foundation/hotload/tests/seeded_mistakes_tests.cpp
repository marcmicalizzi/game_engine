// E7's agent-error-rate proxy for the C++ arm (docs/experiments/e7-luau-spike.md, "The C++
// hot-reload arm"): the mistakes the Luau arm seeded into a quest condition, seeded into the same
// condition written as a game module, plus mistakes only C++ can make — and, for each, whether the
// compiler caught it, the loader refused the build, the host refused a read at run time, or the
// module ran and gave the wrong answer. The table on the page is this test's output, and the test
// asserts every row, so the two cannot drift.
//
// Each case is a module compiled from source here with the build's own compiler and flags (the
// command engine_game_module() exports), loaded by a ModuleHost, and called over the same three
// quests the correct condition answers true, false and false.
#include "hotload_test_support.h"

#include <foundation/hotload/module_host.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <schemas/hotload_test.h>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::hotload;
using engine::hotload::test::build_module;
using engine::hotload::test::in_bundle;
using engine::hotload::test::place;
using engine::hotload::test::write_text;

namespace {

// Condition A, correct: the Luau arm's
//   if quest.state ~= "Active" then return false end
//   if not player.alive or player.level < quest.required_level then return false end
//   local item = quest.required_item
//   if item == nil then return true end
//   return table.find(player.inventory, item) ~= nil
constexpr const char* k_condition = R"(#include <foundation/hotload/module_abi.h>
using namespace engine;
using namespace engine::hotload;
namespace {
constexpr const char* k_quest = "engine.hotload_test.Quest";
constexpr const char* k_character = "engine.hotload_test.Character";
struct Refs {
  FieldRef state, required_level, required_item, title, reward_xp, stage;
  FieldRef alive, level, inventory, xp, health;
  i64 active = 0;
  i64 complete = 0;
} g;
/*GLOBALS*/
i32 on_load(ModuleContext* c) {
  resolve(c, k_quest, "state", g.state);
  resolve(c, k_quest, "required_level", g.required_level);
  resolve(c, k_quest, "required_item", g.required_item);
  resolve(c, k_quest, "title", g.title);
  resolve(c, k_quest, "reward_xp", g.reward_xp);
  resolve(c, k_quest, "stage", g.stage);
  resolve(c, k_character, "alive", g.alive);
  resolve(c, k_character, "level", g.level);
  resolve(c, k_character, "inventory", g.inventory);
  resolve(c, k_character, "xp", g.xp);
  enum_value(c, g.state, "Active", g.active);
  /*ON_LOAD*/
  return 0;
}
bool evaluate(ModuleContext* c, View quest, View player) {
  Value v;
  /*FIRST*/
  read(c, quest, g.state, v);
  if (v.i != g.active) return false;
  /*CHECKS*/
  Value alive, level, required;
  read(c, player, g.alive, alive);
  read(c, /*PLAYER*/player, g.level, level);
  read(c, quest, g.required_level, required);
  if (!alive.b || level.u < required.u) return false;
  Value item;
  read(c, quest, g.required_item, item);
  if (item.kind == ValueKind::Nil) return true;
  Value inventory;
  read(c, player, g.inventory, inventory);
  for (u32 i = 0; i < inventory.count; ++i) {
    Value id;
    read_element(c, player, g.inventory, i, id);
    if (id.id[0] == item.id[0] && id.id[1] == item.id[1]) return true;
  }
  /*LAST*/return false;
}
/*FUNCTIONS*/
}  // namespace
ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<ConditionFn>("evaluate", &evaluate),
      /*EXPORTS*/
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "quest";
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  /*DESCRIBE*/
  return 0;
}
)";

enum class Outcome { Compiler, Loader, RefusedRead, RunsWrong, RunsRight, NotRun };

const char* outcome_name(Outcome o) {
  switch (o) {
    case Outcome::Compiler: return "caught by the compiler";
    case Outcome::Loader: return "refused by the loader";
    case Outcome::RefusedRead: return "a read refused at run time (the module ran on)";
    case Outcome::RunsWrong: return "runs, wrong answer";
    case Outcome::RunsRight: return "runs, right answer";
    case Outcome::NotRun: return "compiles and loads; not run (undefined behaviour or a hang)";
  }
  return "?";
}

struct Edit {
  std::string from;
  std::string to;
};

enum class Scenario { Quests, Reload };

struct Seeded {
  const char* id;
  const char* mistake;
  std::vector<Edit> edits;
  Outcome expected;
  Scenario scenario = Scenario::Quests;
  bool run = true;
};

std::string apply_edits(std::string source, const std::vector<Edit>& edits) {
  for (const Edit& e : edits) {
    const usize at = source.find(e.from);
    REQUIRE_MESSAGE(at != std::string::npos, "seeded edit marker not found: " << e.from);
    source.replace(at, e.from.size(), e.to);
  }
  return source;
}

// The mistakes. Rows 1-7 are the Luau arm's seven, in its order (B: guessed names; C: misuse);
// rows 8 on are the C++ arm's own.
std::vector<Seeded> seeded() {
  // MSVC's level-4 C4800 would catch an integer returned as a bool; it is off by default and the
  // engine does not turn it on, and GCC and Clang have no such warning in -Wall -Wextra.
  return {
      {"A", "correct", {}, Outcome::RunsRight},
      {"1",
       "B: `status` for `state`",
       {{"resolve(c, k_quest, \"state\"", "resolve(c, k_quest, \"status\""}},
       Outcome::Loader},
      {"2",
       "B: `hp` for `health`",
       {{"/*ON_LOAD*/", "resolve(c, k_character, \"hp\", g.health);"},
        {"/*CHECKS*/", "read(c, player, g.health, v); if (v.f <= 0.0) return false;"}},
       Outcome::Loader},
      {"3",
       "B: `requiredLevel` for `required_level`",
       {{"\"required_level\"", "\"requiredLevel\""}},
       Outcome::Loader},
      {"4",
       "C: `\"Complete\"` for the enumerator `Completed`",
       {{"/*ON_LOAD*/", "enum_value(c, g.state, \"Complete\", g.complete);"},
        {"/*FIRST*/", "read(c, quest, g.state, v); if (v.i == g.complete) return true;"}},
       Outcome::Loader},
      {"5",
       "C: log level `Information` for `Info`",
       {{"/*FIRST*/", "log(c, LogLevel::Information, \"checking quest\");"}},
       Outcome::Compiler},
      {"6",
       "C: writes `player.xp` (views are read-only)",
       {{"/*CHECKS*/",
         "Value xp, reward; read(c, player, g.xp, xp); read(c, quest, g.reward_xp, reward);"
         " write(c, player, g.xp, xp.u + reward.u);"}},
       Outcome::Compiler},
      {"7",
       "C: returns a number where it promised a boolean (`return quest.stage`)",
       {{"/*LAST*/return false;", "read(c, quest, g.stage, v); return v.u;"}},
       Outcome::RunsWrong},
      {"8",
       "lifetime: keeps a borrowed string past the call",
       {{"/*GLOBALS*/", "const char* g_first_title = nullptr;"},
        {"/*CHECKS*/",
         "Value title; read(c, quest, g.title, title);"
         " if (g_first_title == nullptr) g_first_title = title.str.data;"
         " if (g_first_title[0] == '#') return false;"}},
       Outcome::NotRun,
       Scenario::Quests,
       false},
      {"9",
       "lifetime: keeps a view past the call it was bound for",
       {{"/*GLOBALS*/", "View g_first_player{0, 0}; bool g_have_player = false;"},
        {"/*CHECKS*/", "if (!g_have_player) { g_first_player = player; g_have_player = true; }"},
        {"/*PLAYER*/player", "g_first_player"}},
       Outcome::RefusedRead},
      {"10",
       "ABI: built against another module_abi.h",
       {{"/*DESCRIBE*/", "out->abi_version = 0;"}},
       Outcome::Loader},
      {"11",
       "ABI: an export whose signature is not the one it is called with",
       {{"bool evaluate(ModuleContext* c, View quest, View player)",
         "bool evaluate(ModuleContext* c, const View& quest, const View& player)"},
        {"export_function<ConditionFn>(\"evaluate\", &evaluate)",
         "export_function(\"evaluate\", &evaluate)"}},
       Outcome::Compiler},
      {"12",
       "ABI: the entry point written by hand without extern \"C\"",
       {{"ENGINE_HOTLOAD_DESCRIBE(api, out) {",
         "ENGINE_HOTLOAD_EXPORT i32 engine_hotload_describe(const HostApi* api, ModuleDesc* out) "
         "{"}},
       Outcome::Loader},
      {"13",
       "state: kept in a module global instead of the schema state",
       {{"/*GLOBALS*/", "u32 g_checks = 0;"},
        {"/*FIRST*/", "++g_checks;"},
        {"/*FUNCTIONS*/", "f64 checks(ModuleContext*, f64) { return static_cast<f64>(g_checks); }"},
        {"/*EXPORTS*/", "export_function<NumberFn>(\"checks\", &checks),"}},
       Outcome::RunsWrong,
       Scenario::Reload},
      {"14",
       "an unbounded loop (the counter is never decremented)",
       {{"/*CHECKS*/",
         "read(c, quest, g.stage, v); u64 left = v.u; while (left > 0) { (void)c; }"}},
       Outcome::NotRun,
       Scenario::Quests,
       false},
  };
}

struct Fixture {
  hotload_test::Quest quest;
  hotload_test::Character player;
  bool answer;
};

std::vector<Fixture> fixtures() {
  const Id128 key{7, 9};
  std::vector<Fixture> out(3);
  // Active, the player carries the item: true.
  out[0].quest.title = "The well";
  out[0].quest.state = hotload_test::QuestState::Active;
  out[0].quest.required_item = key;
  out[0].quest.stage = 2;
  out[0].player.level = 3;
  out[0].player.inventory.push_back(key);
  out[0].answer = true;
  // Active, the item is not carried: false (and stage 2, so `return quest.stage` says true).
  out[1].quest.title = "The tower";
  out[1].quest.state = hotload_test::QuestState::Active;
  out[1].quest.required_item = key;
  out[1].quest.stage = 2;
  out[1].player.level = 3;
  out[1].answer = false;
  // Completed: false.
  out[2].quest.title = "The gate";
  out[2].quest.state = hotload_test::QuestState::Completed;
  out[2].quest.stage = 3;
  out[2].player.level = 3;
  out[2].answer = false;
  return out;
}

struct CaseResult {
  Outcome outcome = Outcome::NotRun;
  std::string detail;
};

CaseResult run_case(const engine::test::TempDir& dir, const Seeded& s) {
  CaseResult result;
  const std::string src = dir.file(std::string("case_") + s.id);
  std::filesystem::create_directories(src);
  write_text(src + "/quest.cpp", apply_edits(k_condition, s.edits));
  const std::string library =
      dir.file(std::string("quest_") + s.id +
               std::filesystem::path(HOTLOAD_MODULE_COUNTER_V1).extension().string());
  std::string log;
  if (!build_module(src, library, log)) {
    result.outcome = Outcome::Compiler;
    // The first diagnostic line the compiler printed.
    for (usize at = 0; at < log.size();) {
      const usize end = log.find('\n', at);
      const std::string line = log.substr(at, end == std::string::npos ? end : end - at);
      if (line.find("error") != std::string::npos) {
        result.detail = line;
        break;
      }
      if (end == std::string::npos) break;
      at = end + 1;
    }
    return result;
  }
  ModuleHost host(HostConfig{.shadow_directory = dir.file(std::string("shadow_") + s.id)});
  if (host.load(library) != Status::Ok) {
    result.outcome = Outcome::Loader;
    result.detail = host.last_error();
    return result;
  }
  if (!s.run) return result;
  FunctionId evaluate;
  REQUIRE(host.find<ConditionFn>("evaluate", evaluate) == Status::Ok);
  bool right = true;
  // Each call binds its objects for that call alone, as the Luau host binds an Arg::object.
  auto call = [&](const Fixture& f) {
    const View quest = host.bind(f.quest);
    const View player = host.bind(f.player);
    const bool answer = host.function<ConditionFn>(evaluate)(host.context(), quest, player);
    host.unbind(quest);
    host.unbind(player);
    return answer;
  };
  const std::vector<Fixture> all = fixtures();
  for (const Fixture& f : all)
    right = right && call(f) == f.answer;
  if (s.scenario == Scenario::Reload) {
    // What a module keeps across a reload is what its schema state holds: three calls, a
    // reload of the same build, one more call, and the count should be four.
    REQUIRE(host.reload() == Status::Ok);
    call(all[0]);
    FunctionId checks;
    REQUIRE(host.find<NumberFn>("checks", checks) == Status::Ok);
    const f64 seen = host.function<NumberFn>(checks)(host.context(), 0.0);
    result.detail = "calls counted after a reload: " + std::to_string(seen) + " (4 made)";
    right = right && seen == 4.0;
  }
  if (host.stats().refused_reads > 0) {
    result.outcome = Outcome::RefusedRead;
    result.detail = std::to_string(host.stats().refused_reads) + " reads refused (StaleView)";
    return result;
  }
  result.outcome = right ? Outcome::RunsRight : Outcome::RunsWrong;
  return result;
}

}  // namespace

TEST_CASE(
    "hotload E7: the seeded mistakes, and who catches each (C++ compiler, loader, run time)") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-seeded");
  u32 compiler = 0, loader = 0, refused = 0, wrong = 0, not_run = 0;
  for (const Seeded& s : seeded()) {
    const CaseResult r = run_case(dir, s);
    MESSAGE("seeded " << std::string(s.id) << " (" << std::string(s.mistake)
                      << "): " << std::string(outcome_name(r.outcome))
                      << (r.detail.empty() ? "" : " — " + r.detail));
    CHECK_MESSAGE(r.outcome == s.expected, "seeded " << std::string(s.id) << " expected "
                                                     << std::string(outcome_name(s.expected)));
    if (std::string_view(s.id) == "A") continue;
    switch (r.outcome) {
      case Outcome::Compiler: ++compiler; break;
      case Outcome::Loader: ++loader; break;
      case Outcome::RefusedRead: ++refused; break;
      case Outcome::RunsWrong: ++wrong; break;
      case Outcome::NotRun: ++not_run; break;
      case Outcome::RunsRight: break;
    }
  }
  MESSAGE("totals over 14 seeded mistakes: compiler "
          << compiler << ", loader " << loader << ", refused at run time " << refused
          << ", ran wrong " << wrong << ", not run " << not_run);
  CHECK(compiler == 3);
  CHECK(loader == 6);
  CHECK(refused == 1);
  CHECK(wrong == 2);
  CHECK(not_run == 2);
}

TEST_CASE("hotload E7: the Luau arm's conditions B and C as written, all mistakes at once") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-grouped");
  const std::vector<Seeded> all = seeded();
  // B: the three guessed names in one build. The loader lists all three in one refusal, as the
  // type file names all three in one check.
  Seeded b{"B", "condition B", {}, Outcome::Loader};
  for (const Seeded& s : all)
    if (std::string_view(s.id) == "1" || std::string_view(s.id) == "2" ||
        std::string_view(s.id) == "3")
      for (const Edit& e : s.edits)
        b.edits.push_back(e);
  const CaseResult rb = run_case(dir, b);
  MESSAGE("condition B: " << std::string(outcome_name(rb.outcome)) << " — " << rb.detail);
  CHECK(rb.outcome == Outcome::Loader);
  CHECK(rb.detail.find("'status'") != std::string::npos);
  CHECK(rb.detail.find("'hp'") != std::string::npos);
  CHECK(rb.detail.find("'requiredLevel'") != std::string::npos);

  // C: the four misuses in one build. The compiler stops it; the enum name is the loader's to
  // find once the build compiles, and the returned number is nobody's.
  Seeded c{"C", "condition C", {}, Outcome::Compiler};
  for (const Seeded& s : all)
    if (std::string_view(s.id) == "4" || std::string_view(s.id) == "5" ||
        std::string_view(s.id) == "6" || std::string_view(s.id) == "7") {
      for (const Edit& e : s.edits) {
        // Rows 4 and 5 both write at /*FIRST*/; chain them.
        if (e.from == "/*FIRST*/")
          c.edits.push_back({"/*FIRST*/", e.to + "/*FIRST*/"});
        else
          c.edits.push_back(e);
      }
    }
  const CaseResult rc = run_case(dir, c);
  MESSAGE("condition C: " << std::string(outcome_name(rc.outcome)) << " — " << rc.detail);
  CHECK(rc.outcome == Outcome::Compiler);
}

TEST_CASE("hotload E7: a state field renamed in the schema is carried as dropped and defaulted") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-rename");
  const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
  ModuleHost host(HostConfig{.shadow_directory = dir.file("shadow")});
  REQUIRE(host.load(live) == Status::Ok);
  FunctionId bump, get;
  REQUIRE(host.find<VoidFn>("bump", bump) == Status::Ok);
  REQUIRE(host.find<NumberFn>("get", get) == Status::Ok);
  for (int i = 0; i < 3; ++i)
    host.function<VoidFn>(bump)(host.context());

  const std::string src = dir.file("src");
  std::filesystem::create_directories(src);
  write_text(src + "/counter.schema", R"(namespace engine.hotload_counter
struct CounterState @version(2) {
  total: u32 = 0
  scale: f32 = 1
  label: u16 = 7
}
)");
  write_text(src + "/counter.cpp", R"(#include <foundation/hotload/module_state.h>
#include <schemas/counter.h>
using namespace engine;
using namespace engine::hotload;
using State = hotload_counter::CounterState;
namespace {
void bump(ModuleContext* c) { state<State>(c).total += 1; }
f64 get(ModuleContext* c, f64 x) { return static_cast<f64>(state<State>(c).total) + x; }
f64 build(ModuleContext*, f64) { return 4.0; }
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
)");
  std::string log;
  const bool built = build_module(src, live, log);
  INFO(log);
  REQUIRE(built);
  REQUIRE(host.poll_changes() == 1);
  // The rename is a removal and an addition: reported, not refused, and the count is lost.
  const ReloadReport& report = host.last_report();
  CHECK(report.removed.size() == 1);
  CHECK(report.added.size() == 1);
  CHECK(host.function<NumberFn>(get)(host.context(), 0.0) == 0.0);
  MESSAGE(
      "seeded 15 (state: a field renamed in the schema): runs, wrong answer — the reload "
      "reported 'count' dropped and 'total' defaulted; the count of 3 is gone");
}
