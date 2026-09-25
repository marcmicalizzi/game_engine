// engine-host (docs/plan/02-architecture.md §2.2, ADR-0001): the engine as a headless server.
// JSON-RPC over stdio, one request per line in and one response per line out. The editor, the
// MCP bridge, and engine-cli are clients of this. Beyond the document methods it now serves
// `render.*` over `systems/renderer` (render_methods.cpp) — offscreen, with no window anywhere
// in the process, which is what the Phase 1 exit criterion asks for: agents capture and
// benchmark, and an agent has no display — and the rest of plan 06 §6.9's day-one operations,
// content.build, session.events, engine.budgets, session.run_headless and engine.run_tests
// (ops_methods.cpp). It answers `engine.ping` itself, below, because liveness is a question about
// this process rather than about any library it links.
#include "host_state.h"
#include "ops_methods.h"
#include "render_methods.h"

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/time/time.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

// `audio.devices` when this build has the audio capability (ADR-0027); CMakeLists.txt defines the
// switch only when it does, and a minimal build is the proof that the host needs nothing from it.
#if defined(ENGINE_HOST_AUDIO)
#include <domain/audio/protocol.h>
#endif

#if ENGINE_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_host, "host");

const char* k_usage =
    "usage: engine-host [--stdio] [--request <json>] [--mount <scheme>=<dir>[:rw]]...\n"
    "                   [--log <spec>] [--log-json <path>] [--tunables <file>]\n"
    "                   [--roles <file.json>] [--actor <name>] [--role <name>] [--task <id>]\n"
    "\n"
    "  --stdio          serve JSON-RPC 2.0: one request per line on stdin, one response per\n"
    "                   line on stdout (default)\n"
    "  --request <json> answer one request and exit\n"
    "  --mount          expose a directory as <scheme>://; ':rw' makes it writable\n"
    "  --log <spec>     log levels, e.g. \"info,host=debug\" (stderr shows warnings and up)\n"
    "  --log-json       append every log record as JSON lines to this file\n"
    "  --tunables       load tunable values from a JSON object file\n"
    "  --roles          role configurations (plan 06 section 6.5): what each role may change;\n"
    "                   without it nothing is restricted (content/roles/roles.json)\n"
    "  --actor, --role, --task\n"
    "                   who a call that names no actor, role or task is: its attribution, and\n"
    "                   with --roles the role it is checked as (--role must be in the file)\n"
    "\n"
    "Test hook, not for use (docs/subsystems/apps.md, \"Deadlines\"):\n"
    "  --debug-hang <method>  never answer <method>, and read nothing after it: the host a\n"
    "                         client's deadline exists for, made on purpose\n";

bool next_value(int argc, char** argv, int& i, std::string_view flag, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "engine-host: %.*s needs a value\n", static_cast<int>(flag.size()),
                 flag.data());
    return false;
  }
  out = argv[++i];
  return true;
}

// ---- engine.ping -------------------------------------------------------------------------------
//
// Liveness, and nothing else: the process id and how long it has been up. It reads a clock and
// touches no state — no session, no device, no lock — so it can be asked at any time and can never
// itself be the reason a host stops answering.
bool engine_ping(protocol::Context& ctx, protocol::PingResult& out, protocol::RpcError&) {
#if ENGINE_PLATFORM_WINDOWS
  out.pid = static_cast<u32>(::_getpid());
#else
  out.pid = static_cast<u32>(::getpid());
#endif
  const auto* state = static_cast<const host::HostState*>(ctx.app);
  if (state != nullptr) {
    out.uptime_seconds = static_cast<f64>(time::monotonic_ns() - state->started_ns) / 1.0e9;
  }
  return true;
}

// ---- --debug-hang ------------------------------------------------------------------------------
//
// A test hook: the one way to make a host that is alive and never answers on purpose, which is
// what a client's deadline exists for (engine-mcp's `--call-timeout`; apps.md, "Deadlines"). The
// request is recognized before it is dispatched, so the method's handler never runs and the hook
// works for any method, including one this host does not serve. It is listed in `--help` under a
// heading of its own rather than hidden: a flag the binary takes but never mentions is behaviour
// nobody can find, and the heading says what it is for.
bool names_method(std::string_view line, std::string_view method) {
  JsonValue request;
  if (!parse_json(line, request).ok || !request.is_object()) return false;
  const JsonValue* name = request.find("method");
  std::string_view text;
  return name != nullptr && name->get_string(text) && text == method;
}

