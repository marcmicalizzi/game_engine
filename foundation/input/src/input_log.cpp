#include <core/base/assert.h>
#include <core/json/json.h>
#include <core/json/json_format.h>
#include <core/log/log.h>
#include <foundation/input/input_log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <share.h>
#endif

namespace engine::input {

ENGINE_LOG_CATEGORY_DEFINE(log_input, "input.log");

namespace {

// **The value a line carries** (T216's review, L6): a finite float as it is; a NaN as 0, and an
// infinity as the largest float of its sign — the loader refuses `null` and anything past a
// float's range, so a non-finite value written as it is would make the whole log unloadable. The
// save and the journal both write it through this, so the two spell every value alike.
f64 line_value(f32 value) noexcept {
  if (std::isnan(value)) return 0.0;
  if (std::isinf(value))
    return static_cast<f64>(value > 0.0f ? std::numeric_limits<f32>::max()
                                         : -std::numeric_limits<f32>::max());
  return static_cast<f64>(value);
}

// One event per line as [tick, source, code, value, device]: the tick sorts first, and an
// array costs a fifth of what the same fields cost as an object in a log of a million events.
JsonValue event_to_json(const RawEvent& event) {
  JsonValue::Array row;
  row.push_back(JsonValue(event.tick.value));
  row.push_back(JsonValue(source_name(event.source)));
  row.push_back(JsonValue(static_cast<u64>(event.code)));
  row.push_back(JsonValue(line_value(event.value)));
  row.push_back(JsonValue(static_cast<u64>(event.device)));
  return JsonValue(std::move(row));
}

bool event_from_json(const JsonValue& row, RawEvent& out, std::string* error) {
  if (!row.is_array() || row.size() != 5) {
    if (error != nullptr) *error = "input log: an event line is an array of five values";
    return false;
  }
  u64 tick = 0;
  std::string_view source;
  u64 code = 0;
  f64 value = 0.0;
  u64 device = 0;
  if (!row[0].get_u64(tick) || !row[1].get_string(source) || !row[2].get_u64(code) ||
      !row[3].get_f64(value) || !row[4].get_u64(device)) {
    if (error != nullptr) *error = "input log: an event field has the wrong type";
    return false;
  }
  if (!source_from_name(source, out.source)) {
    if (error != nullptr) *error = "input log: unknown source '" + std::string(source) + "'";
    return false;
  }
  if (code > 0xFFFFFFFFull || device > 0xFFFFFFFFull) {
    if (error != nullptr) *error = "input log: a code or device does not fit in 32 bits";
    return false;
  }
  // **A value past a 32-bit float's range is refused** (T190, as `json_reflect`'s `f32_of` since
  // T187): the cast of a double past FLT_MAX is undefined behaviour, in practice an infinity an
  // axis would then hand to every action bound to it. The parser refuses a non-finite double, so
  // the range is the one check; within it the cast rounds as it always did, so every recording
  // replays to the same bits.
  constexpr f64 k_max_value = static_cast<f64>(std::numeric_limits<f32>::max());
  if (!(value >= -k_max_value && value <= k_max_value)) {
    if (error != nullptr)
      *error = "input log: an event value is past a 32-bit float's range (about 3.4e38)";
    return false;
  }
  out.tick = SimTick{tick};
  out.code = static_cast<u32>(code);
  out.value = static_cast<f32>(value);
  out.device = static_cast<u32>(device);
  return true;
}

}  // namespace

void InputLog::record(const RawEvent& event) {
  ENGINE_ASSERT(events_.empty() || events_.back().tick <= event.tick,
                "input::InputLog::record: ticks must not go backwards");
  events_.push_back(event);
}

void InputLog::clear() noexcept { events_.clear(); }

// A load that fails leaves nothing of the file behind: not its events, and not the map hash or
// session its header got as far as setting.
void InputLog::forget() noexcept {
  events_.clear();
  map_hash_ = 0;
  session_ = JsonValue();
  through_ = SimTick{0};
  has_through_ = false;
  dropped_ = DroppedTail{};
}

SimTick InputLog::first_tick() const noexcept {
  return events_.empty() ? SimTick{1} : events_.front().tick;
}

SimTick InputLog::last_tick() const noexcept {
  return events_.empty() ? SimTick{0} : events_.back().tick;
}

namespace {

JsonValue header_json(u64 map_hash, const JsonValue& session, u32 version) {
  JsonValue header = JsonValue::object();
  header.set("type", k_log_type);
  header.set("version", static_cast<u64>(version));
  header.set("map", map_hash);
  JsonValue::Array columns;
  columns.push_back(JsonValue("tick"));
  columns.push_back(JsonValue("source"));
  columns.push_back(JsonValue("code"));
  columns.push_back(JsonValue("value"));
  columns.push_back(JsonValue("device"));
  header.set("columns", JsonValue(std::move(columns)));
  // Only when there is one, so a log recorded without a session is the bytes it always was.
  if (!session.is_null()) header.set("session", session);
  return header;
}

}  // namespace

io::Status InputLog::save(std::string_view native_path) const {
  const JsonWriteOptions compact{.pretty = false};
  std::string text = write_json(header_json(map_hash_, session_, k_saved_log_version), compact);
  text.push_back('\n');
  for (const RawEvent& event : events_) {
    text += write_json(event_to_json(event), compact);
    text.push_back('\n');
  }
  return io::write_file_atomic(native_path, text);
}

io::Status InputLog::load(std::string_view native_path, std::string* error) {
  clear();
  map_hash_ = 0;
  session_ = JsonValue();
  through_ = SimTick{0};
  has_through_ = false;
  dropped_ = DroppedTail{};
  u64 version = 0;
  std::string text;
  const io::Status status = io::read_file(native_path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) {
      *error = std::string("input log: cannot read '") + std::string(native_path) +
               "': " + io::status_name(status);
    }
    return status;
  }

