// The one engine-host behind the bridge: spawned like engine-cli spawns it, spoken to one
// JSON-RPC line at a time, reaped when its pipes close, and killed when it stops answering.
//
// **Why a reader thread** (docs/subsystems/apps.md, "Deadlines"). A call has to wait for its
// answer with a deadline, so the read of the host's stdout needs a timeout, and a blocking read of
// a pipe has none. The two ways to get one are a read that can be waited on — `poll` on the
// descriptor on POSIX, overlapped I/O on Windows — and a thread that does the blocking read while
// the caller waits on a condition variable with a deadline. Overlapped I/O is not available on the
// pipes `platform::Process` makes: an anonymous pipe (`CreatePipe`) does not support it, and the
// only pipe that does is a *named* one, which is an object in the machine's pipe namespace that
// another local process could open between its creation and ours — a listener in all but name,
// which this bridge must not have (it is stdio on both sides and nothing else). A thread keeps the
// pipe anonymous and gives both platforms one code path: it blocks in the kernel's read (no busy
// loop, no sleep between polls), hands each whole line over under a mutex, and wakes the caller
// through the condition variable, whose `wait_until` is the deadline. One thread per host
// generation, started with the host and joined when the host is reaped.
//
// On a deadline the host is killed, which closes its end of the pipe and so ends the thread's read.
// On Windows the read is also cancelled (`CancelSynchronousIo`), because a process the host started
// inherits that end — `CreateProcess` passes every inheritable handle, and the machine-state
// sampler starts nvidia-smi around a benchmark — and a kill that happened while one ran would
// otherwise leave the join waiting for it. The cancel can miss a thread that was between two reads
// at that instant; the join then waits for the pipe to close, which is when the host's last child
// exits, and nvidia-smi exits on its own.
#include "bridge.h"

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/thread.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::mcp {

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_host_client, "mcp.host");
}  // namespace

// ---- deadlines ---------------------------------------------------------------------------------

bool is_long_call(std::string_view method) noexcept {
  // The methods whose time grows with the content or with what was asked for. Everything else is
  // bookkeeping over a document or the host's own state, and a host that takes minutes over it is
  // a host that has stopped. render.unload and render.scenes are in the short class: they release
  // or list what is there, and the release waits only for the frames already submitted.
  constexpr std::string_view k_long[] = {
      "render.load",    "render.capture", "render.benchmark",     "render.evaluate",
      "render.compare", "content.build",  "session.run_headless", "engine.run_tests"};
  for (const std::string_view m : k_long) {
    if (method == m) return true;
  }
  return false;
}

u64 call_deadline_ms(const BridgeOptions& options, std::string_view method) noexcept {
  return is_long_call(method) ? options.long_call_timeout_ms : options.call_timeout_ms;
}

std::string seconds_text(u64 milliseconds) {
  if (milliseconds % 1000 == 0) return std::to_string(milliseconds / 1000);
  char text[32];
  std::snprintf(text, sizeof(text), "%.3f", static_cast<f64>(milliseconds) / 1000.0);
  std::string s = text;
  while (s.back() == '0')
    s.pop_back();
  return s;
}

// ---- the reader --------------------------------------------------------------------------------

// Written by the reader thread, taken by the caller; `mutex` guards everything but `thread`. The
// Process's read side (its stdout handle and its line buffer) belongs to the thread alone while it
// runs, and the write side and the process handle to the caller, so the two never touch the same
// state; the Process is destroyed only after the thread has been joined.
struct HostClient::Reader {
  std::mutex mutex;
  std::condition_variable ready;
  Vector<std::string> lines;  // oldest first, from `head`
  u32 head = 0;
  bool end = false;
  std::thread thread;

  void run(platform::Process& process) {
    (void)platform::set_current_thread_name("mcp-host-reader");
    std::string line;
    for (;;) {
      const bool got = process.read_line(line);
      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (got) {
          lines.push_back(std::move(line));
        } else {
          end = true;
        }
      }
      ready.notify_all();
      if (!got) return;
      line.clear();
    }
  }
};

HostClient::HostClient() = default;

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
  for (const std::string& a : options.host_args)
    argv.push_back(a);
  auto process = std::make_unique<platform::Process>();
  std::string why;
  if (!process->spawn(std::span<const std::string_view>(argv.data(), argv.size()), &why)) {
    error = "cannot start engine-host at '" + options.host_path + "': " + why;
    return false;
  }
  process_ = std::move(process);
  call_timeout_ms_ = options.call_timeout_ms;
  long_call_timeout_ms_ = options.long_call_timeout_ms;
  reader_ = std::make_unique<Reader>();
  Reader* reader = reader_.get();
  platform::Process* host = process_.get();
  reader_->thread = std::thread([reader, host] { reader->run(*host); });
  ++generation_;
  ENGINE_LOG_INFO(log_host_client, "engine-host started", log::field("path", options.host_path),
                  log::field("generation", generation_));
  return true;
}

