// The typed API: the definition file generated from the schema descriptors, loaded into Luau's
// analyser, catches what the bindings would refuse at run time — before the script runs
// (docs/subsystems/scripting.md, "The typed API"; docs/experiments/e7-luau-spike.md, "What the
// type file catches").
#include "luau_check.h"

#include <core/json/json.h>
#include <core/schema/type_info.h>
#include <foundation/scripting/script_context.h>
#include <foundation/scripting/type_definitions.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <iterator>
#include <schemas/scripting_test.h>
#include <string>
#include <string_view>
#include <vector>

using namespace engine;
using scripting::Arg;
using scripting::ScriptContext;
using scripting::ScriptId;
using scripting::Status;
using scripting::test::check_luau;
using scripting::test::describe;

namespace {

std::string test_definitions() {
  const schema::TypeInfo* types[] = {
      &schema::type_of<scripting_test::Quest>(),
      &schema::type_of<scripting_test::Character>(),
      &schema::type_of<scripting_test::Item>(),
      &schema::type_of<scripting_test::Kinds>(),
  };
  return scripting::emit_type_definitions(types);
}

bool mentions(const std::vector<scripting::test::Diagnostic>& diagnostics, unsigned line,
              std::string_view text) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) {
    return d.line == line && d.message.find(text) != std::string::npos;
  });
}

// The sample a test asserts on: a quest condition written against the typed API, with one
// deliberate type error — `levle` for `level` on line 9.
constexpr std::string_view k_sample = R"(--!strict
-- Can the player finish this quest? It is active, the player is alive and at the level it asks
-- for, and carries the item it asks for (when it asks for one).
function evaluate(quest: Quest, player: Character): boolean
  if quest.state ~= "Active" or not player.alive then
    return false
  end
  local item = quest.required_item
  if player.levle < quest.required_level then
    return false
  end
  return item == nil or table.find(player.inventory, item) ~= nil
end
)";

}  // namespace

TEST_CASE("scripting typed API: the definition file declares every type and property") {
  const std::string defs = test_definitions();
  MESSAGE("definition file:\n" << defs);
  CHECK(defs.find("declare engine: {") != std::string::npos);
  CHECK(defs.find(
            "export type QuestState = \"NotStarted\" | \"Active\" | \"Completed\" | \"Failed\"") !=
        std::string::npos);
  CHECK(defs.find("declare extern type Character with") != std::string::npos);
  CHECK(defs.find("    read health: number\n") != std::string::npos);
  CHECK(defs.find("    read faction: Faction\n") != std::string::npos);
  CHECK(defs.find("    read position: vector\n") != std::string::npos);
  CHECK(defs.find("    read attributes: Attributes\n") != std::string::npos);
  CHECK(defs.find("    read inventory: { string }\n") != std::string::npos);
  CHECK(defs.find("    read home: string?\n") != std::string::npos);
  CHECK(defs.find("    read reputation: { [string]: number }\n") != std::string::npos);
  CHECK(defs.find("    read by_id: { [number]: string }\n") != std::string::npos);
  CHECK(defs.find("    read extra: any\n") != std::string::npos);
  CHECK(defs.find("    -- Hit points; 0 is dead.\n") != std::string::npos);
  // A struct comes after the structs it contains, so the analyser never meets a forward name.
  CHECK(defs.find("declare extern type Attributes with") <
        defs.find("declare extern type Character with"));

  // The output depends on the set of types, not the order they were listed in.
  const schema::TypeInfo* reversed[] = {
      &schema::type_of<scripting_test::Kinds>(),
      &schema::type_of<scripting_test::Item>(),
      &schema::type_of<scripting_test::Character>(),
      &schema::type_of<scripting_test::Quest>(),
  };
  CHECK(scripting::emit_type_definitions(reversed) == defs);
}

TEST_CASE("scripting typed API: the analyser loads the definition file cleanly") {
  const scripting::test::CheckResult result =
      check_luau(test_definitions(), "--!strict\nlocal x: number = engine.tick()\n");
  INFO(describe(result.definition_errors));
  CHECK(result.definitions_loaded);
  CHECK(result.definition_errors.empty());
  INFO(describe(result.diagnostics));
  CHECK(result.diagnostics.empty());
}