  bool have_header = false;
  usize begin = 0;
  // A journal's (version 2) last whole through line: the events read by then and the byte after
  // it. Everything past it is a tail the session had not vouched for (T216's review, M1).
  u32 kept_events = 0;
  usize kept_bytes = 0;
  bool torn = false;
  while (begin <= text.size()) {
    usize end = text.find('\n', begin);
    const bool terminated = end != std::string::npos;
    if (!terminated) end = text.size();
    std::string_view line(text.data() + begin, end - begin);
    begin = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.remove_suffix(1);
    if (line.empty()) continue;
    // A journal writes every line whole with its newline, so an unterminated last line is one a
    // process ended in the middle of: never parsed, so a number cut short is never read as a
    // smaller one.
    const bool journal = have_header && version >= 2;
    if (journal && !terminated) {
      torn = true;
      break;
    }

    JsonValue value;
    if (const JsonParseResult parsed = parse_json(line, value); !parsed.ok) {
      // A journal's last line that does not parse, with nothing after it, is the same cut.
      // Anything else that does not parse is a broken file, refused as it always was.
      if (journal &&
          (begin >= text.size() || text.find_first_not_of(" \r\n", begin) == std::string::npos)) {
        torn = true;
        break;
      }
      if (error != nullptr) *error = std::string("input log: ") + parsed.message;
      forget();
      return io::Status::IoError;
    }
    if (!have_header) {
      std::string_view type;
      const JsonValue* type_value = value.find("type");
      if (!value.is_object() || type_value == nullptr || !type_value->get_string(type) ||
          type != k_log_type) {
        if (error != nullptr) *error = "input log: the first line is not an input log header";
        forget();
        return io::Status::IoError;
      }
      const JsonValue* version_value = value.find("version");
      if (version_value == nullptr || !version_value->get_u64(version) || version > k_log_version) {
        if (error != nullptr) {
          *error = "input log: version " + std::to_string(version) + " is newer than " +
                   std::to_string(k_log_version);
        }
        forget();
        return io::Status::IoError;
      }
      const JsonValue* map_value = value.find("map");
      if (map_value == nullptr || !map_value->get_u64(map_hash_)) {
        if (error != nullptr) *error = "input log: the header has no action map hash";
        forget();
        return io::Status::IoError;
      }
      if (const JsonValue* session = value.find("session"); session != nullptr) {
        if (!session->is_object()) {
          if (error != nullptr) *error = "input log: the header's session is not an object";
          forget();
          return io::Status::IoError;
        }
        session_ = *session;
      }
      have_header = true;
      kept_bytes = begin < text.size() ? begin : text.size();
      continue;
    }
    // A journal's through line (version 2, T216): the tick its recorder had reached. Never
    // backwards, and never before an event already read.
    if (value.is_object()) {
      const JsonValue* through = value.find("through");
      u64 tick = 0;
      if (version < 2 || through == nullptr || value.size() != 1 || !through->get_u64(tick)) {
        if (error != nullptr) *error = "input log: an event line is an array of five values";
        forget();
        return io::Status::IoError;
      }
      if ((has_through_ && tick < through_.value) ||
          (!events_.empty() && tick < events_.back().tick.value)) {
        if (error != nullptr) *error = "input log: a through line goes back in time";
        forget();
        return io::Status::IoError;
      }
      through_ = SimTick{tick};
      has_through_ = true;
      kept_events = events_.size();
      kept_bytes = begin < text.size() ? begin : text.size();
      continue;
    }
    RawEvent event;
    if (!event_from_json(value, event, error)) {
      forget();
      return io::Status::IoError;
    }
    if (!events_.empty() && event.tick < events_.back().tick) {
      if (error != nullptr) *error = "input log: the events are not in tick order";
      forget();
      return io::Status::IoError;
    }
    // **The span a file can make a replay walk is bounded** (T190): `replay` and `feed_ticks` step
    // every tick from the first to the last, the empty ones included, so two events at ticks 0 and
    // 2^63 made a replay of the log's own range run for ever. The bound is on the span, not on the
    // tick, so a log a long-running world records late in its life still loads.
    if (!events_.empty() && event.tick.value - events_.front().tick.value > k_max_log_tick_span) {
      if (error != nullptr) {
        *error = "input log: the events span " +
                 std::to_string(event.tick.value - events_.front().tick.value) +
                 " ticks; at most " + std::to_string(k_max_log_tick_span) +
                 " (seven days at 240 Hz) are replayed";
      }
      forget();
      return io::Status::IoError;
    }
    events_.push_back(event);
  }
  if (!have_header) {
    if (error != nullptr) *error = "input log: the file is empty";
    return io::Status::IoError;
  }
  // A journal stops at its last whole through line: a cut line and the events after that line are
  // dropped, said by name, and never replayed (T216's review, M1). A whole save (version 1) has
  // no through lines and keeps every event.
  if (version >= 2 && (torn || events_.size() > kept_events)) {
    dropped_.dropped = true;
    dropped_.through = through_;
    dropped_.events = events_.size() - kept_events;
    dropped_.bytes = text.size() - kept_bytes;
    events_.resize(kept_events);
    ENGINE_LOG_WARN(log_input, "input log: a journal's torn tail was dropped",
                    log::field("path", native_path), log::field("through", dropped_.through.value),
                    log::field("dropped_events", dropped_.events),
                    log::field("dropped_bytes", dropped_.bytes));
  }
  return io::Status::Ok;
}

