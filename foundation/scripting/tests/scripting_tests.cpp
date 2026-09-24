// The host's contract: the sandbox, the memory limit, the step budget, errors as statuses with a
// position, results as JSON, determinism, the engine API, and the tick driver
// (docs/subsystems/scripting.md, "Invariants").
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <foundation/scripting/scripting.h>

#include <doctest/doctest.h>

#include <schemas/scripting_test.h>
#include <string>
#include <string_view>

using namespace engine;
using scripting::Arg;
using scripting::ScriptContext;
using scripting::ScriptId;
using scripting::Status;

namespace {

std::string render(const JsonValue& value) {
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(value, options);
}

// Loads `source` as `name` and requires it to load.
ScriptId must_load(ScriptContext& context, std::string_view name, std::string_view source) {
  ScriptId id;
  const Status status = context.load(name, source, id);
  INFO(context.last_error().message);
  REQUIRE(status == Status::Ok);
  return id;
}

Status call(ScriptContext& context, ScriptId script, std::string_view fn, JsonValue* out = nullptr,
            std::span<const Arg> args = {}) {
  Status status = Status::Ok;
  // Nothing may be thrown across the boundary: a script error is a status. The test is compiled
  // with exceptions on precisely so that this check would see one.
  CHECK_NOTHROW(status = context.call(script, fn, args, out));
  return status;
}

}  // namespace

// ---- the sandbox --------------------------------------------------------------------------------

TEST_CASE("scripting sandbox: the forbidden libraries and functions are absent") {
  ScriptContext context;
  const ScriptId id = must_load(context, "probe.luau", R"(
function probe()
  return {
    os = type(os), io = type(io), debug = type(debug), require = type(require),
    loadstring = type(loadstring), load = type(load), dofile = type(dofile),
    loadfile = type(loadfile), package = type(package), getfenv = type(getfenv),
    setfenv = type(setfenv), print = type(print), gcinfo = type(gcinfo),
    collectgarbage = type(collectgarbage), random = type(math.random),
    randomseed = type(math.randomseed),
    -- what a script does get
    math = type(math), string = type(string), table = type(table), engine = type(engine),
    vector = type(vector), coroutine = type(coroutine), buffer = type(buffer),
  }
end
)");
  JsonValue out;
  REQUIRE(call(context, id, "probe", &out) == Status::Ok);
  for (const char* forbidden :
       {"os", "io", "debug", "require", "loadstring", "load", "dofile", "loadfile", "package",
        "getfenv", "setfenv", "print", "gcinfo", "collectgarbage", "random", "randomseed"}) {
    INFO(forbidden);
    REQUIRE(out.find(forbidden) != nullptr);
    CHECK(out.find(forbidden)->as_string() == "nil");
  }
  for (const char* library :
       {"math", "string", "table", "engine", "vector", "coroutine", "buffer"}) {
    INFO(library);
    CHECK(out.find(library)->as_string() == "table");
  }
}

TEST_CASE("scripting sandbox: escape attempts are refused with the error text") {
  ScriptContext context;
  const ScriptId id = must_load(context, "escape.luau", R"(
function run_os() return os.execute("calc") end
function patch_string() string.rep = function() return "" end end
function patch_metatable() getmetatable("").__index.upper = nil end
function rawset_global() rawset(_G, "engine", {}) end
function patch_engine() engine.log = nil end
function new_global() sneaky = 1 end
function change_env() setfenv(1, {}) end
)");
  struct Case {
    const char* function;
    const char* message;
  };
  const Case cases[] = {
      {"run_os", "attempt to index nil"},
      {"patch_string", "readonly"},
      {"patch_metatable", "readonly"},
      {"rawset_global", "readonly"},
      {"patch_engine", "readonly"},
      {"new_global", "readonly"},
      {"change_env", "attempt to call a nil value"},
  };
  for (const Case& c : cases) {
    INFO(c.function);
    CHECK(call(context, id, c.function) == Status::RuntimeError);
    CHECK(context.last_error().chunk == "escape.luau");
    CHECK(context.last_error().line > 1);
    CHECK(context.last_error().message.find(c.message) != std::string::npos);
  }
}

