// engine-host (docs/plan/02-architecture.md §2.2, ADR-0001): the engine as a headless server.
// Phase 0 shape: `sim` mode only, JSON-RPC over stdio, one request per line in and one
// response per line out. The editor, the MCP bridge, and engine-cli are clients of this.
#include <core/log/log.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#if ENGINE_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_host, "host");

const char* k_usage =
    "usage: engine-host [--stdio] [--request <json>] [--mount <scheme>=<dir>[:rw]]...\n"
    "                   [--log <spec>] [--log-json <path>] [--tunables <file>]\n"
    "\n"
    "  --stdio          serve JSON-RPC 2.0: one request per line on stdin, one response per\n"
    "                   line on stdout (default)\n"
    "  --request <json> answer one request and exit\n"
    "  --mount          expose a directory as <scheme>://; ':rw' makes it writable\n"
    "  --log <spec>     log levels, e.g. \"info,host=debug\" (stderr shows warnings and up)\n"
    "  --log-json       append every log record as JSON lines to this file\n"
    "  --tunables       load tunable values from a JSON object file\n";

bool next_value(int argc, char** argv, int& i, std::string_view flag, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "engine-host: %.*s needs a value\n", static_cast<int>(flag.size()),
                 flag.data());
    return false;
  }
  out = argv[++i];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string request;
  std::string log_spec;
  std::string log_json;
  std::string tunables_file;
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

  protocol::SessionManager sessions(vfs);
  protocol::Dispatcher dispatcher(protocol::Context{&sessions, &ring, nullptr});
  protocol::add_builtin_methods(dispatcher);
  ENGINE_LOG_INFO(log_host, "engine-host ready",
                  log::field("methods", static_cast<u64>(dispatcher.methods().size())),
                  log::field("mounts", static_cast<u64>(vfs.mounts().size())));

  int exit_code = 0;
  if (!request.empty()) {
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
      const std::string out = dispatcher.dispatch_text(line);
      if (!out.empty()) {
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
      }
    }
  }
  std::fflush(stdout);
  ENGINE_LOG_INFO(log_host, "engine-host exiting", log::field("sessions", sessions.count()));
  log::flush();
  log::remove_sink(&json_sink);
  log::remove_sink(&stderr_sink);
  log::remove_sink(&ring);
  if (json_file != nullptr) std::fclose(json_file);
  return exit_code;
}
