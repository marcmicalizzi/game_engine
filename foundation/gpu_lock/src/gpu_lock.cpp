#include "lock_file.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/time/time.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace engine::gpu_lock {

namespace fs = std::filesystem;

namespace detail {

std::string environment(const char* name) {
#if ENGINE_COMPILER_MSVC
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

void set_environment(const char* name, const std::string& value) {
#if ENGINE_PLATFORM_WINDOWS
  // Both copies: the C runtime's, which getenv and _dupenv_s read, and the OS block, which is
  // what CreateProcess hands a child when it is given no environment of its own.
  (void)::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
  (void)_putenv_s(name, value.c_str());
#else
  if (value.empty()) {
    (void)::unsetenv(name);
  } else {
    (void)::setenv(name, value.c_str(), 1);
  }
#endif
}

u64 current_pid() noexcept {
#if ENGINE_PLATFORM_WINDOWS
  return static_cast<u64>(GetCurrentProcessId());
#else
  return static_cast<u64>(::getpid());
#endif
}

std::string current_host() {
#if ENGINE_PLATFORM_WINDOWS
  // What the PowerShell tools and GPU-LOCK.md's Python write: $env:COMPUTERNAME.
  return environment("COMPUTERNAME");
#else
  char name[256] = {};
  if (::gethostname(name, sizeof(name) - 1) != 0) return {};
  return std::string(name);
#endif
}

std::string executable_path() {
#if ENGINE_PLATFORM_WINDOWS
  char buffer[4096] = {};
  const DWORD n = ::GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (n == 0 || n >= sizeof(buffer)) return {};
  std::string path(buffer, n);
#else
  char buffer[4096] = {};
  const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n <= 0) return {};
  std::string path(buffer, static_cast<usize>(n));
#endif
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

bool read_text(const std::string& path, std::string& out, bool& exists) {
  std::error_code ec;
  exists = fs::exists(fs::path(path), ec);
  if (!exists) return false;
  std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
  (void)fopen_s(&f, path.c_str(), "rb");
#else
  f = std::fopen(path.c_str(), "rb");
#endif
  if (f == nullptr) return false;  // exists, but a writer has it exclusively
  out.clear();
  char buffer[4096];
  for (;;) {
    const usize n = std::fread(buffer, 1, sizeof(buffer), f);
    out.append(buffer, n);
    if (n < sizeof(buffer)) break;
  }
  std::fclose(f);
  return true;
}

// "x" is C11's exclusive mode (O_EXCL on POSIX, CREATE_NEW on Windows), so two processes can
// never both succeed.
CreateResult create_exclusive(const std::string& path, std::string_view body) {
  std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
  (void)fopen_s(&f, path.c_str(), "wbx");
#else
  f = std::fopen(path.c_str(), "wbx");
#endif
  if (f == nullptr) {
    std::error_code ec;
    return fs::exists(fs::path(path), ec) ? CreateResult::Exists : CreateResult::Failed;
  }
  const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
  const bool closed = std::fclose(f) == 0;
  return ok && closed ? CreateResult::Created : CreateResult::Failed;
}

namespace {

std::string unique_suffix() {
  std::random_device entropy;
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%llu-%08x", static_cast<unsigned long long>(current_pid()),
                entropy());
  return std::string(buffer);
}

// Moves `from` to `to` only if `to` does not exist: how a breaker puts back a live lock it took
// by mistake without clobbering one somebody created in the meantime.
bool rename_no_replace(const std::string& from, const std::string& to) {
#if ENGINE_PLATFORM_WINDOWS
  return MoveFileExA(from.c_str(), to.c_str(), 0) != 0;
#else
  if (::link(from.c_str(), to.c_str()) != 0) return false;
  (void)::unlink(from.c_str());
  return true;
#endif
}

}  // namespace

// std::filesystem::rename replaces an existing target on both platforms; on Windows it fails for
// the moment a reader has the file open, so it is retried briefly.
bool replace_atomically(const std::string& path, std::string_view body) {
  const std::string tmp = path + "." + unique_suffix() + ".tmp";
  {
    std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
    (void)fopen_s(&f, tmp.c_str(), "wb");
#else
    f = std::fopen(tmp.c_str(), "wb");
#endif
    if (f == nullptr) return false;
    const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    if (std::fclose(f) != 0 || !ok) {
      std::error_code ec;
      fs::remove(fs::path(tmp), ec);
      return false;
    }
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::error_code ec;
    fs::rename(fs::path(tmp), fs::path(path), ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::error_code ec;
  fs::remove(fs::path(tmp), ec);
  return false;
}

bool break_stale(const std::string& path, const std::string& judged) {
  const std::string grave = path + ".stale-" + unique_suffix();
  std::error_code ec;
  fs::rename(fs::path(path), fs::path(grave), ec);
  if (ec) return false;
  std::string moved;
  bool exists = false;
  (void)read_text(grave, moved, exists);
  if (moved == judged) {
    (void)remove_file(grave);
    return true;
  }
  if (!rename_no_replace(grave, path)) (void)remove_file(grave);
  return false;
}

bool remove_file(const std::string& path) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::error_code ec;
    const bool removed = fs::remove(fs::path(path), ec);
    if (!ec) return removed || !fs::exists(fs::path(path), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void append_line(const std::string& path, std::string_view line) {
  if (path.empty()) return;
  std::string text(line);
  text.push_back('\n');
  std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
  (void)fopen_s(&f, path.c_str(), "ab");
#else
  f = std::fopen(path.c_str(), "ab");
#endif
  if (f == nullptr) return;  // best effort: a log that cannot be written never stops a device
  (void)std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}

ParsedLock parse_lock(std::string_view text) {
  ParsedLock out;
  JsonValue root;
  if (!parse_json(text, root).ok || !root.is_object()) return out;
  out.readable = true;
  auto text_of = [&](const char* key, std::string& into) {
    const JsonValue* v = root.find(key);
    std::string_view s;
    if (v != nullptr && v->get_string(s)) into.assign(s);
  };
  text_of("owner", out.owner);
  text_of("purpose", out.purpose);
  text_of("started", out.started);
  text_of("expires", out.expires);
  text_of("host", out.host);
  if (const JsonValue* v = root.find("pid"); v != nullptr) {
    if (!v->get_u64(out.pid)) {
      std::string_view s;
      if (v->get_string(s)) {
        u64 value = 0;
        bool digits = !s.empty();
        for (const char c : s) {
          if (c < '0' || c > '9') {
            digits = false;
            break;
          }
          value = value * 10 + static_cast<u64>(c - '0');
        }
        if (digits) out.pid = value;
      }
    }
  }
  return out;
}

std::string lock_body(std::string_view owner, std::string_view purpose, u64 pid,
                      std::string_view started, i64 expires_unix_s, std::string_view host) {
  JsonValue body = JsonValue::object();
  body.set("owner", owner);
  body.set("purpose", purpose);
  body.set("pid", pid);
  body.set("started", started);
  body.set("expires", format_iso8601_utc(expires_unix_s));
  body.set("host", host);
  return write_json(body, JsonWriteOptions{.pretty = false});
}

// ---- releasing on a signal --------------------------------------------------------------------

namespace {

// A hold's path is here while it is armed, so SIGINT and SIGTERM can delete the file before the
// process dies. Only what a signal handler may touch lives here — a fixed buffer and atomics.
char g_signal_path[1024] = {};
std::atomic<i64> g_signal_expires{0};
std::atomic<bool> g_signal_armed{false};
using SignalHandler = void (*)(int);
SignalHandler g_previous_int = SIG_DFL;
SignalHandler g_previous_term = SIG_DFL;

void release_on_signal(int sig) {
  // Only while the lease has not run out: after that the file may already be somebody else's.
  if (g_signal_armed.load() && static_cast<i64>(std::time(nullptr)) < g_signal_expires.load()) {
#if ENGINE_PLATFORM_WINDOWS
    (void)DeleteFileA(g_signal_path);
#else
    (void)::unlink(g_signal_path);
#endif
  }
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

}  // namespace

void arm_signal_release(const std::string& path, i64 expires_unix_s) {
  if (path.size() >= sizeof(g_signal_path)) return;
  std::memcpy(g_signal_path, path.c_str(), path.size() + 1);
  g_signal_expires.store(expires_unix_s);
  if (!g_signal_armed.exchange(true)) {
    g_previous_int = std::signal(SIGINT, release_on_signal);
    g_previous_term = std::signal(SIGTERM, release_on_signal);
  }
}

void update_signal_expiry(i64 expires_unix_s) { g_signal_expires.store(expires_unix_s); }

void disarm_signal_release() {
  if (g_signal_armed.exchange(false)) {
    (void)std::signal(SIGINT, g_previous_int == SIG_ERR ? SIG_DFL : g_previous_int);
    (void)std::signal(SIGTERM, g_previous_term == SIG_ERR ? SIG_DFL : g_previous_term);
  }
}

i64 now_unix_s() { return time::wall_unix_ms() / 1000; }

}  // namespace detail

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
i64 days_from_civil(i64 y, i64 m, i64 d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const i64 yoe = y - era * 400;
  const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civil_from_days(i64 z, i64& y, i64& m, i64& d) noexcept {
  z += 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const i64 doe = z - era * 146097;
  const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const i64 mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}

i64 file_age_s(const std::string& path) {
  std::error_code ec;
  const auto written = fs::last_write_time(fs::path(path), ec);
  if (ec) return 0;
  const auto age = fs::file_time_type::clock::now() - written;
  return std::chrono::duration_cast<std::chrono::seconds>(age).count();
}

// The purpose is the holder's own one line; a very long one is somebody else's bug and is not
// worth carrying into every report.
constexpr usize k_max_purpose = 200;

}  // namespace

// ---- time ------------------------------------------------------------------------------------

bool parse_iso8601_utc(std::string_view text, i64& out_unix_s) noexcept {
  // YYYY-MM-DDTHH:MM:SS[.fraction][Z|+HH:MM|-HH:MM]
  auto digits = [&](usize at, usize count, i64& value) {
    if (at + count > text.size()) return false;
    value = 0;
    for (usize i = at; i < at + count; ++i) {
      if (text[i] < '0' || text[i] > '9') return false;
      value = value * 10 + (text[i] - '0');
    }
    return true;
  };
  i64 year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!digits(0, 4, year) || text.size() < 19 || text[4] != '-' || !digits(5, 2, month) ||
      text[7] != '-' || !digits(8, 2, day) || (text[10] != 'T' && text[10] != ' ') ||
      !digits(11, 2, hour) || text[13] != ':' || !digits(14, 2, minute) || text[16] != ':' ||
      !digits(17, 2, second)) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return false;
  }
  usize at = 19;
  if (at < text.size() && text[at] == '.') {
    ++at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9')
      ++at;
  }
  i64 offset_s = 0;
  if (at < text.size()) {
    if (text[at] == 'Z' || text[at] == 'z') {
      ++at;
    } else if (text[at] == '+' || text[at] == '-') {
      const i64 sign = text[at] == '-' ? -1 : 1;
      i64 oh = 0, om = 0;
      if (!digits(at + 1, 2, oh) || at + 3 >= text.size() || text[at + 3] != ':' ||
          !digits(at + 4, 2, om)) {
        return false;
      }
      offset_s = sign * (oh * 3600 + om * 60);
      at += 6;
    } else {
      return false;
    }
  }
  if (at != text.size()) return false;
  out_unix_s =
      days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offset_s;
  return true;
}

std::string format_iso8601_utc(i64 unix_s) {
  i64 days = unix_s / 86400;
  i64 rem = unix_s % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  i64 y = 0, m = 0, d = 0;
  civil_from_days(days, y, m, d);
  // Room for six i64 fields at their widest, not just for the 20 bytes a real date needs: GCC's
  // -Wformat-truncation reasons about the types, not the ranges, and warnings are errors.
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                static_cast<long long>(y), static_cast<long long>(m), static_cast<long long>(d),
                static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                static_cast<long long>(rem % 60));
  return std::string(buffer);
}

// ---- reading ---------------------------------------------------------------------------------

std::string default_path() {
  std::string configured = detail::environment("ENGINE_GPU_LOCK");
  if (!configured.empty()) return configured;
#if ENGINE_PLATFORM_WINDOWS
  return "D:\\workspace\\gpu.lock";
#else
  return "/tmp/gpu.lock";
#endif
}

Identity current_identity() {
  Identity self;
  self.owner = detail::environment("ENGINE_GPU_LOCK_OWNER");
  if (self.owner.empty()) self.owner = "claude-engine";
  self.pid = detail::current_pid();
  const std::string holder = detail::environment("ENGINE_GPU_LOCK_HOLDER");
  u64 value = 0;
  bool digits = !holder.empty();
  for (const char c : holder) {
    if (c < '0' || c > '9') {
      digits = false;
      break;
    }
    value = value * 10 + static_cast<u64>(c - '0');
  }
  self.holder_pid = digits ? value : 0;
  self.host = detail::current_host();
  return self;
}

State read(const std::string& path, const Identity& self, i64 now_unix_s) {
  State state;
  std::string text;
  bool exists = false;
  const bool got = detail::read_text(path, text, exists);
  if (!exists) return state;
  state.present = true;
  if (!got) return state;  // held open by a writer: being written, so held and not expired

  const detail::ParsedLock lock = detail::parse_lock(text);
  if (!lock.readable) {
    state.expired = file_age_s(path) > k_unreadable_stale_s;
    return state;
  }
  state.readable = true;
  state.pid = lock.pid;
  state.owner = lock.owner;
  state.purpose = lock.purpose.substr(0, std::min(lock.purpose.size(), k_max_purpose));
  state.started = lock.started;
  state.expires = lock.expires;
  i64 expires_s = 0;
  // An expiry nobody can read is a promise nobody made: expired.
  state.expired = !parse_iso8601_utc(lock.expires, expires_s) || expires_s < now_unix_s;
  const bool same_host = lock.host.empty() || self.host.empty() || lock.host == self.host;
  state.mine = same_host && lock.owner == self.owner && lock.pid != 0 &&
               (lock.pid == self.pid || (self.holder_pid != 0 && lock.pid == self.holder_pid));
  return state;
}

// ---- holding ---------------------------------------------------------------------------------

const char* to_string(Lease::Outcome outcome) noexcept {
  switch (outcome) {
    case Lease::Outcome::Taken: return "taken";
    case Lease::Outcome::AlreadyMine: return "already held for this process";
    case Lease::Outcome::NoDirectory: return "no lock directory";
    case Lease::Outcome::TimedOut: return "timed out";
    case Lease::Outcome::Error: return "error";
  }
  return "unknown";
}

Lease::~Lease() { release(); }

std::string Lease::body_json() const {
  return detail::lock_body(config_.self.owner, config_.purpose, config_.self.pid, started_,
                           expires_s_, config_.self.host);
}

Lease::Outcome Lease::acquire(const Config& config) {
  release();
  config_ = config;
  path_ = config.path.empty() ? default_path() : config.path;

  const fs::path parent = fs::path(path_).parent_path();
  std::error_code ec;
  if (!parent.empty() && !fs::is_directory(parent, ec)) return Outcome::NoDirectory;

  const i64 began_ns = time::monotonic_ns();
  const i64 timeout_ns = std::max<i64>(config.timeout_s, 0) * i64{1'000'000'000};
  std::string announced;
  for (;;) {
    const i64 now_s = detail::now_unix_s();
    started_ = format_iso8601_utc(now_s);
    expires_s_ = now_s + std::max<i64>(config.lease_s, 1);
    switch (detail::create_exclusive(path_, body_json())) {
      case detail::CreateResult::Created:
        held_ = true;
        detail::arm_signal_release(path_, expires_s_);
        if (!announced.empty() && config.log != nullptr) {
          std::fprintf(config.log, "bench: gpu lock taken after %.0f s\n",
                       static_cast<f64>(time::monotonic_ns() - began_ns) / 1e9);
        }
        return Outcome::Taken;
      case detail::CreateResult::Failed: return Outcome::Error;
      case detail::CreateResult::Exists: break;
    }

    std::string text;
    bool exists = false;
    const bool got = detail::read_text(path_, text, exists);
    if (!exists) continue;  // released between the attempt and the look: try again at once
    const State seen = read(path_, config.self, now_s);
    // This process's own, or held for it by a wrapper that is still refreshing it. A wrapper's
    // lock that has expired is a wrapper that died: it is broken like anybody else's below.
    if (seen.mine && (seen.pid == config.self.pid || !seen.expired)) return Outcome::AlreadyMine;
    if (got && seen.expired && seen.owner != "marc") {
      if (config.log != nullptr) {
        std::fprintf(config.log, "bench: breaking an expired gpu lock held by '%s' (%s)\n",
                     seen.owner.c_str(), seen.purpose.c_str());
      }
      if (detail::break_stale(path_, text)) continue;
    }

    const i64 waited_ns = time::monotonic_ns() - began_ns;
    if (waited_ns >= timeout_ns) return Outcome::TimedOut;
    // Once per holder, not once per refresh: a holder's heartbeat moves `expires`, and a line per
    // move would bury the one that says somebody new has the GPU.
    const std::string holder = seen.readable ? seen.owner + "\n" + seen.purpose : std::string("\n");
    if (config.log != nullptr && holder != announced) {
      if (seen.readable) {
        std::fprintf(config.log, "bench: waiting for the gpu lock, held by '%s': %s (until %s)\n",
                     seen.owner.c_str(), seen.purpose.c_str(), seen.expires.c_str());
      } else {
        std::fprintf(config.log, "bench: waiting for the gpu lock (its file is being written)\n");
      }
      announced = holder;
    }
    const i64 sleep_ns =
        std::min(timeout_ns - waited_ns, std::max<i64>(config.poll_s, 1) * i64{1'000'000'000});
    std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
  }
}

void Lease::refresh_if_due() {
  if (!held_) return;
  const i64 now_s = detail::now_unix_s();
  const i64 lease = std::max<i64>(config_.lease_s, 1);
  if (expires_s_ - now_s > lease / 2) return;
  // Only a file that is still ours is refreshed; one somebody broke after it expired is theirs.
  const State seen = read(path_, config_.self, now_s);
  if (!seen.present || !seen.readable || !seen.mine) {
    held_ = false;
    detail::disarm_signal_release();
    return;
  }
  const i64 previous = expires_s_;
  expires_s_ = now_s + lease;
  if (detail::replace_atomically(path_, body_json())) {
    detail::update_signal_expiry(expires_s_);
  } else {
    expires_s_ = previous;  // retried at the next call
  }
}

void Lease::release() noexcept {
  if (!held_) return;
  held_ = false;
  detail::disarm_signal_release();
  std::string text;
  bool exists = false;
  if (!detail::read_text(path_, text, exists)) return;
  const detail::ParsedLock lock = detail::parse_lock(text);
  // The file this lease wrote, and nobody else's: owner, pid and start time all match.
  if (lock.readable && lock.owner == config_.self.owner && lock.pid == config_.self.pid &&
      lock.started == started_) {
    (void)detail::remove_file(path_);
  }
}

}  // namespace engine::gpu_lock
