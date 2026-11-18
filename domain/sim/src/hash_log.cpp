#include <domain/sim/hash_log.h>

#include <core/json/json.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace engine::sim {

namespace {

// The file: "ENGHLOG1", then u32 version, u32 table count, u32 phase mask, u64 row count, each
// table as u32 length + bytes, then each row as u64 tick, u32 phase, u32 0, and its table hashes as
// u64. All little-endian (every supported platform is; the writer is the memcpy below).
constexpr char k_magic[8] = {'E', 'N', 'G', 'H', 'L', 'O', 'G', '1'};
constexpr u32 k_version = 1;
// A name past this is not a table name: a bound against a hostile header, not a format limit.
constexpr u32 k_max_name = 4096;

template <class T>
void put(std::string& out, T value) {
  char bytes[sizeof(T)];
  std::memcpy(bytes, &value, sizeof(T));
  out.append(bytes, sizeof(T));
}

struct Reader {
  std::string_view bytes;
  usize at = 0;
  template <class T>
  bool get(T& value) {
    if (bytes.size() - at < sizeof(T)) return false;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
  }
  usize left() const noexcept { return bytes.size() - at; }
};

void fail(std::string* error, std::string_view why) {
  if (error != nullptr) *error = std::string(why);
}

u64 row_key(const HashLogRow& row) noexcept { return row.tick * 16u + row.phase; }

std::string compact(const JsonValue& value) {
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(value, options);
}

// Every leaf where `a` and `b` differ, by path. An object's keys are walked as a merge of the two
// sorted key lists (a JsonValue object keeps its keys sorted), an array index by index.
void diff_values(const JsonValue& a, const JsonValue& b, std::string& path,
                 Vector<FieldDifference>& out) {
  if (a.is_object() && b.is_object()) {
    const JsonValue::Object& oa = a.as_object();
    const JsonValue::Object& ob = b.as_object();
    JsonValue::Object::size_type i = 0;
    JsonValue::Object::size_type j = 0;
    while (i < oa.size() || j < ob.size()) {
      const bool take_a = j >= ob.size() || (i < oa.size() && oa.key_at(i) < ob.key_at(j));
      const bool take_b = i >= oa.size() || (j < ob.size() && ob.key_at(j) < oa.key_at(i));
      const usize mark = path.size();
      if (!path.empty()) path.push_back('.');
      if (take_a) {
        path += oa.key_at(i);
        out.push_back(FieldDifference{path, compact(oa.value_at(i)), std::string()});
        ++i;
      } else if (take_b) {
        path += ob.key_at(j);
        out.push_back(FieldDifference{path, std::string(), compact(ob.value_at(j))});
        ++j;
      } else {
        path += oa.key_at(i);
        diff_values(oa.value_at(i), ob.value_at(j), path, out);
        ++i;
        ++j;
      }
      path.resize(mark);
    }
    return;
  }
  if (a.is_array() && b.is_array() && a.size() == b.size()) {
    for (usize i = 0; i < a.size(); ++i) {
      const usize mark = path.size();
      path += "[" + std::to_string(i) + "]";
      diff_values(a[i], b[i], path, out);
      path.resize(mark);
    }
    return;
  }
  if (!(a == b)) out.push_back(FieldDifference{path, compact(a), compact(b)});
}

std::string_view entity_id(const JsonValue& entity) {
  std::string_view id;
  const JsonValue* v = entity.find("id");
  if (v != nullptr) (void)v->get_string(id);
  return id;
}

}  // namespace

// ---- HashLog -----------------------------------------------------------------------------------

void HashLog::set_tables(std::span<const std::string> names, u8 phases) {
  tables_.clear();
  for (const std::string& name : names)
    tables_.push_back(name);
  phases_ = phases;
  clear();
}

u32 HashLog::table_index(std::string_view name) const noexcept {
  for (u32 i = 0; i < tables_.size(); ++i) {
    if (tables_[i] == name) return i;
  }
  return ~0u;
}

void HashLog::reserve(u64 ticks) {
  u32 per_tick = 0;
  for (u32 p = 0; p < k_phase_count; ++p)
    per_tick += (phases_ >> p) & 1u;
  rows_.reserve(static_cast<u32>(ticks * per_tick));
  hashes_.reserve(static_cast<u32>(ticks * per_tick * tables_.size()));
}