TEST_CASE("scripting typed API: the analyser reports the deliberate type error in the sample") {
  const scripting::test::CheckResult result = check_luau(test_definitions(), k_sample);
  REQUIRE(result.definitions_loaded);
  MESSAGE("sample diagnostics:\n" << describe(result.diagnostics));
  REQUIRE(result.diagnostics.size() == 1u);
  CHECK(result.diagnostics[0].line == 9u);
  CHECK(result.diagnostics[0].message.find("levle") != std::string::npos);

  // With the error fixed the sample checks clean, and runs against real objects with the
  // answer the types promised: the definitions and the bindings describe the same thing.
  std::string fixed(k_sample);
  fixed.replace(fixed.find("levle"), 5, "level");
  const scripting::test::CheckResult clean = check_luau(test_definitions(), fixed);
  INFO(describe(clean.diagnostics));
  CHECK(clean.diagnostics.empty());

  ScriptContext context;
  ScriptId id;
  REQUIRE(context.load("sample.luau", fixed, id) == Status::Ok);
  scripting_test::Quest quest;
  quest.state = scripting_test::QuestState::Active;
  quest.required_level = 3;
  quest.required_item = Id128::from_seed(4, 2);
  scripting_test::Character player;
  player.level = 5;
  player.inventory = {Id128::from_seed(4, 1), Id128::from_seed(4, 2)};
  const Arg args[] = {Arg::object(scripting::object_ref(quest)),
                      Arg::object(scripting::object_ref(player))};
  JsonValue out;
  REQUIRE(context.call(id, "evaluate", args, &out) == Status::Ok);
  CHECK(out == JsonValue(true));
  player.inventory.clear();
  REQUIRE(context.call(id, "evaluate", args, &out) == Status::Ok);
  CHECK(out == JsonValue(false));
}

TEST_CASE("scripting typed API: the declared engine API is the one a script gets") {
  // Every function the runtime puts in `engine` is declared, and nothing else is.
  ScriptContext context;
  ScriptId id;
  REQUIRE(context.load("names.luau", R"(
function names()
  local out = {}
  for name in engine do table.insert(out, name) end
  table.sort(out)
  return out
end
)",
                       id) == Status::Ok);
  JsonValue out;
  REQUIRE(context.call(id, "names", {}, &out) == Status::Ok);
  const std::string api = scripting::emit_engine_api_definitions();
  REQUIRE(out.is_array());
  for (const JsonValue& name : out.as_array()) {
    const std::string declared = "    " + std::string(name.as_string()) + ": (";
    INFO(name.as_string());
    CHECK(api.find(declared) != std::string::npos);
  }
  usize declared = 0;
  for (usize at = api.find(": ("); at != std::string::npos; at = api.find(": (", at + 1))
    ++declared;
  CHECK(declared == out.size());
}

// ---- the E7 qualitative row --------------------------------------------------------------------
//
// Three quest conditions written the way an agent writes one from a prose description of the
// quest, before it has looked at the schema: condition A is right, B guesses names, C mutates
// state and misuses the API. Each is checked twice — as written against the definition file, and
// as it would be written without one (the same code, without the type annotations it could not
// have known). The counts are asserted so that the write-up's table cannot drift from what the
// analyser actually says; the diagnostics are printed for the write-up.

namespace {

struct Condition {
  const char* name;
  const char* typed;    // annotated against the definition file
  const char* untyped;  // the same code with no types to annotate against
};

constexpr Condition k_conditions[] = {
    {"A (correct)",
     R"(--!strict
function evaluate(quest: Quest, player: Character): boolean
  if quest.state ~= "Active" then return false end
  if not player.alive or player.level < quest.required_level then return false end
  local item = quest.required_item
  if item == nil then return true end
  return table.find(player.inventory, item) ~= nil
end
)",
     R"(--!strict
function evaluate(quest, player)
  if quest.state ~= "Active" then return false end
  if not player.alive or player.level < quest.required_level then return false end
  local item = quest.required_item
  if item == nil then return true end
  return table.find(player.inventory, item) ~= nil
end
)"},
    {"B (guessed names)",
     R"(--!strict
function evaluate(quest: Quest, player: Character): boolean
  if quest.status ~= "Active" then return false end
  if player.hp <= 0 then return false end
  if player.level < quest.requiredLevel then return false end
  for _, id in player.inventory do
    if id == quest.required_item then return true end
  end
  return false
end
)",
     R"(--!strict
function evaluate(quest, player)
  if quest.status ~= "Active" then return false end
  if player.hp <= 0 then return false end
  if player.level < quest.requiredLevel then return false end
  for _, id in player.inventory do
    if id == quest.required_item then return true end
  end
  return false
end
)"},
    {"C (mutates, misuses the API)",
     R"(--!strict
function evaluate(quest: Quest, player: Character): boolean
  if quest.state == "Complete" then return true end
  engine.log("information", "checking quest", { quest = quest.title })
  player.xp = player.xp + quest.reward_xp
  return quest.stage
end
)",
     R"(--!strict