HostClient::Next HostClient::next_line(std::string& line,
                                       const std::chrono::steady_clock::time_point* due) {
  std::unique_lock<std::mutex> lock(reader_->mutex);
  const auto has = [this] { return reader_->head < reader_->lines.size() || reader_->end; };
  if (due == nullptr) {
    reader_->ready.wait(lock, has);
  } else if (!reader_->ready.wait_until(lock, *due, has)) {
    return Next::timeout;
  }
  if (reader_->head < reader_->lines.size()) {
    line = std::move(reader_->lines[reader_->head++]);
    if (reader_->head == reader_->lines.size()) {
      reader_->lines.clear();
      reader_->head = 0;
    }
    return Next::line;
  }
  return Next::end;
}

void HostClient::join_reader() noexcept {
  if (reader_ == nullptr) return;
  if (reader_->thread.joinable()) {
#if ENGINE_PLATFORM_WINDOWS
    // See the top of the file: a child of the host may still hold the pipe's write end.
    (void)::CancelSynchronousIo(static_cast<HANDLE>(reader_->thread.native_handle()));
#endif
    reader_->thread.join();
  }
  reader_.reset();
}

void HostClient::stop() {
  if (process_ == nullptr) return;
  process_->close_stdin();
  // A host exits when its input ends, and its output ends with it. One still there after the
  // short deadline is hung, and waiting for it would hang the bridge's own exit with it.
  const auto due = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(static_cast<i64>(call_timeout_ms_));
  bool ended = false;
  for (std::string ignored;;) {
    const Next next = next_line(ignored, call_timeout_ms_ != 0 ? &due : nullptr);
    if (next == Next::line) continue;
    ended = next == Next::end;
    break;
  }
  if (!ended) {
    ENGINE_LOG_WARN(log_host_client, "engine-host did not exit when its input ended; stopping it",
                    log::field("seconds", seconds_text(call_timeout_ms_)));
    process_->kill();
  }
  const i32 code = process_->wait();
  join_reader();
  ENGINE_LOG_INFO(log_host_client, "engine-host stopped", log::field("exit_code", code));
  process_.reset();
}

void HostClient::reap(Error& error, std::string_view what) {
  // The host's end of a pipe is closed, which a process does by exiting: waiting cannot block for
  // long. The exit code is the only account of the death the bridge will get.
  process_->close_stdin();
  const i32 code = process_->wait();
  join_reader();
  process_.reset();
  error.code = code;
  error.deadline_ms = 0;
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

void HostClient::abandon(Error& error, std::string_view method, u64 deadline_ms) {
  // Alive and silent past the deadline. Killed rather than asked to stop: a host that is not
  // answering is not reading its input either, so closing it would change nothing.
  process_->kill();
  process_->close_stdin();
  const i32 code = process_->wait();  // the kill is asynchronous; this is when the handles go
  join_reader();
  process_.reset();
  error.code = code;
  error.deadline_ms = deadline_ms;
  error.message = "engine-host did not answer " + std::string(method) + " within " +
                  seconds_text(deadline_ms) + " s, so the bridge stopped it";
  error.data = JsonValue();
  ENGINE_LOG_WARN(log_host_client, "engine-host did not answer in time; stopped it",
                  log::field("method", method), log::field("seconds", seconds_text(deadline_ms)));
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
  // The deadline starts when the request is sent and covers the whole answer. The write itself has
  // none: a host that answered the previous call is back in its read of the next line, and reads a
  // whole line before it dispatches it, so a hang is always in the dispatch — after the write.
  const u64 deadline_ms = is_long_call(method) ? long_call_timeout_ms_ : call_timeout_ms_;
  const auto due =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<i64>(deadline_ms));
  if (!process_->write(line)) {
    reap(error, std::string("it would not take the request for ") + std::string(method));
    return Status::gone;
  }
  // The host answers in order and writes nothing else to stdout, so the next line with this id is
  // the answer. A line that is not one is skipped with a warning rather than taken for it.
  for (;;) {
    std::string text;
    const Next next = next_line(text, deadline_ms != 0 ? &due : nullptr);
    if (next == Next::timeout) {
      abandon(error, method, deadline_ms);
      return Status::timed_out;
    }
    if (next == Next::end) {
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