bool HashLog::append(u64 tick, TickPhase phase, std::span<const u64> hashes) {
  if (hashes.size() != tables_.size()) return false;
  HashLogRow row;
  row.tick = tick;
  row.phase = static_cast<u32>(phase);
  row.first = static_cast<u32>(hashes_.size());
  if (!rows_.empty() && row_key(row) <= row_key(rows_.back())) return false;
  rows_.push_back(row);
  for (const u64 h : hashes)
    hashes_.push_back(h);
  return true;
}

void HashLog::clear() noexcept {
  rows_.clear();
  hashes_.clear();
}

std::span<const u64> HashLog::row_hashes(u32 row) const noexcept {
  if (row >= rows_.size()) return {};
  return {hashes_.data() + rows_[row].first, tables_.size()};
}

usize HashLog::memory_bytes() const noexcept {
  usize names = 0;
  for (const std::string& name : tables_)
    names += sizeof(std::string) + name.capacity();
  return rows_.capacity() * sizeof(HashLogRow) + hashes_.capacity() * sizeof(u64) + names;
}

void HashLog::serialize(std::string& out) const {
  out.clear();
  out.reserve(32 + rows_.size() * (16 + 8 * tables_.size()));
  out.append(k_magic, sizeof(k_magic));
  put<u32>(out, k_version);
  put<u32>(out, static_cast<u32>(tables_.size()));
  put<u32>(out, phases_);
  put<u64>(out, rows_.size());
  for (const std::string& name : tables_) {
    put<u32>(out, static_cast<u32>(name.size()));
    out.append(name);
  }
  for (u32 r = 0; r < rows_.size(); ++r) {
    put<u64>(out, rows_[r].tick);
    put<u32>(out, rows_[r].phase);
    put<u32>(out, 0u);
    for (const u64 h : row_hashes(r))
      put<u64>(out, h);
  }
}

bool HashLog::deserialize(std::string_view bytes, std::string* error) {
  tables_.clear();
  phases_ = 0;
  clear();
  Reader in{bytes};
  if (bytes.size() < sizeof(k_magic) || std::memcmp(bytes.data(), k_magic, sizeof(k_magic)) != 0) {
    fail(error, "not a hash log (no ENGHLOG1 header)");
    return false;
  }
  in.at = sizeof(k_magic);
  u32 version = 0;
  u32 tables = 0;
  u32 phases = 0;
  u64 rows = 0;
  if (!in.get(version) || !in.get(tables) || !in.get(phases) || !in.get(rows)) {
    fail(error, "the hash log's header is cut short");
    return false;
  }
  if (version != k_version) {
    fail(error, "the hash log's version " + std::to_string(version) + " is not " +
                    std::to_string(k_version));
    return false;
  }
  if (phases > 0xFFu || tables > in.left() / 4) {
    fail(error, "the hash log's header is inconsistent with its size");
    return false;
  }
  Vector<std::string> names;
  for (u32 t = 0; t < tables; ++t) {
    u32 length = 0;
    if (!in.get(length) || length > k_max_name || length > in.left()) {
      fail(error, "a table name in the hash log is cut short or too long");
      return false;
    }
    names.push_back(std::string(bytes.substr(in.at, length)));
    in.at += length;
  }
  const u64 row_bytes = 16u + 8u * static_cast<u64>(tables);
  if (rows != in.left() / row_bytes || in.left() % row_bytes != 0) {
    fail(error, "the hash log's row count does not match its size");
    return false;
  }
  if (rows * (u64{tables} + 1u) > 0xFFFF'FFFFull) {
    fail(error, "the hash log holds more rows than a log can");
    return false;
  }
  set_tables(std::span<const std::string>(names.data(), names.size()), static_cast<u8>(phases));
  rows_.reserve(static_cast<u32>(rows));
  hashes_.reserve(static_cast<u32>(rows * tables));
  Vector<u64> row(tables);
  for (u64 r = 0; r < rows; ++r) {
    u64 tick = 0;
    u32 phase = 0;
    u32 zero = 0;
    in.get(tick);
    in.get(phase);
    in.get(zero);
    for (u32 t = 0; t < tables; ++t)
      in.get(row[t]);
    if (phase >= k_phase_count ||
        !append(tick, static_cast<TickPhase>(phase), std::span<const u64>(row.data(), tables))) {
      fail(error, "row " + std::to_string(r) + " of the hash log is out of order");
      clear();
      tables_.clear();
      return false;
    }
  }
  return true;
}

bool HashLog::save(std::string_view native_path, std::string* error) const {
  std::string bytes;
  serialize(bytes);
  const io::Status status = io::write_file_atomic(native_path, bytes);
  if (status != io::Status::Ok) {
    fail(error, std::string("cannot write the hash log: ") + io::status_name(status));
    return false;
  }
  return true;
}