u32 feed_ticks(InputState& state, std::span<const RawEvent> events, u32 cursor, SimTick from,
               SimTick to, const ReplayOptions& options) {
  const u32 count = static_cast<u32>(events.size());
  while (cursor < count && events[cursor].tick < from)
    ++cursor;
  if (to < from) return cursor;
  for (SimTick t = from;; ++t) {
    state.begin_tick(t);
    while (cursor < count && events[cursor].tick == t)
      state.feed(events[cursor++]);
    state.end_tick();
    if (options.on_tick != nullptr) options.on_tick(options.user, t, state);
    if (t == to) break;
  }
  return cursor;
}

bool InputLog::check_map(const InputState& state, std::string* error) const {
  const ActionMap* bound_map = state.map();
  if (bound_map == nullptr) {
    if (error != nullptr) *error = "input log: replay needs an InputState with an action map";
    return false;
  }
  const u64 actual = bound_map->hash();
  if (actual != map_hash_) {
    if (error != nullptr) {
      *error = "input log: recorded against action map " + std::to_string(map_hash_) +
               ", replayed against " + std::to_string(actual) + "; rebinding invalidates a replay";
    }
    return false;
  }
  return true;
}

bool InputLog::replay(InputState& state, SimTick from, SimTick to, const ReplayOptions& options,
                      std::string* error) const {
  if (!check_map(state, error)) return false;
  (void)feed_ticks(state, events(), 0, from, to, options);
  return true;
}

