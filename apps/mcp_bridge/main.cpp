// engine-mcp (docs/plan/06-agent-tooling.md §6.3, docs/subsystems/apps.md "engine-mcp"): a Model
// Context Protocol server over stdio that spawns one engine-host and turns curated tools into
// engine protocol calls. stdout belongs to MCP alone; every diagnostic goes to stderr, which an
// MCP client keeps as the server's log. The bridge never opens a socket.
#include "bridge.h"

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>

#include <charconv>
#include <cstdio>
#include <engine_build_stamp.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#if ENGINE_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

using namespace engine;

namespace {

const char* k_usage =
    "usage: engine-mcp [--host <path>] [--host-arg <arg>]... [--mount <scheme>=<dir>[:rw]]...\n"
    "                  [--workspace <dir>] [--actor <name>] [--role <name>] [--task <id>]\n"
    "                  [--roles <file.json>]\n"
    "                  [--call-timeout <seconds>] [--long-call-timeout <seconds>] [--log <spec>]\n"
    "\n"
    "A Model Context Protocol server on stdin/stdout (newline-delimited JSON-RPC 2.0) over one\n"
    "engine-host it starts and owns. Register it with an MCP client; see docs/subsystems/apps.md.\n"
    "\n"
    "  --host <path>        engine-host executable (default: beside engine-mcp)\n"
    "  --host-arg <arg>     appended to engine-host's command line; repeatable\n"
    "  --mount <spec>       forwarded to engine-host; repeatable\n"
    "  --workspace <dir>    where captures, benchmark results and reports are written (default:\n"
    "                       ./mcp-workspace); tools return file:// URIs into it\n"
    "  --actor <name>       the attribution actor of a mutation that names none (default: mcp)\n"
    "  --role <name>        the attribution role of a mutation that names none (default: none);\n"
    "                       with --roles, also the role configuration every call runs in\n"
    "  --task <id>          the attribution task of a mutation that names none (default: none)\n"
    "  --roles <file>       role configurations for the host (content/roles/roles.json): tools\n"
    "                       the role may not call are not offered, and the host refuses its\n"
    "                       writes outside the role's layers, tiles and types\n"
    "  --call-timeout <s>   how long a quick call may go unanswered before the host is taken for\n"
    "                       hung, stopped and replaced on the next call (default: 120; 0: never)\n"
    "  --long-call-timeout <s>  the same for renders, content builds, headless runs and the\n"
    "                       validators (default: 3600; 0: never)\n"
    "  --log <spec>         log levels on stderr, e.g. \"info\" or \"info,mcp=debug\" (default:\n"
    "                       warnings and up)\n"
    "  --version            print the build stamp as one JSON line and exit\n";

std::string utf8(const std::filesystem::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// Seconds as given on the command line — a whole or a decimal number, 0 for no deadline — in
// milliseconds. At most thirty days: a deadline past that is a typo, and a clock's time point that
// far out is where the arithmetic starts to overflow.
bool read_seconds(std::string_view text, u64& milliseconds) {
  f64 seconds = -1.0;
  const std::from_chars_result r = std::from_chars(text.data(), text.data() + text.size(), seconds);
  if (r.ec != std::errc() || r.ptr != text.data() + text.size() || !(seconds >= 0.0) ||
      seconds > 30.0 * 24.0 * 3600.0) {
    return false;
  }
  milliseconds = static_cast<u64>(seconds * 1000.0 + 0.5);
  if (seconds > 0.0 && milliseconds == 0) milliseconds = 1;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();  // ADR-0031, first statement
  mcp::BridgeOptions options;
  options.actor = "mcp";
  std::string workspace;
  std::string log_spec;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "engine-mcp: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (a == "--help" || a == "-h") {
      std::fputs(k_usage, stdout);
      return 0;
    } else if (a == "--version") {
      std::printf("{\"tool\":\"engine-mcp\",\"commit\":\"%s\",\"dirty\":%s}\n",
                  build_stamp::commit(), build_stamp::dirty() ? "true" : "false");
      return 0;
    } else if (a == "--host") {
      if (!value(options.host_path)) return 2;
    } else if (a == "--workspace") {
      if (!value(workspace)) return 2;
    } else if (a == "--actor") {
      if (!value(options.actor)) return 2;
    } else if (a == "--role") {
      if (!value(options.role)) return 2;
    } else if (a == "--task") {
      if (!value(options.task)) return 2;
    } else if (a == "--roles") {
      if (!value(options.roles)) return 2;
    } else if (a == "--call-timeout" || a == "--long-call-timeout") {
      std::string text;
      if (!value(text)) return 2;
      u64& target = a == "--call-timeout" ? options.call_timeout_ms : options.long_call_timeout_ms;
      if (!read_seconds(text, target)) {
        std::fprintf(stderr,
                     "engine-mcp: %.*s takes seconds, from 0 (no deadline) to 2592000; got '%s'\n",
                     static_cast<int>(a.size()), a.data(), text.c_str());
        return 2;
      }
    } else if (a == "--log") {
      if (!value(log_spec)) return 2;
    } else if (a == "--mount") {
      std::string m;
      if (!value(m)) return 2;
      options.mounts.push_back(m);
    } else if (a == "--host-arg") {
      std::string h;
      if (!value(h)) return 2;
      options.host_args.push_back(h);
    } else {
      std::fprintf(stderr, "engine-mcp: unknown option '%s'\n%s", argv[i], k_usage);
      return 2;
    }
  }
  if (options.actor.empty()) {
    std::fprintf(stderr, "engine-mcp: --actor must not be empty\n");
    return 2;
  }

#if ENGINE_PLATFORM_WINDOWS
  // Messages are bytes: no CRLF translation either way.
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stdin), _O_BINARY);
#else
  // A host that has died makes the next write to its stdin a broken pipe. That has to come back
  // as a failed write the bridge reports, not as the signal that would end the bridge with it.
  std::signal(SIGPIPE, SIG_IGN);
