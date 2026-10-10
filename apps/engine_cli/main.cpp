// engine-cli (docs/plan/02-architecture.md §2.2): the thin scriptable client. Spawns
// engine-host, optionally opens a document, sends one method call, and prints the result.
// Because sessions persist their journal and undo position on disk, a sequence of separate
// invocations behaves like one editing session. `--console <file>` runs a Luau script against one
// host instead (engine-host --console; docs/subsystems/apps.md, "The console").
#include <core/json/json.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>

#include <cstdio>
#include <ctime>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#if ENGINE_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

using namespace engine;

namespace {

const char* k_usage =
    "usage: engine-cli [options] <method> [params-json]\n"
    "       engine-cli [options] --console <file.luau | -> [-- <script args>...]\n"
    "\n"
    "  --host <path>      engine-host executable (default: beside engine-cli)\n"
    "  --doc <dir>        open this document first and pass its session to the method\n"
    "  --create           with --doc: create the document when it does not exist\n"
    "  --name <name>      with --create: the document's name\n"
    "  --mount <spec>     forwarded to engine-host (<scheme>=<dir>[:rw]); repeatable\n"
    "  --roles <file>     forwarded to engine-host: the role configurations calls are checked\n"
    "                     against (content/roles/roles.json)\n"
    "  --actor, --role, --task <value>\n"
    "                     forwarded to engine-host: who a call that names nobody is\n"
    "  --compact          print the result on one line\n"
    "  --report <file>    also write the result to this file as one JSON document, with the\n"
    "                     method and a UTC timestamp around it: a file to send back\n"
    "\n"
    "  --console <file>   run a Luau script against one engine-host (engine-host --console):\n"
    "                     engine.doc.apply{...}, engine.call(method, params), print to stdout;\n"
    "                     '-' reads the script from stdin. The exit code is the host's: 0 the\n"
    "                     script ended, 1 it failed (chunk:line on stderr), 2 it could not start\n"
    "  --step-budget <n>, --memory-mb <n>\n"
    "                     with --console: the script's limits, forwarded to engine-host\n"
    "  -- <args>          with --console: the script's `...`\n"
    "\n"
    "examples:\n"
    "  engine-cli engine.methods\n"
    "  engine-cli gpu.adapters --report adapters.json\n"
    "  engine-cli --doc ./world --create session.info\n"
    "  engine-cli --doc ./world doc.apply "
    "'{\"commands\":[...],\"attribution\":{\"actor\":\"me\"}}'\n"
    "  engine-cli --doc ./world doc.undo\n"
    "  engine-cli --console edit.luau -- ./world\n";

struct Client {
  platform::Process host;
  u32 next_id = 1;

  // Returns false on transport failure or an error response (already printed to stderr).
  bool call(std::string_view method, JsonValue params, JsonValue& result) {
    JsonValue request = JsonValue::object();
    request.set("jsonrpc", JsonValue("2.0"));
    request.set("id", JsonValue(next_id++));
    request.set("method", JsonValue(method));
    if (!params.is_null()) request.set("params", std::move(params));
    std::string line = write_json(request, JsonWriteOptions{.pretty = false});
    line.push_back('\n');
    if (!host.write(line)) {
      std::fprintf(stderr, "engine-cli: cannot write to engine-host\n");
      return false;
    }
    std::string response_text;
    if (!host.read_line(response_text)) {
      std::fprintf(stderr, "engine-cli: engine-host closed the connection\n");
      return false;
    }
    JsonValue response;
    const JsonParseResult parsed = parse_json(response_text, response);
    if (!parsed.ok) {
      std::fprintf(stderr, "engine-cli: unreadable response: %s\n%s\n", parsed.message,
                   response_text.c_str());
      return false;
    }
    if (const JsonValue* error = response.find("error"); error != nullptr) {
      const JsonValue* code = error->find("code");
      const JsonValue* message = error->find("message");
      std::fprintf(stderr, "error %lld: %s\n",
                   code != nullptr ? static_cast<long long>(code->as_int()) : 0ll,
                   message != nullptr ? std::string(message->as_string()).c_str() : "");
      if (const JsonValue* data = error->find("data"); data != nullptr && !data->is_null()) {
        const std::string text = write_json(*data);
        std::fprintf(stderr, "%s\n", text.c_str());
      }
      return false;
    }
    const JsonValue* r = response.find("result");
    result = r != nullptr ? *r : JsonValue();
    return true;
  }
};

// "2026-09-19T07:54:11Z", or an empty string if the clock cannot be read. The report is a file
// somebody mails back weeks later, so it says when it was taken.
std::string utc_now() {
  const std::time_t now = std::time(nullptr);
  if (now == static_cast<std::time_t>(-1)) return {};
  std::tm parts{};
#if defined(_WIN32)
  if (gmtime_s(&parts, &now) != 0) return {};
#else
  if (gmtime_r(&now, &parts) == nullptr) return {};
#endif
  char text[32];
  if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts) == 0) return {};
  return text;
}

