// engine-host --console (console.h; docs/subsystems/apps.md, "The console").
#include "console.h"

#include <core/json/json.h>
#include <core/schema/type_info.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <iostream>
#include <iterator>
#include <string_view>

#if defined(ENGINE_HOST_SCRIPTING)
#include <foundation/scripting/script_context.h>
#endif

namespace engine::host {

#if defined(ENGINE_HOST_SCRIPTING)
namespace {

struct Link {
  protocol::Dispatcher* dispatcher = nullptr;
  u64 next_id = 1;
};

// One method call from the program: a JSON-RPC request object handed to the dispatcher, which is
// what a line on stdin becomes — so the role gate, the identity's attribution and the handlers
// see nothing that tells the two apart.
bool call(void* user, std::string_view method, const JsonValue& params, JsonValue& result,
          scripting::HostError& error) {
  Link& link = *static_cast<Link*>(user);
  JsonValue request = JsonValue::object();
  request.set("jsonrpc", JsonValue("2.0"));
  request.set("id", JsonValue(link.next_id++));
  request.set("method", JsonValue(method));
  if (!params.is_null()) request.set("params", params);
  const JsonValue response = link.dispatcher->dispatch(request);
  if (const JsonValue* e = response.find("error"); e != nullptr) {
    if (const JsonValue* code = e->find("code"); code != nullptr && code->is_number())
      error.code = code->as_int();
    if (const JsonValue* message = e->find("message"); message != nullptr && message->is_string())
      error.message = std::string(message->as_string());
    if (const JsonValue* data = e->find("data"); data != nullptr) error.data = *data;
    return false;
  }
  const JsonValue* r = response.find("result");
  result = r != nullptr ? *r : JsonValue();
  return true;
}

// The program's own output. Flushed per line, so engine-cli, which forwards the host's stdout a
// line at a time, shows a long script's progress as it goes.
void print(void*, std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

}  // namespace
#endif

int run_console(protocol::Dispatcher& dispatcher, const ConsoleOptions& options) {
#if !defined(ENGINE_HOST_SCRIPTING)
  (void)dispatcher;
  (void)options;
  std::fputs(
      "engine-host: --console needs the scripting capability, which this build does not have "
      "(ENGINE_WITH_SCRIPTING=OFF)\n",
      stderr);
  return k_console_exit_usage;
#else
  std::string source;
  std::string chunk;
  if (!options.script.empty()) {
    if (const io::Status s = io::read_file(options.script, source); s != io::Status::Ok) {
      std::fprintf(stderr, "engine-host: cannot read script '%s': %s\n", options.script.c_str(),
                   io::status_name(s));
      return k_console_exit_usage;
    }
    chunk = options.script;
  } else {
    source.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    chunk = "stdin";
  }

  // Every method the dispatcher has, each with its params type from the schema registry, so the
  // program's tables are read through the descriptors and every method has a function.
  Vector<scripting::HostMethod> methods;
  for (const protocol::MethodDesc& m : dispatcher.methods()) {
    const schema::TypeInfo* type = m.params_type != nullptr && m.params_type[0] != '\0'
                                       ? schema::Registry::global().find(m.params_type)
                                       : nullptr;
    methods.push_back(scripting::HostMethod{m.name, type});
  }

  scripting::ScriptLimits limits;
  limits.step_budget = options.step_budget;
  limits.memory_bytes = options.memory_mb << 20;
  limits.time_budget_ns = 0;  // a method may render for minutes; only the script's own work counts
  scripting::ScriptContext context(limits);
  Link link;
  link.dispatcher = &dispatcher;
  const scripting::ProgramHost host{&link, {methods.data(), methods.size()}, &call, &print};
  Vector<std::string_view> args;
  for (const std::string& a : options.args)
    args.push_back(a);

  const scripting::Status status =
      context.run_program(chunk, source, host, {args.data(), args.size()});
  std::fflush(stdout);
  if (status == scripting::Status::Ok) return k_console_exit_ok;

  // "<chunk>:<line>: <message>", the way a compiler reports a position, so an editor or an agent
  // goes straight to the line.
  const scripting::ScriptError& e = context.last_error();
  if (e.line != 0) {
    std::fprintf(stderr, "%s:%u: %s\n", e.chunk.c_str(), e.line, e.message.c_str());
  } else {
    std::fprintf(stderr, "%s: %s\n", e.chunk.c_str(), e.message.c_str());
  }
  if (status == scripting::Status::BudgetExceeded) {
    std::fprintf(stderr,
                 "engine-host: the script was stopped by its step budget, %llu steps for the "
                 "whole run (--step-budget <n>, 0 for none)\n",
                 static_cast<unsigned long long>(options.step_budget));
  } else if (status == scripting::Status::MemoryExceeded) {
    std::fprintf(stderr,
                 "engine-host: the script was stopped by its memory limit, %llu MiB "
                 "(--memory-mb <n>)\n",
                 static_cast<unsigned long long>(options.memory_mb));
  }
  return k_console_exit_failed;
#endif
}

}  // namespace engine::host