// ---- the budgets ------------------------------------------------------------------------------

TEST_CASE("scripting memory limit: an allocation loop stops at the limit and the VM stays usable") {
  scripting::ScriptLimits limits;
  limits.memory_bytes = u64{4} << 20;
  ScriptContext context(limits);
  const ScriptId id = must_load(context, "hog.luau", R"(
function hog()
  local t = {}
  for i = 1, 100000000 do
    t[i] = string.rep("x", 64) .. i
  end
  return #t
end
function fine() return 7 end
)");
  const u64 before = context.stats().heap_bytes;
  CHECK(call(context, id, "hog") == Status::MemoryExceeded);
  CHECK(context.last_error().chunk == "hog.luau");
  CHECK(context.last_error().message.find("not enough memory") != std::string::npos);
  CHECK(context.stats().heap_peak_bytes <= limits.memory_bytes);
  // The failed call's garbage was collected: the heap is back near where it started.
  CHECK(context.stats().heap_bytes < before + (u64{1} << 20));

  JsonValue out;
  CHECK(call(context, id, "fine", &out) == Status::Ok);
  CHECK(out == JsonValue(i64{7}));
  // And the limit holds a second time.
  CHECK(call(context, id, "hog") == Status::MemoryExceeded);
  CHECK(call(context, id, "fine", &out) == Status::Ok);
}

TEST_CASE("scripting step budget: an infinite loop is interrupted and the VM stays usable") {
  scripting::ScriptLimits limits;
  limits.step_budget = 10'000;
  ScriptContext context(limits);
  const ScriptId id = must_load(context, "spin.luau", R"(
function spin()
  local n = 0
  while true do
    n += 1
  end
end
function stubborn()
  -- pcall cannot swallow the budget: the next safepoint raises again.
  while true do
    pcall(function() while true do end end)
  end
end
function recurse(n) return recurse(n + 1) + 1 end
function fine() return "ok" end
)");
  CHECK(call(context, id, "spin") == Status::BudgetExceeded);
  CHECK(context.last_error().chunk == "spin.luau");
  CHECK(context.last_error().line >= 4);
  CHECK(context.last_error().line <= 5);
  CHECK(context.last_error().message.find("step budget") != std::string::npos);
  CHECK(context.stats().last_call_steps == limits.step_budget + 1);

  CHECK(call(context, id, "stubborn") == Status::BudgetExceeded);
  // Unbounded recursion ends at the budget or at Luau's own stack limit, whichever comes first;
  // either way it is a status, not a crash.
  const Arg recurse_args[] = {Arg::number(0)};
  const Status recursion = call(context, id, "recurse", nullptr, recurse_args);
  CHECK((recursion == Status::BudgetExceeded || recursion == Status::RuntimeError));

  JsonValue out;
  CHECK(call(context, id, "fine", &out) == Status::Ok);
  CHECK(out == JsonValue("ok"));
  CHECK(context.stats().last_call_steps <= 2);
}

TEST_CASE("scripting step budget: a count, the same on every run") {
  // The budget is deterministic because it counts safepoints rather than time: the same function
  // over the same input takes the same number of steps, run after run and machine after machine.
  ScriptContext context;
  const ScriptId id = must_load(context, "count.luau", R"(
function sum(n)
  local s = 0
  for i = 1, n do s += i end
  return s
end
)");
  const Arg args[] = {Arg::number(1000)};
  REQUIRE(call(context, id, "sum", nullptr, args) == Status::Ok);
  const u64 steps = context.stats().last_call_steps;
  CHECK(steps >= 1000);
  CHECK(steps <= 1010);
  for (int i = 0; i < 3; ++i) {
    REQUIRE(call(context, id, "sum", nullptr, args) == Status::Ok);
    CHECK(context.stats().last_call_steps == steps);
  }
}

TEST_CASE("scripting time budget: the optional backstop interrupts too") {
  scripting::ScriptLimits limits;
  limits.step_budget = 0;
  limits.time_budget_ns = 2'000'000;
  ScriptContext context(limits);
  const ScriptId id = must_load(context, "slow.luau", "function spin() while true do end end");
  CHECK(call(context, id, "spin") == Status::BudgetExceeded);
  CHECK(context.last_error().message.find("time budget") != std::string::npos);
}

