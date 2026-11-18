#include <core/base/assert.h>
#include <core/json/json.h>
#include <foundation/input/input_log.h>

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace engine::input {

namespace {

// One event per line as [tick, source, code, value, device]: the tick sorts first, and an
// array costs a fifth of what the same fields cost as an object in a log of a million events.
JsonValue event_to_json(const RawEvent& event) {
  JsonValue::Array row;
  row.push_back(JsonValue(event.tick.value));
  row.push_back(JsonValue(source_name(event.source)));
  row.push_back(JsonValue(static_cast<u64>(event.code)));
  row.push_back(JsonValue(static_cast<f64>(event.value)));
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
}

SimTick InputLog::first_tick() const noexcept {
  return events_.empty() ? SimTick{1} : events_.front().tick;
}

SimTick InputLog::last_tick() const noexcept {
  return events_.empty() ? SimTick{0} : events_.back().tick;
}

io::Status InputLog::save(std::string_view native_path) const {
  JsonValue header = JsonValue::object();
  header.set("type", k_log_type);
  header.set("version", static_cast<u64>(k_log_version));
  header.set("map", map_hash_);
  JsonValue::Array columns;
  columns.push_back(JsonValue("tick"));
  columns.push_back(JsonValue("source"));
  columns.push_back(JsonValue("code"));
  columns.push_back(JsonValue("value"));
  columns.push_back(JsonValue("device"));
  header.set("columns", JsonValue(std::move(columns)));
  // Only when there is one, so a log recorded without a session is the bytes it always was.
  if (!session_.is_null()) header.set("session", session_);

  const JsonWriteOptions compact{.pretty = false};
  std::string text = write_json(header, compact);
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
  while (begin <= text.size()) {
    usize end = text.find('\n', begin);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + begin, end - begin);
    begin = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.remove_suffix(1);
    if (line.empty()) continue;

    JsonValue value;
    if (const JsonParseResult parsed = parse_json(line, value); !parsed.ok) {
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
      u64 version = 0;
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

}  // namespace engine::input