function evaluate(quest, player)
  if quest.state == "Complete" then return true end
  engine.log("information", "checking quest", { quest = quest.title })
  player.xp = player.xp + quest.reward_xp
  return quest.stage
end
)"},
};

// The mistakes seeded in B and C, and whether the analyser is expected to name each one with the
// type file. The one it does not is an honest limit of the checker, recorded in the write-up:
// comparing a `QuestState` with a string that is not one of its names is not a type error to
// Luau's solver, because `==` between a union of singletons and another string is allowed.
struct Seeded {
  usize condition;
  unsigned line;
  const char* token;
  bool caught_with_types;
};

constexpr Seeded k_seeded[] = {
    {1, 3, "status", true},         // B: the field is `state`
    {1, 4, "hp", true},             // B: the field is `health`
    {1, 5, "requiredLevel", true},  // B: the field is `required_level`
    {2, 3, "Complete", false},      // C: the enumerator is `Completed`
    {2, 4, "information", true},    // C: the level is `info`
    {2, 5, "xp", true},             // C: a read-only property; mutation is a command
    {2, 6, "boolean", true},        // C: returns a number where it promised a boolean
};

}  // namespace

TEST_CASE("scripting typed API: what the type file catches in three quest conditions (E7)") {
  const std::string defs = test_definitions();
  const std::string api_only = scripting::emit_engine_api_definitions();
  std::vector<scripting::test::CheckResult> typed;
  std::vector<scripting::test::CheckResult> untyped;
  for (const Condition& condition : k_conditions) {
    typed.push_back(check_luau(defs, condition.typed));
    untyped.push_back(check_luau(api_only, condition.untyped));
    const auto bare = check_luau({}, condition.untyped);
    REQUIRE(typed.back().definitions_loaded);
    MESSAGE("condition " << std::string(condition.name) << "\n  with the type file:\n"
                         << describe(typed.back().diagnostics)
                         << "  without it (the engine API's definitions only):\n"
                         << describe(untyped.back().diagnostics) << "  without any definitions:\n"
                         << describe(bare.diagnostics));

    // And what the run time says about the same condition: the first error it reaches, if the
    // inputs lead it there.
    ScriptContext context;
    ScriptId id;
    REQUIRE(context.load(condition.name, condition.typed, id) == Status::Ok);
    scripting_test::Quest quest;
    quest.state = scripting_test::QuestState::Active;
    scripting_test::Character player;
    const Arg args[] = {Arg::object(scripting::object_ref(quest)),
                        Arg::object(scripting::object_ref(player))};
    JsonValue out;
    const Status status = context.call(id, "evaluate", args, &out);
    MESSAGE("  at run time: " << std::string(scripting::status_name(status)) << " "
                              << (status == Status::Ok ? write_json(out)
                                                       : std::to_string(context.last_error().line) +
                                                             ": " + context.last_error().message));
  }

  // The correct condition checks clean against the type file.
  CHECK(typed[0].diagnostics.empty());

  usize caught_typed = 0;
  usize caught_untyped = 0;
  for (const Seeded& s : k_seeded) {
    INFO(s.token);
    const bool with_types = mentions(typed[s.condition].diagnostics, s.line, s.token);
    CHECK(with_types == s.caught_with_types);
    caught_typed += with_types ? 1u : 0u;
    caught_untyped += mentions(untyped[s.condition].diagnostics, s.line, s.token) ? 1u : 0u;
  }
  // Every diagnostic the type file produces is one of the seeded mistakes: no noise.
  usize typed_diagnostics = 0;
  for (const auto& result : typed)
    typed_diagnostics += result.diagnostics.size();
  CHECK(typed_diagnostics == caught_typed);
  // Without the type file only the engine API's own definitions catch anything: the log level.
  CHECK(caught_untyped <= 1u);
  MESSAGE("seeded mistakes: " << std::size(k_seeded) << ", caught with the type file: "
                              << caught_typed << ", without it: " << caught_untyped);
}