// ---- errors -----------------------------------------------------------------------------------

TEST_CASE("scripting errors: a runtime error is a status with file, line and message") {
  ScriptContext context;
  const ScriptId id = must_load(context, "quests/q17.luau", R"(-- line 1
local function helper(x)
  return x.missing.field -- line 3
end
function raise()
  error("the bridge is out") -- line 6
end
function arithmetic()
  local n = nil
  return n + 1 -- line 10
end
function nested()
  return helper({}) -- the error is on line 3, in the helper
end
function table_error()
  error({ code = 7 }) -- line 16
end
)");
  CHECK(call(context, id, "raise") == Status::RuntimeError);
  CHECK(context.last_error().chunk == "quests/q17.luau");
  CHECK(context.last_error().line == 6);
  CHECK(context.last_error().message == "the bridge is out");

  CHECK(call(context, id, "arithmetic") == Status::RuntimeError);
  CHECK(context.last_error().line == 10);
  CHECK(context.last_error().message.find("arithmetic") != std::string::npos);

  CHECK(call(context, id, "nested") == Status::RuntimeError);
  CHECK(context.last_error().line == 3);

  CHECK(call(context, id, "table_error") == Status::RuntimeError);
  CHECK(context.last_error().line == 16);
  CHECK(context.last_error().message == "(error object is not a string)");

  CHECK(call(context, id, "no_such_function") == Status::NotFound);
}

TEST_CASE("scripting errors: a compile error is a status with the line, and loads nothing") {
  ScriptContext context;
  ScriptId id;
  CHECK(context.load("broken.luau", "function ok() return 1 end\nfunction f(\n  return\nend\n",
                     id) == Status::CompileError);
  CHECK_FALSE(id.valid());
  CHECK(context.last_error().chunk == "broken.luau");
  CHECK(context.last_error().line >= 2);
  CHECK_FALSE(context.last_error().message.empty());

  // A top level that fails to run loads nothing either.
  CHECK(context.load("throws.luau", "function f() end\nerror('no')\n", id) == Status::RuntimeError);
  CHECK_FALSE(id.valid());
  CHECK(context.last_error().line == 2);
}

TEST_CASE("scripting errors: yielding out of a call is an error, coroutines inside one work") {
  ScriptContext context;
  const ScriptId id = must_load(context, "co.luau", R"(
function generate()
  local gen = coroutine.wrap(function()
    for i = 1, 3 do coroutine.yield(i) end
  end)
  return { gen(), gen(), gen() }
end
function escape() coroutine.yield(1) end
)");
  JsonValue out;
  CHECK(call(context, id, "generate", &out) == Status::Ok);
  CHECK(render(out) == "[1,2,3]");
  CHECK(call(context, id, "escape") == Status::RuntimeError);
}

// ---- results and arguments ----------------------------------------------------------------------

TEST_CASE("scripting results: what a script returns is converted to JSON") {
  ScriptContext context;
  const ScriptId id = must_load(context, "data.luau", R"(
function none() return nil end
function yes() return true end
function whole() return 42 end
function fraction() return 2.5 end
function text() return "flee" end
function list() return { 1, 2, 3 } end
function record() return { action = "flee", target = { x = 1, urgent = true } } end
function empty() return {} end
function vec() return vector.create(1, 2, 3) end
function mixed() return { 1, 2, key = "v" } end
function holey() local t = { 1, 2 } t[4] = 4 return t end
function func() return function() end end
function nan() return 0 / 0 end
function cycle() local t = {} t.self = t return t end
)");
  JsonValue out;
  REQUIRE(call(context, id, "none", &out) == Status::Ok);
  CHECK(out.is_null());
  REQUIRE(call(context, id, "yes", &out) == Status::Ok);
  CHECK(out == JsonValue(true));
  REQUIRE(call(context, id, "whole", &out) == Status::Ok);
  CHECK(out.is_int());
  CHECK(out == JsonValue(i64{42}));
  REQUIRE(call(context, id, "fraction", &out) == Status::Ok);
  CHECK(out == JsonValue(2.5));
  REQUIRE(call(context, id, "text", &out) == Status::Ok);
  CHECK(out == JsonValue("flee"));
  REQUIRE(call(context, id, "list", &out) == Status::Ok);
  CHECK(render(out) == "[1,2,3]");
  REQUIRE(call(context, id, "record", &out) == Status::Ok);
  CHECK(render(out) == R"({"action":"flee","target":{"urgent":true,"x":1}})");
  REQUIRE(call(context, id, "empty", &out) == Status::Ok);
  CHECK(render(out) == "[]");
  REQUIRE(call(context, id, "vec", &out) == Status::Ok);
  CHECK(render(out) == "[1,2,3]");

  for (const char* bad : {"mixed", "holey", "func", "nan", "cycle"}) {
    INFO(bad);
    CHECK(call(context, id, bad, &out) == Status::InvalidArgument);
    CHECK_FALSE(context.last_error().message.empty());
  }
}