[[noreturn]] void hang_forever(std::string_view method) {
  ENGINE_LOG_WARN(log_host, "--debug-hang: this request is never answered",
                  log::field("method", method));
  log::flush();
  // Blocked in the kernel, not spinning: nothing ever notifies, and a spurious wake-up waits again.
  // The process ends when a client kills it, which is what the hook is for.
  std::mutex mutex;
  std::condition_variable never;
  std::unique_lock<std::mutex> lock(mutex);
  for (;;)
    never.wait(lock);
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();  // ADR-0031, first statement
  const i64 started_ns = time::monotonic_ns();
  std::string request;
  std::string log_spec;
  std::string log_json;
  std::string tunables_file;
  std::string hang_method;
  std::string roles_file;
  protocol::Policy policy;
  Vector<std::string> mounts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    std::string value;
    if (a == "--stdio") {
      continue;
    } else if (a == "--help" || a == "-h") {
      std::fputs(k_usage, stdout);
      return 0;
    } else if (a == "--request") {
      if (!next_value(argc, argv, i, a, request)) return 2;
    } else if (a == "--mount") {
      if (!next_value(argc, argv, i, a, value)) return 2;
      mounts.push_back(value);
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, a, log_spec)) return 2;
    } else if (a == "--log-json") {
      if (!next_value(argc, argv, i, a, log_json)) return 2;
    } else if (a == "--tunables") {
      if (!next_value(argc, argv, i, a, tunables_file)) return 2;
    } else if (a == "--debug-hang") {
      if (!next_value(argc, argv, i, a, hang_method)) return 2;
    } else if (a == "--roles") {
      if (!next_value(argc, argv, i, a, roles_file)) return 2;
    } else if (a == "--actor") {
      if (!next_value(argc, argv, i, a, policy.identity.actor)) return 2;
    } else if (a == "--role") {
      if (!next_value(argc, argv, i, a, policy.identity.role)) return 2;
    } else if (a == "--task") {
      if (!next_value(argc, argv, i, a, policy.identity.task)) return 2;
    } else {
      std::fprintf(stderr, "engine-host: unknown option '%s'\n%s", argv[i], k_usage);
      return 2;
    }
  }

