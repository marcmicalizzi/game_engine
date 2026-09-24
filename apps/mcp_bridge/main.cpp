// engine-mcp (docs/plan/06-agent-tooling.md §6.3, docs/subsystems/apps.md "engine-mcp"): a Model
// Context Protocol server over stdio that spawns one engine-host and turns curated tools into
// engine protocol calls. stdout belongs to MCP alone; every diagnostic goes to stderr, which an
// MCP client keeps as the server's log. The bridge never opens a socket.
#include "bridge.h"

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>

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
    "usage: engine-mcp [--host <path>] [--mount <scheme>=<dir>[:rw]]... [--workspace <dir>]\n"
    "                  [--actor <name>] [--log <spec>]\n"
    "\n"
    "A Model Context Protocol server on stdin/stdout (newline-delimited JSON-RPC 2.0) over one\n"
    "engine-host it starts and owns. Register it with an MCP client; see docs/subsystems/apps.md.\n"
    "\n"
    "  --host <path>      engine-host executable (default: beside engine-mcp)\n"
    "  --mount <spec>     forwarded to engine-host; repeatable\n"
    "  --workspace <dir>  where captures, benchmark results and reports are written (default:\n"
    "                     ./mcp-workspace); tools return file:// URIs into it\n"
    "  --actor <name>     the attribution actor of a mutation that names none (default: mcp)\n"
    "  --log <spec>       log levels on stderr, e.g. \"info\" or \"info,mcp=debug\" (default:\n"
    "                     warnings and up)\n"
    "  --version          print the build stamp as one JSON line and exit\n";

std::string utf8(const std::filesystem::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
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
    } else if (a == "--log") {
      if (!value(log_spec)) return 2;
    } else if (a == "--mount") {
      std::string m;
      if (!value(m)) return 2;
      options.mounts.push_back(m);
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
