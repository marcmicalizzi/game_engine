// ScriptContext::run_program (docs/subsystems/scripting.md, "Programs: the console") against a
// fake host that records what it was asked: the params a table becomes through the method's schema
// type, the error table pcall returns, the names the methods get, print, the arguments, and the
// one step budget a whole program runs under. engine-host's console is this over the protocol's
// dispatcher, and apps/engine_cli/tests/console_tests.cpp drives that end to end.
#include <core/json/json.h>
#include <foundation/scripting/script_context.h>

#include <doctest/doctest.h>

#include <schemas/scripting_test.h>
#include <string>
#include <string_view>

using namespace engine;
using scripting::Status;

namespace {

struct FakeHost {
  Vector<std::string> calls;
  Vector<JsonValue> params;
  std::string printed;
};

// "test.fail" fails as the protocol's role gate would; everything else echoes its params.
bool fake_call(void* user, std::string_view method, const JsonValue& params, JsonValue& result,
               scripting::HostError& error) {
  FakeHost& host = *static_cast<FakeHost*>(user);
  host.calls.push_back(std::string(method));
  host.params.push_back(params);
  if (method == "test.fail") {
    error.code = 1008;
    error.message = "refused";
    error.data = JsonValue::object();
    error.data.set("reason", JsonValue("method"));
    return false;
  }
  result = JsonValue::object();
  result.set("echo", params);
  result.set("method", JsonValue(method));
  result.set("empty", JsonValue(std::string_view()));  // a null view: pushed as ""
  if (method == "test.deep") {
    // Forty levels of objects and arrays: more than the twenty stack slots a C function is given,
    // which is what a method's result is pushed from.
    JsonValue deep = JsonValue("bottom");
    for (int level = 0; level < 40; ++level) {
      JsonValue wrapper = (level % 2) == 0 ? JsonValue::array() : JsonValue::object();
      if (wrapper.is_array()) {
        wrapper.push_back(std::move(deep));
      } else {
        wrapper.set("next", std::move(deep));
      }
      deep = std::move(wrapper);
    }
    result.set("deep", std::move(deep));
  }
  return true;
}

void fake_print(void* user, std::string_view text) {
  static_cast<FakeHost*>(user)->printed.append(text.data(), text.size());
}

const scripting::HostMethod k_methods[] = {
    {"test.character", &schema::type_of<scripting_test::Character>()},
    {"test.fail", nullptr},
    {"engine.ping", nullptr},
    // Taken places: a namespace where `test.character` is already a function, and `engine.call`.
    {"test.character.nested", nullptr},
    {"engine.call", nullptr},
};

struct Outcome {
  Status status = Status::Ok;
  scripting::ScriptError error;
};

Outcome run(FakeHost& fake, std::string_view source, std::span<const std::string_view> args = {},
            const scripting::ScriptLimits& limits = scripting::ScriptLimits{}) {
  scripting::ScriptContext context(limits);
  const scripting::ProgramHost host{&fake, k_methods, &fake_call, &fake_print};
  Outcome out;
  out.status = context.run_program("program.luau", source, host, args);
  out.error = context.last_error();
  return out;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "no '" << key << "' in " << write_json(o));
  return *v;
}

}  // namespace

TEST_CASE("programs: a params table becomes JSON through the method's schema type") {
  FakeHost fake;
  const Outcome r = run(fake,
                        "engine.test.character({ name = 'Ash', tags = {}, reputation = {}, "
                        "attributes = {}, health = 50, level = 3 })\n"
                        "engine.call('test.character', { tags = {} })\n"
                        "engine.call('test.untyped', { a = {} })\n");
  INFO(r.error.line << ": " << r.error.message);
  REQUIRE(r.status == Status::Ok);
  REQUIRE(fake.params.size() == 3);
  const JsonValue& typed = fake.params[0];
  // Luau's one empty table, read three ways by the fields it fills.
  CHECK(at(typed, "tags").is_array());
  CHECK(at(typed, "reputation").is_object());
  CHECK(at(typed, "attributes").is_object());
  // A float field stays a float, an integer field an integer.
  CHECK(at(typed, "health").kind() == JsonValue::Kind::Float);
  CHECK(at(typed, "level").kind() == JsonValue::Kind::Int);
  CHECK(at(typed, "name") == JsonValue("Ash"));
  // engine.call by name finds the same type; a method the host did not list is read untyped,
  // where an empty table is the empty array.
  CHECK(at(fake.params[1], "tags").is_array());
  CHECK(fake.calls[2] == "test.untyped");
  CHECK(at(fake.params[2], "a").is_array());
}