TEST_CASE("scripting arguments: numbers, strings and JSON arrive, and JSON is read-only") {
  ScriptContext context;
  const ScriptId id = must_load(context, "args.luau", R"(
function describe(n, s, data)
  return { n = n * 2, s = s .. "!", first = data.items[1], count = #data.items, flag = data.flag }
end
function mutate(data) data.flag = false end
)");
  JsonValue data = JsonValue::object();
  JsonValue items = JsonValue::array();
  items.push_back(JsonValue("rope"));
  items.push_back(JsonValue("torch"));
  data.set("items", std::move(items));
  data.set("flag", JsonValue(true));
  const Arg args[] = {Arg::number(21), Arg::string("go"), Arg::json(data)};
  JsonValue out;
  REQUIRE(call(context, id, "describe", &out, args) == Status::Ok);
  CHECK(render(out) == R"({"count":2,"first":"rope","flag":true,"n":42,"s":"go!"})");

  const Arg mutate_args[] = {Arg::json(data)};
  CHECK(call(context, id, "mutate", nullptr, mutate_args) == Status::RuntimeError);
  CHECK(context.last_error().message.find("readonly") != std::string::npos);
}

// ---- determinism ------------------------------------------------------------------------------

namespace {

constexpr std::string_view k_deterministic = R"(
function evaluate(npc, seed)
  local rolls = {}
  for i = 0, 4 do rolls[i + 1] = engine.random(seed, i) end
  local tags = {}
  for _, tag in npc.tags do table.insert(tags, string.upper(tag)) end
  table.sort(tags)
  local reputation = 0
  for _, value in npc.reputation do reputation += value end
  return {
    rolls = rolls,
    tags = tags,
    reputation = reputation,
    tick = engine.tick(),
    step = engine.fixed_step(),
    danger = npc.health < 50 and npc.faction ~= "Ironbound",
  }
end
)";

scripting_test::Character sample_character() {
  scripting_test::Character npc;
  npc.name = "Ada";
  npc.health = 42.0f;
  npc.faction = scripting_test::Faction::Drifters;
  npc.tags = {"scout", "archer", "night"};
  npc.reputation.insert_or_assign(std::string("ironbound"), -0.25f);
  npc.reputation.insert_or_assign(std::string("drifters"), 0.75f);
  return npc;
}

std::string run_deterministic(const scripting_test::Character& npc) {
  ScriptContext context;
  context.set_time(1234, 1.0 / 60.0);
  const ScriptId id = must_load(context, "determinism.luau", k_deterministic);
  const Arg args[] = {Arg::object(scripting::object_ref(npc)), Arg::number(99)};
  JsonValue out;
  REQUIRE(call(context, id, "evaluate", &out, args) == Status::Ok);
  return render(out);
}

}  // namespace

TEST_CASE("scripting determinism: the same script over the same document gives the same output") {
  const scripting_test::Character npc = sample_character();
  const std::string first = run_deterministic(npc);
  const std::string second = run_deterministic(npc);
  CHECK(first == second);
  MESSAGE("determinism output: " << first);

  // And within one context, call after call.
  ScriptContext context;
  context.set_time(1234, 1.0 / 60.0);
  const ScriptId id = must_load(context, "determinism.luau", k_deterministic);
  const Arg args[] = {Arg::object(scripting::object_ref(npc)), Arg::number(99)};
  for (int i = 0; i < 3; ++i) {
    JsonValue out;
    REQUIRE(call(context, id, "evaluate", &out, args) == Status::Ok);
    CHECK(render(out) == first);
  }
}

