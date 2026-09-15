#include <core/base/assert.h>
#include <core/log/log.h>
#include <core/platform/spin_lock.h>
#include <core/platform/thread.h>
#include <core/time/time.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <shared_mutex>

namespace engine::log {

namespace {

constexpr Level k_default_global_level = ENGINE_DEBUG ? Level::Debug : Level::Info;

// Constant-initialized so categories constructed during static initialization find it ready.
std::atomic<Level> g_global_min_level{k_default_global_level};

// A tiny spinlock guards the category chain and the accumulated level spec. Both are touched
// at static-initialization time, when function-local statics with non-trivial constructors
// would be a hazard; an atomic flag has constant initialization.
class SpinLock {
 public:
  void lock() noexcept {
    while (flag_.exchange(true, std::memory_order_acquire)) {
      while (flag_.load(std::memory_order_relaxed))
        platform::pause_cpu();
    }
  }
  void unlock() noexcept { flag_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> flag_{false};
};

constexpr usize k_spec_capacity = 1024;

// A spinlock guards the category chain and the accumulated level spec: both are touched
// during static initialization, where a mutex global would be a construction-order hazard.
struct RegistryState {
  platform::SpinLock lock;
  Category* head = nullptr;
  char spec[k_spec_capacity] = {};
  usize spec_size = 0;
};

RegistryState g_registry;

struct SpecEntry {
  std::string_view pattern;  // empty: global level
  Level level;
};

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

// Parses one `name=level` or `level` entry. Returns false when malformed.
bool parse_entry(std::string_view entry, SpecEntry& out) noexcept {
  entry = trim(entry);
  if (entry.empty()) return false;
  const usize eq = entry.find('=');
  if (eq == std::string_view::npos) {
    out.pattern = {};
    return parse_level(entry, out.level);
  }
  out.pattern = trim(entry.substr(0, eq));
  if (out.pattern.empty()) return false;
  return parse_level(trim(entry.substr(eq + 1)), out.level);
}

bool pattern_matches(std::string_view pattern, std::string_view name) noexcept {
  if (!pattern.empty() && pattern.back() == '*') {
    pattern.remove_suffix(1);
    return name.substr(0, pattern.size()) == pattern;
  }
  return pattern == name;
}

// Applies every entry of the accumulated spec to one category. Caller holds the registry lock.
void apply_spec_to(Category& category) noexcept {
  std::string_view spec{g_registry.spec, g_registry.spec_size};
  while (!spec.empty()) {
    const usize comma = spec.find(',');
    const std::string_view entry = spec.substr(0, comma);
    spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
    SpecEntry e{};
    if (!parse_entry(entry, e) || e.pattern.empty()) continue;  // stored entries are valid
    if (pattern_matches(e.pattern, category.name())) category.set_min_level(e.level);
  }
}

// Sinks live behind a function-local static: emit is never called before main in practice,
// and if it is, the table constructs itself on first use.
struct SinkTable {
  std::shared_mutex mutex;
  Sink* sinks[k_max_sinks] = {};
  usize count = 0;
};

SinkTable& sinks() {
  static SinkTable table;
  return table;
}

std::string& scratch() {
  thread_local std::string buffer;
  buffer.clear();
  return buffer;
}

// ---- formatting helpers ------------------------------------------------------------------

void append_uint(std::string& out, u64 v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

void append_int(std::string& out, i64 v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

// Shortest round-trip representation; JSON-compatible ("1e+20", not "1e20"). Non-finite
// values are handled by the caller.
void append_float(std::string& out, f64 v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

void append_hex(std::string& out, u64 v, int digits) {
  static constexpr char k_digits[] = "0123456789abcdef";
  for (int i = digits - 1; i >= 0; --i)
    out.push_back(k_digits[(v >> (i * 4)) & 0xf]);
}

void append_padded_uint(std::string& out, u64 v, int width) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  const int n = static_cast<int>(r.ptr - buf);
  for (int i = n; i < width; ++i)
    out.push_back('0');
  out.append(buf, static_cast<usize>(n));
}

void append_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      default:
        if (uc < 0x20) {
          out.append("\\u00");
          append_hex(out, uc, 2);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

bool needs_quotes(std::string_view s) noexcept {
  if (s.empty()) return true;
  for (const char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc <= 0x20 || c == '"' || c == '=' || c == '\\') return true;
  }
  return false;
}

void append_value_text(std::string& out, const Field& f) {
  switch (f.kind) {
    case Field::Kind::Bool: out.append(f.value.b ? "true" : "false"); break;
    case Field::Kind::Int: append_int(out, f.value.i); break;
    case Field::Kind::Uint: append_uint(out, f.value.u); break;
    case Field::Kind::Float:
      if (std::isfinite(f.value.f)) {
        append_float(out, f.value.f);
      } else {
        out.append(std::isnan(f.value.f) ? "nan" : (f.value.f > 0 ? "inf" : "-inf"));
      }
      break;
    case Field::Kind::String: {
      const std::string_view s = f.string();
      if (needs_quotes(s)) {
        append_json_string(out, s);
      } else {
        out.append(s);
      }
      break;
    }
    case Field::Kind::Pointer:
      out.append("0x");
      append_hex(out, reinterpret_cast<u64>(f.value.p), 16);
      break;
    case Field::Kind::Hex128:
      append_hex(out, f.value.h.hi, 16);
      append_hex(out, f.value.h.lo, 16);
      break;
  }
}

void append_value_json(std::string& out, const Field& f) {
  switch (f.kind) {
    case Field::Kind::Bool: out.append(f.value.b ? "true" : "false"); break;
    case Field::Kind::Int: append_int(out, f.value.i); break;
    case Field::Kind::Uint: append_uint(out, f.value.u); break;
    case Field::Kind::Float:
      if (std::isfinite(f.value.f)) {
        append_float(out, f.value.f);
      } else {
        out.append("null");
      }
      break;
    case Field::Kind::String: append_json_string(out, f.string()); break;
    case Field::Kind::Pointer:
      out.append("\"0x");
      append_hex(out, reinterpret_cast<u64>(f.value.p), 16);
      out.push_back('"');
      break;
    case Field::Kind::Hex128:
      out.push_back('"');
      append_hex(out, f.value.h.hi, 16);
      append_hex(out, f.value.h.lo, 16);
      out.push_back('"');
      break;
  }
}

void write_default_stderr(const Record& record) {
  std::string& line = scratch();
  format_text(record, line);
  line.push_back('\n');
  std::fwrite(line.data(), 1, line.size(), stderr);
  if (record.level >= Level::Error) std::fflush(stderr);
}

void assert_hook(const char* expression, const char* message, const char* file, int line) noexcept {
  static Category category{"engine.assert"};
  const Field fields[] = {field("expression", expression != nullptr ? expression : "")};
  emit(category, Level::Fatal, file, static_cast<u32>(line),
       message != nullptr && message[0] != '\0' ? message : "assertion failed", fields);
}

}  // namespace

// ---- levels --------------------------------------------------------------------------------

const char* level_name(Level level) noexcept {
  switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    case Level::Off: return "off";
  }
  return "?";
}

bool parse_level(std::string_view text, Level& out) noexcept {
  char lower[8] = {};
  if (text.empty() || text.size() >= sizeof(lower)) return false;
  for (usize i = 0; i < text.size(); ++i) {
    const char c = text[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  const std::string_view t{lower, text.size()};
  if (t == "trace")
    out = Level::Trace;
  else if (t == "debug")
    out = Level::Debug;
  else if (t == "info")
    out = Level::Info;
  else if (t == "warn" || t == "warning")
    out = Level::Warn;
  else if (t == "error" || t == "err")
    out = Level::Error;
  else if (t == "fatal")
    out = Level::Fatal;
  else if (t == "off" || t == "none")
    out = Level::Off;
  else
    return false;
  return true;
}

Level global_min_level() noexcept {
  return g_global_min_level.load(std::memory_order_relaxed);
}
void set_global_min_level(Level level) noexcept {
  g_global_min_level.store(level, std::memory_order_relaxed);
}

// ---- categories ----------------------------------------------------------------------------

Category::Category(const char* name) noexcept : name_(name) {
  std::lock_guard lock(g_registry.lock);
  next_ = g_registry.head;
  g_registry.head = this;
  apply_spec_to(*this);
}

Category::~Category() {
  std::lock_guard lock(g_registry.lock);
  Category** link = &g_registry.head;
  while (*link != nullptr && *link != this)
    link = &(*link)->next_;
  if (*link == this) *link = next_;
}

Category* first_category() noexcept {
  std::lock_guard lock(g_registry.lock);
  return g_registry.head;
}

Category* find_category(std::string_view name) noexcept {
  std::lock_guard lock(g_registry.lock);
  for (Category* c = g_registry.head; c != nullptr; c = c->next()) {
    if (name == c->name()) return c;
  }
  return nullptr;
}

bool apply_level_spec(std::string_view spec, std::string* error) {
  std::lock_guard lock(g_registry.lock);
  std::string_view rest = spec;
  while (!rest.empty()) {
    const usize comma = rest.find(',');
    const std::string_view raw = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    const std::string_view entry = trim(raw);
    if (entry.empty()) continue;
    SpecEntry e{};
    if (!parse_entry(entry, e)) {
      if (error != nullptr) {
        error->assign("malformed log level entry '");
        error->append(entry);
        error->append("'; expected 'level' or 'category=level'");
      }
      return false;
    }
    if (e.pattern.empty()) {
      g_global_min_level.store(e.level, std::memory_order_relaxed);
      continue;
    }
    // Remember for categories registered later.
    const usize needed = g_registry.spec_size + entry.size() + (g_registry.spec_size > 0 ? 1 : 0);
    if (needed > k_spec_capacity) {
      if (error != nullptr) error->assign("log level spec exceeds the accumulated capacity");
      return false;
    }
    if (g_registry.spec_size > 0) g_registry.spec[g_registry.spec_size++] = ',';
    std::memcpy(g_registry.spec + g_registry.spec_size, entry.data(), entry.size());
    g_registry.spec_size += entry.size();
    for (Category* c = g_registry.head; c != nullptr; c = c->next()) {
      if (pattern_matches(e.pattern, c->name())) c->set_min_level(e.level);
    }
  }
  return true;
}

void reset_levels() noexcept {
  std::lock_guard lock(g_registry.lock);
  g_registry.spec_size = 0;
  g_global_min_level.store(k_default_global_level, std::memory_order_relaxed);
  for (Category* c = g_registry.head; c != nullptr; c = c->next())
    c->set_min_level(Level::Trace);
}

// ---- formatting ----------------------------------------------------------------------------

void format_text(const Record& record, std::string& out) {
  // UTC wall clock: HH:MM:SS.mmm.
  const i64 us = record.wall_unix_us;
  const i64 total_seconds = us >= 0 ? us / 1'000'000 : -((-us + 999'999) / 1'000'000);
  const i64 ms = (us - total_seconds * 1'000'000) / 1000;
  const i64 day_seconds = ((total_seconds % 86'400) + 86'400) % 86'400;
  append_padded_uint(out, static_cast<u64>(day_seconds / 3600), 2);
  out.push_back(':');
  append_padded_uint(out, static_cast<u64>((day_seconds / 60) % 60), 2);
  out.push_back(':');
  append_padded_uint(out, static_cast<u64>(day_seconds % 60), 2);
  out.push_back('.');
  append_padded_uint(out, static_cast<u64>(ms), 3);
  out.push_back(' ');

  const char* level = level_name(record.level);
  out.append(level);
  for (usize n = std::strlen(level); n < 5; ++n)
    out.push_back(' ');
  out.push_back(' ');
  out.append(record.category != nullptr ? record.category->name() : "-");
  out.append(" [t");
  append_uint(out, record.thread_index);
  out.append("] ");
  out.append(record.message);
  for (const Field& f : record.fields) {
    out.push_back(' ');
    out.append(f.key);
    out.push_back('=');
    append_value_text(out, f);
  }
}

void format_json_line(const Record& record, std::string& out) {
  out.append("{\"t\":");
  append_int(out, record.monotonic_ns);
  out.append(",\"wall\":");
  append_int(out, record.wall_unix_us);
  out.append(",\"level\":\"");
  out.append(level_name(record.level));
  out.append("\",\"cat\":");
  append_json_string(out, record.category != nullptr ? record.category->name() : "");
  out.append(",\"thread\":");
  append_uint(out, record.thread_index);
  out.append(",\"msg\":");
  append_json_string(out, record.message);
  if (record.file != nullptr) {
    out.append(",\"file\":");
    append_json_string(out, record.file);
    out.append(",\"line\":");
    append_uint(out, record.line);
  }
  if (!record.fields.empty()) {
    out.append(",\"fields\":{");
    bool first = true;
    for (const Field& f : record.fields) {
      if (!first) out.push_back(',');
      first = false;
      append_json_string(out, f.key);
      out.push_back(':');
      append_value_json(out, f);
    }
    out.push_back('}');
  }
  out.push_back('}');
}

// ---- sinks and emission ----------------------------------------------------------------------

void add_sink(Sink* sink) {
  ENGINE_VERIFY(sink != nullptr, "log::add_sink: null sink");
  SinkTable& t = sinks();
  std::unique_lock lock(t.mutex);
  for (usize i = 0; i < t.count; ++i) {
    if (t.sinks[i] == sink) return;
  }
  ENGINE_VERIFY(t.count < k_max_sinks, "log::add_sink: k_max_sinks exceeded; raise the budget");
  t.sinks[t.count++] = sink;
}

void remove_sink(Sink* sink) {
  SinkTable& t = sinks();
  std::unique_lock lock(t.mutex);
  for (usize i = 0; i < t.count; ++i) {
    if (t.sinks[i] == sink) {
      for (usize j = i + 1; j < t.count; ++j)
        t.sinks[j - 1] = t.sinks[j];
      t.sinks[--t.count] = nullptr;
      return;
    }
  }
}

usize sink_count() noexcept {
  SinkTable& t = sinks();
  std::shared_lock lock(t.mutex);
  return t.count;
}

void flush() {
  SinkTable& t = sinks();
  std::shared_lock lock(t.mutex);
  for (usize i = 0; i < t.count; ++i)
    t.sinks[i]->flush();
  if (t.count == 0) std::fflush(stderr);
}

void emit(const Category& category, Level level, const char* file, u32 line,
          std::string_view message, std::span<const Field> fields) noexcept {
  Record record{};
  record.monotonic_ns = time::monotonic_ns();
  record.wall_unix_us = time::wall_unix_us();
  record.category = &category;
  record.message = message;
  record.fields = fields;
  record.file = file;
  record.line = line;
  record.thread_index = platform::current_thread_index();
  record.level = level;

  SinkTable& t = sinks();
  std::shared_lock lock(t.mutex);
  if (t.count == 0) {
    write_default_stderr(record);
    return;
  }
  for (usize i = 0; i < t.count; ++i) {
    Sink* sink = t.sinks[i];
    if (level >= sink->min_level()) sink->write(record);
  }
  if (level == Level::Fatal) {
    for (usize i = 0; i < t.count; ++i)
      t.sinks[i]->flush();
  }
}

void fatal_terminate(std::string_view message, const char* file, u32 line) noexcept {
  flush();
  char buf[512];
  const usize n = std::min(message.size(), sizeof(buf) - 1);
  std::memcpy(buf, message.data(), n);
  buf[n] = '\0';
  // The record has already been emitted; keep the assertion path from emitting it again.
  set_assert_hook(nullptr);
  assert_fail("ENGINE_LOG_FATAL", buf, file, static_cast<int>(line));
}

void install_assert_hook() noexcept {
  set_assert_hook(&assert_hook);
}

// ---- StreamSink ----------------------------------------------------------------------------

StreamSink::StreamSink(std::FILE* stream, Format format, bool close_on_destroy) noexcept
    : stream_(stream), format_(format), close_on_destroy_(close_on_destroy) {
  ENGINE_VERIFY(stream != nullptr, "StreamSink: null stream");
}

StreamSink::~StreamSink() {
  std::fflush(stream_);
  if (close_on_destroy_) std::fclose(stream_);
}

void StreamSink::write(const Record& record) {
  std::string& line = scratch();
  if (format_ == Format::Text) {
    format_text(record, line);
  } else {
    format_json_line(record, line);
  }
  line.push_back('\n');
  // One fwrite per record: the C runtime serializes concurrent writers per stream.
  std::fwrite(line.data(), 1, line.size(), stream_);
  if (record.level >= Level::Error) std::fflush(stream_);
}

void StreamSink::flush() {
  std::fflush(stream_);
}

// ---- RingSink ------------------------------------------------------------------------------

RingSink::RingSink(u32 capacity) : slots_(capacity) {
  ENGINE_VERIFY(capacity > 0, "RingSink: capacity must be positive");
}

void RingSink::write(const Record& record) {
  std::lock_guard lock(mutex_);
  Stored* slot;
  if (count_ < slots_.size()) {
    slot = &slots_[(head_ + count_) % slots_.size()];
    ++count_;
  } else {
    slot = &slots_[head_];
    head_ = (head_ + 1) % slots_.size();
  }
  Stored& s = *slot;
  s.sequence = next_sequence_++;
  s.monotonic_ns = record.monotonic_ns;
  s.wall_unix_us = record.wall_unix_us;
  s.category = record.category;
  s.file = record.file;
  s.line = record.line;
  s.thread_index = record.thread_index;
  s.level = record.level;
  s.message_size = static_cast<u32>(record.message.size());

  // Size the blob once so the views built below stay valid.
  usize bytes = record.message.size();
  for (const Field& f : record.fields) {
    bytes += f.key.size();
    if (f.kind == Field::Kind::String) bytes += f.string().size();
  }
  s.blob.clear();
  s.blob.reserve(bytes);
  s.blob.append(record.message);
  s.fields.clear();
  s.fields.reserve(static_cast<u32>(record.fields.size()));
  for (const Field& f : record.fields) {
    Field copy = f;
    const usize key_at = s.blob.size();
    s.blob.append(f.key);
    copy.key = std::string_view{s.blob.data() + key_at, f.key.size()};
    if (f.kind == Field::Kind::String) {
      const usize at = s.blob.size();
      s.blob.append(f.string());
      copy.value.s = {s.blob.data() + at, f.string().size()};
    }
    s.fields.push_back(copy);
  }
}

usize RingSink::size() const noexcept {
  std::lock_guard lock(mutex_);
  return count_;
}

u64 RingSink::next_sequence() const noexcept {
  std::lock_guard lock(mutex_);
  return next_sequence_;
}

u64 RingSink::first_sequence() const noexcept {
  std::lock_guard lock(mutex_);
  return count_ == 0 ? next_sequence_ : slots_[head_].sequence;
}

void RingSink::clear() noexcept {
  std::lock_guard lock(mutex_);
  head_ = 0;
  count_ = 0;
}

RingSink::Entry RingSink::view_of(const Stored& s) noexcept {
  Entry e{};
  e.sequence = s.sequence;
  e.monotonic_ns = s.monotonic_ns;
  e.wall_unix_us = s.wall_unix_us;
  e.category = s.category;
  e.message = std::string_view{s.blob.data(), s.message_size};
  e.fields = std::span<const Field>{s.fields.data(), s.fields.size()};
  e.file = s.file;
  e.line = s.line;
  e.thread_index = s.thread_index;
  e.level = s.level;
  return e;
}

}  // namespace engine::log
