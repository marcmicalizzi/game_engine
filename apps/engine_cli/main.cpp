// engine-cli (docs/plan/02-architecture.md §2.2): the thin scriptable client. Spawns
// engine-host, optionally opens a document, sends one method call, and prints the result.
// Because sessions persist their journal and undo position on disk, a sequence of separate
// invocations behaves like one editing session.
#include <core/json/json.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

using namespace engine;

namespace {

const char* k_usage =
    "usage: engine-cli [options] <method> [params-json]\n"
    "\n"
    "  --host <path>      engine-host executable (default: beside engine-cli)\n"
    "  --doc <dir>        open this document first and pass its session to the method\n"
    "  --create           with --doc: create the document when it does not exist\n"
    "  --name <name>      with --create: the document's name\n"
    "  --mount <spec>     forwarded to engine-host (<scheme>=<dir>[:rw]); repeatable\n"
    "  --compact          print the result on one line\n"
    "  --report <file>    also write the result to this file as one JSON document, with the\n"
    "                     method and a UTC timestamp around it: a file to send back\n"
    "\n"
    "examples:\n"
    "  engine-cli engine.methods\n"
    "  engine-cli gpu.adapters --report adapters.json\n"
    "  engine-cli --doc ./world --create session.info\n"
    "  engine-cli --doc ./world doc.apply "
    "'{\"commands\":[...],\"attribution\":{\"actor\":\"me\"}}'\n"
    "  engine-cli --doc ./world doc.undo\n";

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
  if (method.empty()) {
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