// ---- the journal (T216)
// --------------------------------------------------------------------------

InputLogJournal::~InputLogJournal() { close(); }

io::Status InputLogJournal::open(std::string_view native_path, u64 map_hash,
                                 const JsonValue& session) {
  // A journal taken by a session's end opens nothing again: an open truncates (T216's review, H1).
  if (taken_.load(std::memory_order_acquire)) return io::Status::IoError;
  close();
  events_written_ = 0;
  appends_ = 0;
  through_ = SimTick{0};
  failed_ = false;
  const std::filesystem::path path(
      std::u8string_view(reinterpret_cast<const char8_t*>(native_path.data()), native_path.size()));
  std::FILE* f = nullptr;
#if defined(_WIN32)
  // Shared for reading, so a tool that tails the journal while the session runs can.
  f = ::_wfsopen(path.c_str(), L"wb", _SH_DENYWR);
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  if (f == nullptr) return io::Status::IoError;
  file_ = f;
  std::string text = write_json(header_json(map_hash, session, k_log_version), {.pretty = false});
  text.push_back('\n');
  if (std::fwrite(text.data(), 1, text.size(), f) != text.size() || std::fflush(f) != 0) {
    close();
    return io::Status::IoError;
  }
  return io::Status::Ok;
}

io::Status InputLogJournal::append(std::span<const RawEvent> events, SimTick through) noexcept {
  std::FILE* f = static_cast<std::FILE*>(file_);
  if (f == nullptr || failed_) return io::Status::IoError;
  bool ok = true;
  char line[128];
  // A stack buffer as the canonical writer's sink (json_format.h).
  struct LineSink {
    char* at;
    usize left;
    bool fits = true;
    void append(const char* p, usize n) noexcept {
      if (n > left) {
        fits = false;
        return;
      }
      std::memcpy(at, p, n);
      at += n;
      left -= n;
    }
    void put(char c) noexcept { append(&c, 1); }
  };
  for (const RawEvent& e : events) {
    // The bytes `save` writes for the same event (T216's review, L6): integers by printf, which
    // no locale changes, and the value by the canonical writer's own spelling (shortest
    // round-trip, `std::to_chars`, never the C locale's decimal separator), through `line_value`.
    const int head =
        std::snprintf(line, sizeof(line), "[%llu,\"%s\",%u,",
                      static_cast<unsigned long long>(e.tick.value), source_name(e.source), e.code);
    ok = ok && head > 0 && static_cast<usize>(head) < sizeof(line);
    if (!ok) break;
    LineSink sink{line + head, sizeof(line) - static_cast<usize>(head)};
    (void)json_format::number(sink, line_value(e.value));
    char tail[16];
    const int tail_n = std::snprintf(tail, sizeof(tail), ",%u]\n", e.device);
    if (tail_n > 0) sink.append(tail, static_cast<usize>(tail_n));
    const usize n = sizeof(line) - sink.left;
    ok = sink.fits && tail_n > 0 && std::fwrite(line, 1, n, f) == n;
  }
  const int n = std::snprintf(line, sizeof(line), "{\"through\":%llu}\n",
                              static_cast<unsigned long long>(through.value));
  ok = ok && n > 0 && std::fwrite(line, 1, static_cast<usize>(n), f) == static_cast<usize>(n);
  ok = std::fflush(f) == 0 && ok;
  if (!ok) {
    failed_ = true;  // a short write leaves a line cut; nothing after it would load
    return io::Status::IoError;
  }
  events_written_ += events.size();
  ++appends_;
  through_ = through;
  return io::Status::Ok;
}

void InputLogJournal::close() noexcept {
  if (file_ != nullptr) (void)std::fclose(static_cast<std::FILE*>(file_));
  file_ = nullptr;
}

void InputLogJournal::take() noexcept {
  taken_.store(true, std::memory_order_release);
  close();
}

}  // namespace engine::input