#endif

  // Diagnostics to stderr only: stdout is the MCP stream.
  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  stderr_sink.set_min_level(log::Level::Warn);
  log::add_sink(&stderr_sink);
  if (!log_spec.empty()) {
    std::string error;
    if (!log::apply_level_spec(log_spec, &error)) {
      std::fprintf(stderr, "engine-mcp: %s\n", error.c_str());
      return 2;
    }
    stderr_sink.set_min_level(log::Level::Trace);
  }

  if (options.host_path.empty()) {
    options.host_path = platform::executable_directory() + "/engine-host";
#if ENGINE_PLATFORM_WINDOWS
    options.host_path.append(".exe");
#endif
  }
  {
    std::error_code ec;
    std::filesystem::path dir =
        workspace.empty()
            ? std::filesystem::current_path(ec) / "mcp-workspace"
            : std::filesystem::path(std::u8string_view(
                  reinterpret_cast<const char8_t*>(workspace.data()), workspace.size()));
    const std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
    options.workspace = utf8(ec ? dir : absolute.lexically_normal());
    while (options.workspace.size() > 1 && options.workspace.back() == '/')
      options.workspace.pop_back();
  }
  // Absolute, so that a host started later from anywhere reads the same file.
  if (!options.roles.empty()) {
    std::error_code ec;
    const std::filesystem::path given(std::u8string_view(
        reinterpret_cast<const char8_t*>(options.roles.data()), options.roles.size()));
    const std::filesystem::path absolute = std::filesystem::absolute(given, ec);
    if (!ec) options.roles = utf8(absolute.lexically_normal());
  }

  mcp::Bridge bridge(std::move(options));
  std::string error;
  if (!bridge.start(error)) {
    std::fprintf(stderr, "engine-mcp: %s\n", error.c_str());
    log::remove_sink(&stderr_sink);
    return error.starts_with("cannot start engine-host") ? 2 : 1;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;
    const JsonValue response = bridge.handle_text(line);
    if (response.is_null()) continue;
    std::string out = write_json(response, JsonWriteOptions{.pretty = false});
    out.push_back('\n');
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
  }
  // The client closed our input: the session is over. The host exits when its own input ends.
  bridge.shutdown();
  log::flush();
  log::remove_sink(&stderr_sink);
  return 0;
}