bool HashLog::load(std::string_view native_path, std::string* error) {
  std::string bytes;
  const io::Status status = io::read_file(native_path, bytes);
  if (status != io::Status::Ok) {
    tables_.clear();
    clear();
    fail(error, std::string("cannot read the hash log: ") + io::status_name(status));
    return false;
  }
  return deserialize(bytes, error);
}

// ---- the first divergence ----------------------------------------------------------------------

Divergence first_divergence(const HashLog& a, const HashLog& b) {
  Divergence out;
  out.rows_a = a.rows().size();
  out.rows_b = b.rows().size();
  // Table i of `a` is table map[i] of `b`, or ~0u.
  Vector<u32> map;
  for (const std::string& name : a.tables())
    map.push_back(b.table_index(name));

  const auto differs_at = [&](u32 ra, u32 rb) {
    const std::span<const u64> ha = a.row_hashes(ra);
    const std::span<const u64> hb = b.row_hashes(rb);
    for (u32 t = 0; t < ha.size(); ++t) {
      TableDivergence d;
      d.table = a.tables()[t];
      d.a = ha[t];
      if (map[t] == ~0u) {
        d.only_in_a = true;
      } else {
        d.b = hb[map[t]];
        if (d.a == d.b) continue;
      }
      out.tables.push_back(std::move(d));
    }
    for (u32 t = 0; t < b.tables().size(); ++t) {
      if (a.table_index(b.tables()[t]) != ~0u) continue;
      TableDivergence d;
      d.table = b.tables()[t];
      d.b = hb[t];
      d.only_in_b = true;
      out.tables.push_back(std::move(d));
    }
    return !out.tables.empty();
  };

  u32 ra = 0;
  u32 rb = 0;
  while (ra < a.rows().size() && rb < b.rows().size()) {
    const HashLogRow& x = a.rows()[ra];
    const HashLogRow& y = b.rows()[rb];
    if (row_key(x) != row_key(y)) {
      // A (tick, phase) on one side only: the two hashed different phases or one skipped a tick.
      // Said once; the walk goes on to the next row both have.
      out.rows_differ = true;
      if (row_key(x) < row_key(y)) {
        ++ra;
      } else {
        ++rb;
      }
      continue;
    }
    ++out.rows_compared;
    if (differs_at(ra, rb)) {
      out.found = true;
      out.tick = x.tick;
      out.phase = static_cast<TickPhase>(x.phase);
      return out;
    }
    ++ra;
    ++rb;
  }
  return out;
}

// ---- captures ----------------------------------------------------------------------------------

JsonValue capture_to_json(const Capture& capture) {
  JsonValue out = JsonValue::object();
  out.set("format", "engine.sim.capture.v1");
  out.set("tick", capture.tick);
  out.set("phase", phase_name(capture.phase));
  out.set("table", capture.table);
  out.set("entities", capture.entities);
  return out;
}

bool capture_from_json(const JsonValue& value, Capture& out, std::string* error) {
  out = Capture{};
  std::string_view format;
  std::string_view phase;
  std::string_view table;
  const JsonValue* f = value.find("format");
  const JsonValue* t = value.find("tick");
  const JsonValue* p = value.find("phase");
  const JsonValue* n = value.find("table");
  const JsonValue* e = value.find("entities");
  if (f == nullptr || !f->get_string(format) || format != "engine.sim.capture.v1" ||
      t == nullptr || !t->get_u64(out.tick) || p == nullptr || !p->get_string(phase) ||
      n == nullptr || !n->get_string(table) || e == nullptr || !e->is_array()) {
    fail(error, "not an engine.sim.capture.v1 object");
    return false;
  }
  bool known = false;
  for (u32 i = 0; i < k_phase_count; ++i) {
    if (phase == phase_name(static_cast<TickPhase>(i))) {
      out.phase = static_cast<TickPhase>(i);
      known = true;
    }
  }
  if (!known) {
    fail(error, "the capture names no phase");
    return false;
  }
  out.table = std::string(table);
  out.entities = *e;
  for (usize i = 0; i < out.entities.size(); ++i) {
    if (entity_id(out.entities[i]).empty() || out.entities[i].find("value") == nullptr ||
        (i > 0 && !(entity_id(out.entities[i - 1]) < entity_id(out.entities[i])))) {
      fail(error, "the capture's entities are not {id, value} objects in id order");
      return false;
    }
  }
  return true;
}