// One JSON document: what was asked, when, and the answer. Generic on purpose — any method's
// result can be sent back this way — and the one it exists for is `gpu.adapters`, whose result
// carries the whole requirements table and a verdict per device on the machine.
bool write_report(const std::string& path, std::string_view method, const JsonValue& result) {
  JsonValue envelope = JsonValue::object();
  envelope.set("tool", JsonValue("engine-cli"));
  envelope.set("method", JsonValue(method));
  const std::string when = utc_now();
  if (!when.empty()) envelope.set("generated_utc", JsonValue(when));
  envelope.set("result", result);
  std::string text = write_json(envelope, JsonWriteOptions{.pretty = true});
  text.push_back('\n');
  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, path.c_str(), "wb") != 0) file = nullptr;
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    std::fprintf(stderr, "engine-cli: cannot write report to '%s'\n", path.c_str());
    return false;
  }
  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed) {
    std::fprintf(stderr, "engine-cli: report to '%s' was not fully written\n", path.c_str());
    return false;
  }
  return true;
}

// --console: one engine-host running the script (engine-host --console; apps.md, "The console").
// Its stdout is forwarded a line at a time as the script prints, its stderr is inherited, and its
// exit code is this process's — so what an agent types is one line with a file, and what it reads
// back is the script's own output and the host's verdict. `-` hands this process's stdin over.
int run_console(const Vector<std::string_view>& host_argv, const std::string& script,
                const Vector<std::string>& script_args) {
  Vector<std::string_view> argv;
  for (const std::string_view a : host_argv) {
    if (a != "--stdio") argv.push_back(a);
  }
  argv.push_back("--console");
  const bool from_stdin = script == "-";
  if (!from_stdin) {
    argv.push_back("--script");
    argv.push_back(script);
  }
  if (!script_args.empty()) {
    argv.push_back("--");
    for (const std::string& a : script_args)
      argv.push_back(a);
  }
  platform::Process host;
  std::string error;
  if (!host.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    std::fprintf(stderr, "engine-cli: cannot start engine-host at '%s': %s\n",
                 std::string(argv[0]).c_str(), error.c_str());
    return 2;
  }
  if (from_stdin) {
    const std::string source{std::istreambuf_iterator<char>(std::cin),
                             std::istreambuf_iterator<char>()};
    if (!host.write(source)) std::fputs("engine-cli: cannot write to engine-host\n", stderr);
  }
  host.close_stdin();
#if ENGINE_PLATFORM_WINDOWS
  // The script's lines are forwarded as the host wrote them, without a CR the C runtime would add.
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  std::string line;
  while (host.read_line(line)) {
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
  }
  return host.wait();
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();  // ADR-0031, first statement
  std::string host_path;
  std::string doc_dir;
  std::string doc_name;
  std::string report_path;
  bool create = false;
  bool compact = false;
  Vector<std::string> mounts;
  std::string method;
  std::string params_text;
  // Forwarded to engine-host as they are: the identity and roles in both forms, the console's
  // own flags in the console form.
  Vector<std::string> forwarded;
  std::string console_script;
  bool console = false;
  bool console_only_flag = false;
  Vector<std::string> script_args;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "engine-cli: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (a == "--help" || a == "-h") {
      std::fputs(k_usage, stdout);
      return 0;
    } else if (a == "--host") {
      if (!value(host_path)) return 2;
    } else if (a == "--doc") {
      if (!value(doc_dir)) return 2;
    } else if (a == "--name") {
      if (!value(doc_name)) return 2;
    } else if (a == "--report") {
      if (!value(report_path)) return 2;
    } else if (a == "--mount") {
      std::string m;
      if (!value(m)) return 2;
      mounts.push_back(m);
    } else if (a == "--roles" || a == "--actor" || a == "--role" || a == "--task") {
      std::string v;
      if (!value(v)) return 2;
      forwarded.push_back(std::string(a));
      forwarded.push_back(v);
    } else if (a == "--step-budget" || a == "--memory-mb") {
      std::string v;
      if (!value(v)) return 2;
      forwarded.push_back(std::string(a));
      forwarded.push_back(v);
      console_only_flag = true;
    } else if (a == "--console") {
      if (!value(console_script)) return 2;
      console = true;
    } else if (a == "--") {
      for (++i; i < argc; ++i)
        script_args.push_back(argv[i]);
      console_only_flag = true;
    } else if (a == "--create") {
      create = true;
    } else if (a == "--compact") {
      compact = true;
    } else if (!a.empty() && a[0] == '-' && method.empty()) {
      std::fprintf(stderr, "engine-cli: unknown option '%s'\n%s", argv[i], k_usage);
      return 2;
    } else if (method.empty()) {
      method = std::string(a);
    } else if (params_text.empty()) {
      params_text = std::string(a);
    } else {
      std::fprintf(stderr, "engine-cli: unexpected argument '%s'\n%s", argv[i], k_usage);
      return 2;
    }
  }
  if (console_only_flag && !console) {
    std::fputs("engine-cli: --step-budget, --memory-mb and -- need --console\n", stderr);
    return 2;
  }
  if (console && (!method.empty() || !doc_dir.empty() || !report_path.empty())) {
    // The script opens its documents and prints what it wants kept; pass a path as an argument.
    std::fputs(
        "engine-cli: --console runs a script instead of one method: no method, --doc or "
        "--report beside it (pass a document's path to the script after --)\n",
        stderr);
    return 2;
  }
  if (method.empty() && !console) {
    std::fputs(k_usage, stderr);
    return 2;
  }

  JsonValue params;
  if (!params_text.empty()) {
    const JsonParseResult parsed = parse_json(params_text, params);
    if (!parsed.ok) {
      std::fprintf(stderr, "engine-cli: params are not valid JSON: %s (line %u, column %u)\n",
                   parsed.message, parsed.line, parsed.column);
      return 2;
    }
    if (!params.is_object()) {
      std::fprintf(stderr, "engine-cli: params must be a JSON object\n");
      return 2;
    }
  }

  if (host_path.empty()) {
    host_path = platform::executable_directory() + "/engine-host";
#if ENGINE_PLATFORM_WINDOWS
    host_path.append(".exe");
#endif
  }
  Vector<std::string_view> host_argv;
  host_argv.push_back(host_path);
  host_argv.push_back("--stdio");
  for (const std::string& m : mounts) {
    host_argv.push_back("--mount");
    host_argv.push_back(m);
  }
  for (const std::string& f : forwarded)
    host_argv.push_back(f);
  if (console) return run_console(host_argv, console_script, script_args);

  Client client;
  std::string error;
  if (!client.host.spawn(std::span<const std::string_view>(host_argv.data(), host_argv.size()),
                         &error)) {
    std::fprintf(stderr, "engine-cli: cannot start engine-host at '%s': %s\n", host_path.c_str(),
                 error.c_str());
    return 2;
  }

  int exit_code = 0;
  if (!doc_dir.empty()) {
    JsonValue open = JsonValue::object();
    open.set("path", JsonValue(doc_dir));
    open.set("create", JsonValue(create));
    if (!doc_name.empty()) open.set("name", JsonValue(doc_name));
    JsonValue info;
    if (!client.call("session.open", std::move(open), info)) {
      exit_code = 1;
    } else {
      if (params.is_null()) params = JsonValue::object();
      if (!params.contains("session")) params.set("session", *info.find("session"));
    }
  }
  if (exit_code == 0) {
    JsonValue result;
    if (!client.call(method, std::move(params), result)) {
      exit_code = 1;
    } else {
      std::string text = write_json(result, JsonWriteOptions{.pretty = !compact});
      text.push_back('\n');
      std::fwrite(text.data(), 1, text.size(), stdout);
      // The report is written after the result has printed, so a failure to write it never
      // costs the caller the answer it already has.
      if (!report_path.empty() && !write_report(report_path, method, result)) exit_code = 1;
    }
  }
  client.host.close_stdin();
  const i32 host_exit = client.host.wait();
  if (host_exit != 0 && exit_code == 0) {
    std::fprintf(stderr, "engine-cli: engine-host exited with %d\n", host_exit);
    exit_code = 1;
  }
  return exit_code;
}
