// End to end: the console (docs/subsystems/apps.md, "The console"; plan 06 §6.3). A Luau script
// drives one engine-host through the protocol — `engine-host --console`, and engine-cli's one-shot
// `--console <file>` over it — and every call is dispatched as a request on the wire would be. The
// scripts are content/console/'s, and each asserts its own results in Luau, so these cases read
// the exit code and the lines the script printed.
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace {

const std::string& cli_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}
const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
}

std::string script(std::string_view name) {
  const std::string dir = test::data_path(ENGINE_SOURCE_DIR "/content/console", "content/console");
  return dir + "/" + std::string(name);
}
std::string roles_file() {
  return test::data_path(ENGINE_SOURCE_DIR "/content/roles", "content/roles") + "/roles.json";
}

struct Run {
  i32 exit_code = -1;
  std::string out;  // stdout and stderr together, in the order they were written
};

// Runs a process to its end with `input` on stdin, capturing stdout and stderr in one stream.
Run run(std::vector<std::string> args, std::string_view input = {}) {
  std::vector<std::string_view> argv(args.begin(), args.end());
  platform::Process p;
  std::string error;
  Run r;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
               /*merge_stderr=*/true)) {
    FAIL("cannot spawn " << args[0] << ": " << error);
    return r;
  }
  if (!input.empty()) CHECK(p.write(input));
  p.close_stdin();
  p.read_all(r.out);
  r.exit_code = p.wait();
  return r;
}

bool contains(const std::string& text, std::string_view what) {
  return text.find(what) != std::string::npos;
}

}  // namespace

#if defined(ENGINE_CLI_TESTS_SCRIPTING)

TEST_CASE("console: a script edits a document through engine-cli's one-shot form") {
  const test::TempDir tmp("engine_cli_console");
  const std::string doc = tmp.file("world");
  // Create, apply two transactions with rationales, read the journal, undo one, list objects —
  // asserted in Luau (content/console/edit_session.luau). One line, as an agent types it.
  const Run r = run({cli_exe(), "--host", host_exe(), "--actor", "console-e2e", "--console",
                     script("edit_session.luau"), "--", doc});
  INFO(r.out);
  REQUIRE(r.exit_code == 0);
  CHECK(contains(r.out, "objects\t1\n"));
  CHECK(contains(r.out, "journal\t2\tposition\t1\n"));
  // What the script did is on disk, as any client's would be: the journal holds both patches.
  CHECK(std::filesystem::exists(std::filesystem::path(doc) / "journal.jsonl"));

  // The same document read again by a second script, from stdin through `-`: the session is a
  // new one, the journal the same.
  const Run again = run({cli_exe(), "--host", host_exe(), "--console", "-", "--", doc},
                        "local dir = ...\n"
                        "local s = engine.session.open({ path = dir }).session\n"
                        "local j = engine.doc.journal({ session = s })\n"
                        "print(j.total, j.position, engine.ping().pid > 0)\n");
  INFO(again.out);
  REQUIRE(again.exit_code == 0);
  CHECK(contains(again.out, "2\t1\ttrue\n"));
}

TEST_CASE("console: engine-host runs a script from stdin, and an error names the chunk and line") {
  // The catalogue reads like the protocol's: engine.methods() is "engine.methods".
  const Run listed = run({host_exe(), "--console"},
                         "local m = engine.methods()\n"
                         "local names = {}\n"
                         "for _, method in ipairs(m.methods) do names[method.name] = true end\n"
                         "assert(names['doc.apply'] and names['render.capture'])\n"
                         "assert(type(engine.doc.apply) == 'function')\n"
                         "assert(type(engine.render.capture) == 'function')\n"
                         "print(engine.json({ a = 1 }))\n");
  INFO(listed.out);
  REQUIRE(listed.exit_code == 0);
  CHECK(contains(listed.out, "{\"a\":1}\n"));

  // An uncaught error is exit 1 with "<chunk>:<line>: <message>" on stderr.
  const Run failed = run({host_exe(), "--console"}, "print('one')\nerror('two')\n");
  CHECK(failed.exit_code == 1);
  CHECK(contains(failed.out, "one\n"));
  CHECK(contains(failed.out, "stdin:2: two"));

  // So is a compile error, which runs nothing.
  const Run broken = run({host_exe(), "--console"}, "print('never')\nlocal x = = 1\n");
  CHECK(broken.exit_code == 1);
  CHECK(!contains(broken.out, "never"));
  CHECK(contains(broken.out, "stdin:2:"));

  // A script that does not read, and console flags without --console, are usage errors.
  CHECK(run({host_exe(), "--console", "--script", "no/such/script.luau"}).exit_code == 2);
  CHECK(run({host_exe(), "--script", script("runaway.luau")}).exit_code == 2);
}

