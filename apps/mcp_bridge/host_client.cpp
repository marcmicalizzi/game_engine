// The one engine-host behind the bridge: spawned like engine-cli spawns it, spoken to one
// JSON-RPC line at a time, and reaped when its pipes close.
#include "bridge.h"

#include <core/json/json.h>
#include <core/log/log.h>

namespace engine::mcp {

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_host_client, "mcp.host");
}  // namespace

HostClient::~HostClient() { stop(); }

bool HostClient::start(const BridgeOptions& options, std::string& error) {
  stop();
  Vector<std::string_view> argv;
  argv.push_back(options.host_path);
  argv.push_back("--stdio");
  for (const std::string& m : options.mounts) {
    argv.push_back("--mount");
    argv.push_back(m);
  }
  auto process = std::make_unique<platform::Process>();
  std::string why;
  if (!process->spawn(std::span<const std::string_view>(argv.data(), argv.size()), &why)) {
    error = "cannot start engine-host at '" + options.host_path + "': " + why;
    return false;
  }
  process_ = std::move(process);
  ++generation_;
  ENGINE_LOG_INFO(log_host_client, "engine-host started", log::field("path", options.host_path),
                  log::field("generation", generation_));
  return true;
}

void HostClient::stop() {
  if (process_ == nullptr) return;
  process_->close_stdin();
  const i32 code = process_->wait();
  ENGINE_LOG_INFO(log_host_client, "engine-host stopped", log::field("exit_code", code));
  process_.reset();
}

void HostClient::reap(Error& error, std::string_view what) {
  // The host's end of a pipe is closed, which a process does by exiting: waiting cannot block for
  // long. The exit code is the only account of the death the bridge will get.
  process_->close_stdin();
  const i32 code = process_->wait();
  process_.reset();
  error.code = code;
  error.message = "engine-host exited";
  if (code == -1) {
    error.message += " (killed by a signal, or its exit code is unknown)";
  } else {
    error.message += " with code " + std::to_string(code);
  }
  error.message += "; ";
  error.message += what;
  error.data = JsonValue();
  ENGINE_LOG_WARN(log_host_client, "engine-host exited", log::field("exit_code", code),
                  log::field("while", what));
}

HostClient::Status HostClient::call(std::string_view method, const JsonValue& params,
                                    JsonValue& result, Error& error) {
  if (process_ == nullptr) {
    error.code = -1;
    error.message = "engine-host is not running";
    return Status::gone;
  }
  const u64 id = next_id_++;
  JsonValue request = JsonValue::object();
  request.set("jsonrpc", JsonValue("2.0"));
  request.set("id", JsonValue(id));
  request.set("method", JsonValue(method));
  if (!params.is_null()) request.set("params", params);
  std::string line = write_json(request, JsonWriteOptions{.pretty = false});
  line.push_back('\n');
  if (!process_->write(line)) {
    reap(error, std::string("it would not take the request for ") + std::string(method));
    return Status::gone;
  }
  // The host answers in order and writes nothing else to stdout, so the next line with this id is
  // the answer. A line that is not one is skipped with a warning rather than taken for it.
  for (;;) {
    std::string text;
    if (!process_->read_line(text)) {
      reap(error, std::string("it closed its output before answering ") + std::string(method));
      return Status::gone;
    }
    JsonValue response;
    if (!parse_json(text, response).ok || !response.is_object()) {
      ENGINE_LOG_WARN(log_host_client, "engine-host wrote a line that is not a response",
                      log::field("line", text.substr(0, 200)));
      continue;
    }
    const JsonValue* got = response.find("id");
    u64 got_id = 0;
    if (got == nullptr || !got->get_u64(got_id) || got_id != id) {
      ENGINE_LOG_WARN(log_host_client, "engine-host answered another request",
                      log::field("line", text.substr(0, 200)));
      continue;
    }
    if (const JsonValue* e = response.find("error"); e != nullptr && e->is_object()) {
      i64 code = 0;
      const JsonValue* c = e->find("code");
      if (c == nullptr || !c->get_i64(code)) code = 0;
      error.code = static_cast<i32>(code);
      error.message = text_of(*e, "message");
      const JsonValue* data = e->find("data");
      error.data = data != nullptr ? *data : JsonValue();
      return Status::error;
    }
    const JsonValue* r = response.find("result");
    result = r != nullptr ? *r : JsonValue();
    return Status::ok;
  }
}

}  // namespace engine::mcp
