// Hot reload: an edited script file is recompiled and swapped between calls, and a save that does
// not compile (or whose top level fails) keeps the last good chunk — the way gfx::ShaderLibrary
// treats a shader save (docs/subsystems/scripting.md, "Hot reload").
#include <foundation/scripting/script_context.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace engine;
using scripting::ScriptContext;
using scripting::ScriptId;
using scripting::Status;

namespace {

// Writes `text` and moves the file's modification time on by `seconds`, so a poll sees the edit
// even when two writes land inside one tick of the file system's clock.
void write_script(const std::filesystem::path& path, std::string_view text, int seconds) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  std::error_code ignored;
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(path, now + std::chrono::seconds(seconds), ignored);
}

i64 value_of(ScriptContext& context, scripting::FunctionId fn) {
  JsonValue out;
  const Status status = context.call(fn, {}, &out);
  INFO(context.last_error().message);
  REQUIRE(status == Status::Ok);
  i64 value = 0;
  REQUIRE(out.get_i64(value));
  return value;
}

}  // namespace

TEST_CASE("scripting hot reload: an edited file is recompiled and swapped between calls") {
  engine::test::TempDir dir("scripting-reload");
  REQUIRE(dir.ok());
  const std::filesystem::path file = dir.native_file("value.luau");
  write_script(file, "function value() return 1 end\n", 0);

  ScriptContext context;
  ScriptId id;
  REQUIRE(context.load_file(dir.file("value.luau"), id) == Status::Ok);
  scripting::FunctionId value;
  REQUIRE(context.find_function(id, "value", value) == Status::Ok);
  CHECK(value_of(context, value) == 1);
  CHECK(context.poll_changes() == 0u);  // nothing changed yet
  CHECK(context.generation(id) == 1u);

  write_script(file, "function value() return 2 end\n", 2);
  Vector<ScriptId> reloaded;
  CHECK(context.poll_changes(&reloaded) == 1u);
  REQUIRE(reloaded.size() == 1u);
  CHECK(reloaded[0] == id);
  CHECK(context.generation(id) == 2u);
  CHECK(value_of(context, value) == 2);  // the same FunctionId, the new chunk

  SUBCASE("a compile error keeps the last good chunk, and is reported once") {
    write_script(file, "function value()\n  return 3 +\nend\n", 4);
    CHECK(context.poll_changes() == 0u);
    CHECK(context.last_error().status == Status::CompileError);
    CHECK(context.last_error().chunk == dir.file("value.luau"));
    CHECK(context.last_error().line >= 2);
    CHECK(value_of(context, value) == 2);
    CHECK(context.generation(id) == 2u);
    CHECK(context.poll_changes() == 0u);  // the same broken save is not tried again

    write_script(file, "function value() return 4 end\n", 6);
    CHECK(context.poll_changes() == 1u);
    CHECK(value_of(context, value) == 4);
  }

  SUBCASE("a top level that fails keeps the last good chunk") {
    write_script(file, "error('not ready')\nfunction value() return 5 end\n", 4);
    CHECK(context.poll_changes() == 0u);
    CHECK(context.last_error().status == Status::RuntimeError);
    CHECK(context.last_error().line == 1);
    CHECK(value_of(context, value) == 2);
  }

  SUBCASE("a file that disappears for a moment keeps the chunk") {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    CHECK(context.poll_changes() == 0u);
    CHECK(value_of(context, value) == 2);
  }

  SUBCASE("a save with the same bytes does not reload") {
    write_script(file, "function value() return 2 end\n", 4);
    CHECK(context.poll_changes() == 0u);
    CHECK(context.generation(id) == 2u);
  }
}

TEST_CASE("scripting reload: a function the new chunk drops calls as NotFound until it returns") {
  ScriptContext context;
  ScriptId id;
  REQUIRE(context.load("quest.luau", "function evaluate() return 1 end", id) == Status::Ok);
  scripting::FunctionId evaluate;
  REQUIRE(context.find_function(id, "evaluate", evaluate) == Status::Ok);

  REQUIRE(context.reload(id, "function other() return 0 end") == Status::Ok);
  CHECK(context.call(evaluate) == Status::NotFound);
  CHECK(context.last_error().message.find("evaluate") != std::string::npos);

  REQUIRE(context.reload(id, "function evaluate() return 7 end") == Status::Ok);
  CHECK(value_of(context, evaluate) == 7);
  CHECK(context.generation(id) == 3u);

  CHECK(context.reload(id, "function evaluate( return 8 end") == Status::CompileError);
  CHECK(value_of(context, evaluate) == 7);
}

TEST_CASE("scripting reload: a missing file is an IoError and loads nothing") {
  engine::test::TempDir dir("scripting-missing");
  REQUIRE(dir.ok());
  ScriptContext context;
  ScriptId id;
  CHECK(context.load_file(dir.file("absent.luau"), id) == Status::IoError);
  CHECK_FALSE(id.valid());
}