CaptureDiff diff_captures(const Capture& a, const Capture& b, u32 max_entities, u64* differing) {
  CaptureDiff out;
  u64 count = 0;
  const auto listed = [&](EntityDifference&& d) {
    ++count;
    if (out.entities.size() < max_entities) out.entities.push_back(std::move(d));
  };
  usize i = 0;
  usize j = 0;
  const usize na = a.entities.size();
  const usize nb = b.entities.size();
  std::string path;
  while (i < na || j < nb) {
    const std::string_view ia = i < na ? entity_id(a.entities[i]) : std::string_view();
    const std::string_view ib = j < nb ? entity_id(b.entities[j]) : std::string_view();
    EntityDifference d;
    if (j >= nb || (i < na && ia < ib)) {
      d.id = std::string(ia);
      d.only_in_a = true;
      ++i;
      listed(std::move(d));
    } else if (i >= na || ib < ia) {
      d.id = std::string(ib);
      d.only_in_b = true;
      ++j;
      listed(std::move(d));
    } else {
      ++out.compared;
      path.clear();
      diff_values(*a.entities[i].find("value"), *b.entities[j].find("value"), path, d.fields);
      if (!d.fields.empty()) {
        d.id = std::string(ia);
        listed(std::move(d));
      }
      ++i;
      ++j;
    }
  }
  if (differing != nullptr) *differing = count;
  return out;
}

// ---- the recorded run --------------------------------------------------------------------------

bool plain_file_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
  }
  return true;
}

std::string run_file(std::string_view directory, std::string_view name) {
  if (!plain_file_name(name)) return {};
  return io::join_path(directory, name);
}

bool write_run_record(std::string_view directory, const RunRecord& record, std::string* error) {
  for (const std::string* name : {&record.input_log, &record.external_log, &record.hash_log}) {
    if (!name->empty() && !plain_file_name(*name)) {
      fail(error, "a run record names its files by plain name beside it, not '" + *name + "'");
      return false;
    }
  }
  JsonValue out = JsonValue::object();
  out.set("format", "engine.sim.run.v1");
  out.set("seed", record.seed);
  out.set("hz", record.hz);
  out.set("start_tick", record.start_tick);
  out.set("start_time_us", record.start_time_us);
  out.set("ticks", record.ticks);
  out.set("input_log", record.input_log);
  out.set("external_log", record.external_log);
  out.set("hash_log", record.hash_log);
  const io::Status status =
      io::write_file_atomic(io::join_path(directory, "run.json"), write_json(out));
  if (status != io::Status::Ok) {
    fail(error, std::string("cannot write run.json: ") + io::status_name(status));
    return false;
  }
  return true;
}

bool read_run_record(std::string_view directory, RunRecord& out, std::string* error) {
  out = RunRecord{};
  std::string text;
  const io::Status status = io::read_file(io::join_path(directory, "run.json"), text);
  if (status != io::Status::Ok) {
    fail(error, std::string("cannot read run.json: ") + io::status_name(status));
    return false;
  }
  JsonValue value;
  if (!parse_json(text, value).ok || !value.is_object()) {
    fail(error, "run.json is not a JSON object");
    return false;
  }
  std::string_view format;
  const JsonValue* f = value.find("format");
  if (f == nullptr || !f->get_string(format) || format != "engine.sim.run.v1") {
    fail(error, "run.json is not an engine.sim.run.v1 record");
    return false;
  }
  const auto u = [&](std::string_view key, u64& to) {
    const JsonValue* v = value.find(key);
    return v != nullptr && v->get_u64(to);
  };
  u64 hz = 0;
  const JsonValue* time = value.find("start_time_us");
  if (!u("seed", out.seed) || !u("hz", hz) || hz == 0 || hz > 100'000 ||
      !u("start_tick", out.start_tick) || !u("ticks", out.ticks) || time == nullptr ||
      !time->get_i64(out.start_time_us)) {
    fail(error, "run.json lacks a seed, a rate, a start or a tick count");
    return false;
  }
  out.hz = static_cast<u32>(hz);
  const std::pair<const char*, std::string*> files[] = {
      {"input_log", &out.input_log}, {"external_log", &out.external_log}, {"hash_log", &out.hash_log}};
  for (const auto& [key, to] : files) {
    std::string_view name;
    const JsonValue* v = value.find(key);
    if (v != nullptr && v->get_string(name) && !name.empty()) {
      if (!plain_file_name(name)) {
        fail(error, std::string("run.json's ") + key + " is not a plain file name beside it");
        return false;
      }
      *to = std::string(name);
    }
  }
  return true;
}

}  // namespace engine::sim