#if ENGINE_PLATFORM_WINDOWS
  // Responses are bytes; no CRLF translation on the protocol stream.
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif

  // Logging: everything into the ring for log.tail, warnings and up to stderr, optionally
  // every record to a JSON-lines file. stdout belongs to the protocol.
  log::RingSink ring(2048);
  log::add_sink(&ring);
  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  stderr_sink.set_min_level(log::Level::Warn);
  log::add_sink(&stderr_sink);
  std::FILE* json_file = nullptr;
  if (!log_json.empty()) {
#if ENGINE_COMPILER_MSVC
    (void)fopen_s(&json_file, log_json.c_str(), "ab");
#else
    json_file = std::fopen(log_json.c_str(), "ab");
#endif
    if (json_file == nullptr) {
      std::fprintf(stderr, "engine-host: cannot open log file '%s'\n", log_json.c_str());
      return 2;
    }
  }
  log::StreamSink json_sink(json_file != nullptr ? json_file : stderr,
                            log::StreamSink::Format::JsonLines, /*close_on_destroy=*/false);
  if (json_file != nullptr) log::add_sink(&json_sink);
  log::install_assert_hook();
  if (!log_spec.empty()) {
    std::string error;
    if (!log::apply_level_spec(log_spec, &error)) {
      std::fprintf(stderr, "engine-host: %s\n", error.c_str());
      return 2;
    }
  }

  io::Vfs vfs;
  for (const std::string& m : mounts) {
    const usize eq = m.find('=');
    if (eq == std::string::npos) {
      std::fprintf(stderr, "engine-host: --mount expects <scheme>=<dir>[:rw]\n");
      return 2;
    }
    std::string_view dir = std::string_view(m).substr(eq + 1);
    bool writable = false;
    if (dir.ends_with(":rw")) {
      writable = true;
      dir.remove_suffix(3);
    }
    const io::Status s = vfs.mount(std::string_view(m).substr(0, eq), dir, writable);
    if (s != io::Status::Ok) {
      std::fprintf(stderr, "engine-host: cannot mount '%s': %s\n", m.c_str(), io::status_name(s));
      return 2;
    }
  }

  if (!tunables_file.empty()) {
    Vector<std::string> problems;
    if (!tunables::load_file(tunables_file.c_str(), &problems)) {
      for (const std::string& p : problems) {
        ENGINE_LOG_WARN(log_host, "tunables file problem", log::field("problem", p));
      }
    }
  }

  // Roles and the default identity (plan 06 §6.5, domain/protocol/policy.h). A roles file that does
  // not read, or a --role it does not have, is a usage error: a host told to restrict calls that
  // quietly restricted nothing would be worse than one that refused to start.
  if (!roles_file.empty()) {
    std::string text;
    if (const io::Status s = io::read_file(roles_file, text); s != io::Status::Ok) {
      std::fprintf(stderr, "engine-host: cannot read roles file '%s': %s\n", roles_file.c_str(),
                   io::status_name(s));
      return 2;
    }
    std::string error;
    if (!policy.load_roles(roles_file, text, error)) {
      std::fprintf(stderr, "engine-host: %s\n", error.c_str());
      return 2;
    }
    if (!policy.identity.role.empty() && policy.find_role(policy.identity.role) == nullptr) {
      std::fprintf(stderr, "engine-host: --role '%s' is not a role of %s\n",
                   policy.identity.role.c_str(), roles_file.c_str());
      return 2;
    }
  }

  protocol::SessionManager sessions(vfs);
  // The host's own methods' state reaches their handlers through Context::app (host_state.h): the
  // renderer's device and scenes, and the sessions' runtime worlds. It must outlive the
  // dispatcher. Nothing is created until a call asks for it, so a host that only edits documents
  // never opens a device and never builds a world.
  host::HostState state;
  state.started_ns = started_ns;
  protocol::Dispatcher dispatcher(protocol::Context{&sessions, &ring, nullptr, &state, &policy});
  protocol::add_builtin_methods(dispatcher);
  dispatcher.add(protocol::read_only(protocol::method_no_params<protocol::PingResult, &engine_ping>(
      "engine.ping",
      "Liveness: the host's process id and how long it has been up. Touches no state, so it is "
      "answered at once whenever the host is reading requests at all.")));
  host::add_render_methods(dispatcher);
  // content.build, session.events, engine.budgets, session.run_headless, engine.run_tests
  // (ops_methods.h): the rest of plan 06 §6.9's day-one list.
  host::add_ops_methods(dispatcher);
#if defined(ENGINE_HOST_AUDIO)
  audio::register_methods(dispatcher);
#endif
  ENGINE_LOG_INFO(log_host, "engine-host ready",
                  log::field("methods", static_cast<u64>(dispatcher.methods().size())),
                  log::field("mounts", static_cast<u64>(vfs.mounts().size())));

  if (!hang_method.empty()) {
    ENGINE_LOG_WARN(log_host, "--debug-hang is set: a test hook, never for use",
                    log::field("method", hang_method));
  }
  int exit_code = 0;
  if (!request.empty()) {
    if (!hang_method.empty() && names_method(request, hang_method)) hang_forever(hang_method);
    const std::string out = dispatcher.dispatch_text(request);
    if (!out.empty()) {
      std::fwrite(out.data(), 1, out.size(), stdout);
      std::fputc('\n', stdout);
    }
  } else {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (!hang_method.empty() && names_method(line, hang_method)) hang_forever(hang_method);
      const std::string out = dispatcher.dispatch_text(line);
      if (!out.empty()) {
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
      }
    }
  }
  std::fflush(stdout);
  ENGINE_LOG_INFO(log_host, "engine-host exiting", log::field("sessions", sessions.count()),
                  log::field("scenes", state.render.count()),
                  log::field("worlds", state.ops.count()));
  log::flush();
  log::remove_sink(&json_sink);
  log::remove_sink(&stderr_sink);
  log::remove_sink(&ring);
  if (json_file != nullptr) std::fclose(json_file);
  return exit_code;
}