TEST_CASE("scripting determinism: randomness comes only from a seed the caller gave") {
  ScriptContext context;
  const ScriptId id = must_load(context, "rng.luau", R"(
function draw(seed, index) return engine.random(seed, index) end
function bad() return engine.random(-1, 0) end
)");
  auto draw = [&](f64 seed, f64 index) {
    const Arg args[] = {Arg::number(seed), Arg::number(index)};
    JsonValue out;
    REQUIRE(call(context, id, "draw", &out, args) == Status::Ok);
    f64 value = -1;
    REQUIRE(out.get_f64(value));
    return value;
  };
  const f64 a = draw(7, 0);
  CHECK(a >= 0.0);
  CHECK(a < 1.0);
  CHECK(draw(7, 0) == a);  // a pure function of its arguments
  CHECK(draw(7, 1) != a);
  CHECK(draw(8, 0) != a);
  CHECK(call(context, id, "bad") == Status::RuntimeError);
}

// ---- the engine API -----------------------------------------------------------------------------

TEST_CASE("scripting engine API: log, tunable, tick and fixed step") {
  log::RingSink sink(16);
  log::add_sink(&sink);
  ScriptContext context;
  context.set_time(77, 0.5);
  const ScriptId id = must_load(context, "api.luau", R"(
function run()
  engine.log("info", "bridge checked", { intact = true, span = 12.5, name = "old bridge" })
  return { tick = engine.tick(), step = engine.fixed_step(),
           budget = engine.tunable("scripting.step_budget") }
end
function bad_level() engine.log("information", "x") end
function bad_tunable() return engine.tunable("no.such.tunable") end
)");
  const u64 since = sink.next_sequence();
  JsonValue out;
  REQUIRE(call(context, id, "run", &out) == Status::Ok);
  log::remove_sink(&sink);
  CHECK(out.find("tick")->as_int() == 77);
  f64 step = 0;
  REQUIRE(out.find("step")->get_f64(step));
  CHECK(step == 0.5);
  i64 budget = 0;
  REQUIRE(out.find("budget")->get_i64(budget));
  CHECK(budget == 1'000'000);

  bool found = false;
  sink.for_each(since, [&](const log::RingSink::Entry& entry) {
    if (entry.message != "bridge checked") return;
    found = true;
    CHECK(std::string_view(entry.category->name()) == "scripting");
    CHECK(entry.level == log::Level::Info);
    bool script = false;
    bool intact = false;
    bool name = false;
    for (const log::Field& field : entry.fields) {
      if (field.key == "script") script = field.string() == "api.luau";
      if (field.key == "intact") intact = field.kind == log::Field::Kind::Bool && field.value.b;
      if (field.key == "name") name = field.string() == "old bridge";
    }
    CHECK(script);
    CHECK(intact);
    CHECK(name);
  });
  CHECK(found);

  CHECK(call(context, id, "bad_level") == Status::RuntimeError);
  CHECK(context.last_error().message.find("level must be") != std::string::npos);
  CHECK(call(context, id, "bad_tunable") == Status::RuntimeError);
  CHECK(context.last_error().message.find("no tunable") != std::string::npos);
}

TEST_CASE("scripting limits come from the tunables") {
  const scripting::ScriptLimits limits = scripting::ScriptLimits::from_tunables();
  CHECK(limits.memory_bytes == (u64{64} << 20));
  CHECK(limits.step_budget == 1'000'000);
  CHECK(limits.time_budget_ns == 0);
}

// ---- the tick driver ----------------------------------------------------------------------------