TEST_CASE("programs: results are the program's tables, and a host error is a table pcall returns") {
  FakeHost fake;
  const Outcome r = run(fake,
                        "local r = engine.test.character({ name = 'x' })\n"
                        "assert(r.echo.name == 'x' and r.method == 'test.character')\n"
                        "assert(r.empty == '')\n"
                        "r.extra = 1\n"
                        "r.echo.name = 'y'\n"
                        "local ok, err = pcall(engine.test.fail, {})\n"
                        "assert(not ok)\n"
                        "assert(err.code == 1008 and err.message == 'refused')\n"
                        "assert(err.method == 'test.fail' and err.data.reason == 'method')\n"
                        "assert(tostring(err) == 'test.fail: error 1008: refused', tostring(err))\n"
                        "print('ok', 1, true, nil)\n"
                        "print()\n"
                        "print(engine.json({ 1, 2 }), engine.json('s'))\n");
  INFO(r.error.line << ": " << r.error.message);
  REQUIRE(r.status == Status::Ok);
  CHECK(fake.printed == "ok\t1\ttrue\tnil\n\n[1,2]\t\"s\"\n");
}

TEST_CASE("programs: a deeply nested result arrives whole") {
  FakeHost fake;
  const Outcome r = run(fake,
                        "local v = engine.call('test.deep').deep\n"
                        "local depth = 0\n"
                        "while type(v) == 'table' do\n"
                        "  v = v[1] or v.next\n"
                        "  depth = depth + 1\n"
                        "end\n"
                        "assert(depth == 40 and v == 'bottom', depth)\n");
  INFO(r.error.line << ": " << r.error.message);
  CHECK(r.status == Status::Ok);
}

TEST_CASE("programs: an uncaught host error ends the run at the line that made the call") {
  FakeHost fake;
  const Outcome r = run(fake, "print('before')\n\nengine.test.fail({})\nprint('after')\n");
  CHECK(r.status == Status::RuntimeError);
  CHECK(r.error.chunk == "program.luau");
  CHECK(r.error.line == 3);
  CHECK(r.error.message == "test.fail: error 1008: refused {\"reason\":\"method\"}");
  CHECK(fake.printed == "before\n");

  // A table a script raises itself is reported as its JSON, not as "not a string".
  FakeHost other;
  const Outcome raised = run(other, "error({ why = 'mine' })\n");
  CHECK(raised.status == Status::RuntimeError);
  CHECK(raised.error.message == "error {\"why\":\"mine\"}");
}

TEST_CASE("programs: every method is a function named after it, unless its place is taken") {
  FakeHost fake;
  const Outcome r = run(fake,
                        "assert(type(engine.test.character) == 'function')\n"
                        "assert(type(engine.test.fail) == 'function')\n"
                        "assert(type(engine.ping) == 'function' and engine.engine == nil)\n"
                        "assert(engine.ping().method == 'engine.ping')\n"
                        "assert(engine.call('engine.call').method == 'engine.call')\n"
                        "assert(engine.call('test.character.nested').method == "
                        "'test.character.nested')\n"
                        "assert(engine.tick == nil and engine.object == nil)\n");
  INFO(r.error.line << ": " << r.error.message);
  REQUIRE(r.status == Status::Ok);
  REQUIRE(fake.calls.size() == 3);
  CHECK(fake.calls[0] == "engine.ping");
  CHECK(fake.calls[1] == "engine.call");
  CHECK(fake.calls[2] == "test.character.nested");
}

TEST_CASE("programs: arguments are `...`, globals are the program's, and the sandbox holds") {
  FakeHost fake;
  const std::string_view args[] = {"first", "second"};
  const Outcome r = run(fake,
                        "local a, b = ...\n"
                        "assert(a == 'first' and b == 'second')\n"
                        "counter = 1\n"
                        "counter = counter + 1\n"
                        "assert(counter == 2)\n"
                        "assert(io == nil and os == nil and debug == nil)\n"
                        "assert(loadstring == nil and require == nil and getfenv == nil)\n",
                        args);
  INFO(r.error.line << ": " << r.error.message);
  CHECK(r.status == Status::Ok);
}

TEST_CASE("programs: a value with no JSON form is refused before the host sees it") {
  FakeHost fake;
  const Outcome r = run(fake, "engine.test.character({ name = 'x', tags = { 'a', print } })\n");
  CHECK(r.status == Status::RuntimeError);
  CHECK(r.error.line == 1);
  CHECK(r.error.message.find("test.character: params: tags[2]: a function has no JSON form") !=
        std::string::npos);
  CHECK(fake.calls.empty());

  FakeHost bad_params;
  const Outcome wrong = run(bad_params, "engine.test.character('a string')\n");
  CHECK(wrong.status == Status::RuntimeError);
  CHECK(bad_params.calls.empty());
}

TEST_CASE("programs: one step budget for the whole run, which pcall cannot outlive") {
  scripting::ScriptLimits limits;
  limits.step_budget = 10'000;
  FakeHost fake;
  const Outcome r = run(fake,
                        "while true do\n"
                        "  pcall(function() engine.test.character({}) end)\n"
                        "end\n",
                        {}, limits);
  CHECK(r.status == Status::BudgetExceeded);
  CHECK(r.error.message.find("step budget") != std::string::npos);
  // Each iteration costs a few steps and makes a call, so the budget was spent across calls, not
  // reset by them.
  CHECK(!fake.calls.empty());
  CHECK(fake.calls.size() < 10'000);
}