TEST_CASE("console: a params table is read through the method's schema type") {
  const test::TempDir tmp("engine_cli_console");
  // `channels = {}` is an empty array to render.capture and `properties = {}` an empty map to an
  // ObjectRecord: Luau cannot tell the two apart, the descriptors can. A params type's own reader
  // reports a field of the wrong type in the protocol's words (-32602), and that reaches pcall.
  const Run r = run({host_exe(), "--console", "--", tmp.file("world")},
                    "local dir = ...\n"
                    "local s = engine.session.open({ path = dir, create = true }).session\n"
                    "local id = '00000000000000100000000000000003'\n"
                    "local applied = engine.doc.apply({ session = s, attribution = { rationale = "
                    "'typed' }, commands = { { kind = 'RestoreRecord', id = id, record = { id = "
                    "id, type = 'engine.content.AssetProvenance', properties = {} } } } })\n"
                    "assert(applied.committed, engine.json(applied))\n"
                    "local ok, err = pcall(engine.doc.objects, { session = s, limit = 'many' })\n"
                    "assert(not ok and err.code == -32602, tostring(err))\n"
                    "local ok2, err2 = pcall(engine.doc.objects, { session = s, limit = print })\n"
                    "assert(not ok2 and string.find(err2, 'limit'), tostring(err2))\n"
                    "print('typed', engine.doc.objects({ session = s }).total)\n");
  INFO(r.out);
  REQUIRE(r.exit_code == 0);
  CHECK(contains(r.out, "typed\t1\n"));
}

TEST_CASE("console: a role that may not write is refused, and the refusal reaches pcall") {
  const test::TempDir tmp("engine_cli_console");
  const std::string doc = tmp.file("world");
  const std::string roles = roles_file();
  if (!test::path_exists(roles)) {
    MESSAGE("not in this bundle: " << roles);
    return;
  }
  // qa may read everything and change nothing (content/roles/roles.json).
  const Run caught = run({host_exe(), "--roles", roles, "--role", "qa", "--console", "--script",
                          script("forbidden_write.luau"), "--", doc});
  INFO(caught.out);
  REQUIRE(caught.exit_code == 0);
  CHECK(contains(caught.out, "refused\t1008\tmethod\n"));

  // Uncaught, the same refusal ends the run at the line of the call, in the protocol's words.
  const Run uncaught = run({host_exe(), "--roles", roles, "--role", "qa", "--console", "--script",
                            script("forbidden_write.luau"), "--", doc, "uncaught"});
  INFO(uncaught.out);
  CHECK(uncaught.exit_code == 1);
  CHECK(contains(uncaught.out, "forbidden_write.luau:30: doc.apply: error 1008:"));
  CHECK(!contains(uncaught.out, "unreachable"));
}

TEST_CASE("console: the step budget stops a script that never ends") {
  const Run r = run({cli_exe(), "--host", host_exe(), "--step-budget", "100000", "--console",
                     script("runaway.luau")});
  INFO(r.out);
  CHECK(r.exit_code == 1);
  CHECK(contains(r.out, "looping\n"));
  CHECK(contains(r.out, "runaway.luau:"));
  CHECK(contains(r.out, "step budget"));
  CHECK(contains(r.out, "--step-budget"));
}

TEST_CASE("console: a frame captured through engine.render.capture") {
  const test::TempDir tmp("engine_cli_console");
  const std::string out_dir = tmp.file("captures");
  const Run r = run({host_exe(), "--console", "--script", script("capture.luau"), "--", out_dir});
  INFO(r.out);
  REQUIRE(r.exit_code == 0);
  if (contains(r.out, "render unavailable")) {
    MESSAGE("skipped: this machine cannot render (" << r.out << ")");
    return;
  }
  CHECK(contains(r.out, "color\t"));
  CHECK(std::filesystem::exists(std::filesystem::path(out_dir) / "console.png"));
}

#else

TEST_CASE("console: a host without the scripting capability says so and exits 2") {
  const Run r = run({host_exe(), "--console"}, "print('never')\n");
  CHECK(r.exit_code == 2);
  CHECK(contains(r.out, "ENGINE_WITH_SCRIPTING"));
  CHECK(!contains(r.out, "never"));
}

#endif