TEST_CASE("scripting system ticks every near instance under its own budget") {
  ScriptContext context;
  const ScriptId id = must_load(context, "npc.luau", R"(
function on_tick(self, dt)
  if self.name == "broken" then error("routine failed") end
  if self.health < 50 then return { flee = self.name, dt = dt } end
  return nil
end
)");
  scripting::FunctionId on_tick;
  REQUIRE(context.find_function(id, "on_tick", on_tick) == Status::Ok);

  scripting_test::Character npcs[4];
  npcs[0].name = "calm";
  npcs[1].name = "hurt";
  npcs[1].health = 10.0f;
  npcs[2].name = "broken";
  npcs[3].name = "far";
  npcs[3].health = 1.0f;

  scripting::ScriptingSystem system(context);
  for (const auto& npc : npcs)
    system.add(on_tick, context.bind(scripting::object_ref(npc)));
  system.set_tier(3, scripting::ScriptingSystem::lod_tier(10.0f));

  system.begin_tick(5, 1.0 / 60.0);
  system.tick(0.25f);
  system.end_tick();
  CHECK(system.tick_index() == 5u);
  CHECK(system.calls_last_tick() == 3u);  // the far instance is dormant
  CHECK(system.failures_last_tick() == 1u);
  REQUIRE(system.results().size() == 1u);
  CHECK(system.results()[0].instance == 1u);
  CHECK(render(system.results()[0].value) == R"({"dt":0.25,"flee":"hurt"})");
}

TEST_CASE("scripting system: a steady-state tick allocates nothing, on either side") {
  // The VM's allocator is the engine heap, so mem::allocation_counter() sees the VM's allocations
  // as well as the host's: a tick of routines that read properties and return nil costs zero of
  // either once the views and the argument slots exist.
  ScriptContext context;
  const ScriptId id = must_load(context, "steady.luau", R"(
function on_tick(self, dt)
  if self.health < 50 and self.faction ~= "Ironbound" and self.position.y < 0 then
    return { flee = true }
  end
  return nil
end
)");
  scripting::FunctionId on_tick;
  REQUIRE(context.find_function(id, "on_tick", on_tick) == Status::Ok);
  scripting_test::Character npcs[64];
  scripting::ScriptingSystem system(context);
  for (auto& npc : npcs)
    system.add(on_tick, context.bind(scripting::object_ref(npc)));
  for (u64 tick = 0; tick < 8; ++tick) {
    system.begin_tick(tick, 1.0 / 60.0);
    system.tick(1.0f / 60.0f);
  }
  const u64 before = mem::allocation_counter();
  for (u64 tick = 8; tick < 108; ++tick) {
    system.begin_tick(tick, 1.0 / 60.0);
    system.tick(1.0f / 60.0f);
    system.end_tick();
  }
  CHECK(mem::allocation_counter() - before == 0u);
  CHECK(system.failures_last_tick() == 0u);
  CHECK(system.calls_last_tick() == 64u);
}

TEST_CASE("scripting LOD policy never coarsens as the observer gets nearer") {
  using System = scripting::ScriptingSystem;
  CHECK(System::lod_tier(0.0f) <= System::lod_tier(10.0f));
  CHECK(System::lod_tier(10.0f) <= System::lod_tier(1000.0f));
  CHECK(System::lod_tier(0.0f) == 0u);
}

TEST_CASE("scripting declares a determinism stance") {
  const std::string_view determinism{scripting::k_determinism};
  CHECK(determinism == "hashed");
}

TEST_CASE("scripting context refuses a call re-entered from inside a call") {
  struct Host {
    ScriptContext* context = nullptr;
    ScriptId script;
    Status inner = Status::Ok;
    scripting_test::Item item;
  };
  Host host;
  ScriptContext context;
  host.context = &context;
  host.script = must_load(context, "reenter.luau", R"(
function outer() return engine.object("0123456789abcdef0123456789abcdef") ~= nil end
function inner() return 1 end
)");
  context.set_resolver(
      scripting::ObjectResolver{&host, [](void* user, const Id128&, scripting::ObjectRef& out) {
                                  auto* h = static_cast<Host*>(user);
                                  h->inner = h->context->call(h->script, "inner");
                                  out = scripting::object_ref(h->item);
                                  return true;
                                }});
  JsonValue out;
  REQUIRE(call(context, host.script, "outer", &out) == Status::Ok);
  CHECK(out == JsonValue(true));
  CHECK(host.inner == Status::InvalidArgument);
}
