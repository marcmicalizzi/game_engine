#include <core/base/assert.h>
#include <core/json/json.h>
#include <core/platform/spin_lock.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace engine::tunables {

namespace {

struct RegistryState {
  platform::SpinLock lock;
  Tunable* head = nullptr;
  usize count = 0;
};

RegistryState g_registry;
std::atomic<u64> g_generation{0};

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (usize i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

void append_i64(std::string& out, i64 v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

void append_f64(std::string& out, f64 v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

void set_error(std::string* error, const Tunable& t, std::string_view what) {
  if (error == nullptr) return;
  error->assign(t.name());
  error->append(": ");
  error->append(what);
}

// Every tunable, sorted by name. Cold path; the registry chain is in reverse registration
// order, which is not useful to a reader.
Vector<Tunable*> sorted_tunables() {
  Vector<Tunable*> all;
  {
    std::lock_guard lock(g_registry.lock);
    all.reserve(static_cast<u32>(g_registry.count));
    for (Tunable* t = g_registry.head; t != nullptr; t = t->next())
      all.push_back(t);
  }
  std::sort(all.begin(), all.end(), [](const Tunable* a, const Tunable* b) {
    return std::strcmp(a->name(), b->name()) < 0;
  });
  return all;
}

}  // namespace

const char* kind_name(Kind kind) noexcept {
  switch (kind) {
    case Kind::Bool: return "bool";
    case Kind::Int: return "int";
    case Kind::Float: return "float";
    case Kind::Enum: return "enum";
  }
  return "?";
}

// ---- Tunable ---------------------------------------------------------------------------------

Tunable::Tunable(const char* name, const char* doc, Kind kind) noexcept
    : name_(name), doc_(doc != nullptr ? doc : ""), kind_(kind) {
  ENGINE_VERIFY(name != nullptr && name[0] != '\0', "tunable: name must be non-empty");
  std::lock_guard lock(g_registry.lock);
  for (Tunable* t = g_registry.head; t != nullptr; t = t->next_) {
    ENGINE_VERIFY(std::strcmp(t->name_, name) != 0, "tunable: duplicate name");
  }
  next_ = g_registry.head;
  g_registry.head = this;
  ++g_registry.count;
}

Tunable::~Tunable() {
  std::lock_guard lock(g_registry.lock);
  Tunable** link = &g_registry.head;
  while (*link != nullptr && *link != this)
    link = &(*link)->next_;
  if (*link == this) {
    *link = next_;
    --g_registry.count;
  }
}

void Tunable::changed() noexcept {
  version_.fetch_add(1, std::memory_order_relaxed);
  g_generation.fetch_add(1, std::memory_order_relaxed);
}

JsonValue Tunable::describe() const {
  JsonValue o = JsonValue::object();
  o.set("name", name_);
  o.set("kind", kind_name(kind_));
  o.set("value", value_json());
  o.set("default", default_json());
  o.set("doc", doc_);
  o.set("modified", !is_default());
  o.set("version", version());
  describe_range(o);
  return o;
}

// ---- Int -------------------------------------------------------------------------------------

Int::Int(const char* name, i64 default_value, i64 min, i64 max, const char* doc) noexcept
    : Tunable(name, doc, Kind::Int),
      value_(default_value),
      default_(default_value),
      min_(min),
      max_(max) {
  ENGINE_VERIFY(min <= default_value && default_value <= max,
                "tunables::Int: default outside [min, max]");
}

bool Int::set(i64 value) noexcept {
  if (value < min_ || value > max_) return false;
  if (value_.exchange(value, std::memory_order_acq_rel) != value) changed();
  return true;
}

bool Int::set_from_text(std::string_view text, std::string* error) {
  text = trim(text);
  i64 v = 0;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
  if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
    set_error(error, *this, "expected an integer");
    return false;
  }
  return set_from_json(JsonValue(v), error);
}

void Int::append_value(std::string& out) const {
  append_i64(out, get());
}
void Int::append_default(std::string& out) const {
  append_i64(out, default_);
}

bool Int::set_from_json(const JsonValue& value, std::string* error) {
  i64 v = 0;
  if (!value.get_i64(v)) {
    set_error(error, *this, "expected an integer");
    return false;
  }
  if (!set(v)) {
    std::string what = "value ";
    append_i64(what, v);
    what.append(" outside range [");
    append_i64(what, min_);
    what.append(", ");
    append_i64(what, max_);
    what.push_back(']');
    set_error(error, *this, what);
    return false;
  }
  return true;
}

void Int::describe_range(JsonValue& object) const {
  object.set("min", JsonValue(min_));
  object.set("max", JsonValue(max_));
}

// ---- Float -----------------------------------------------------------------------------------

Float::Float(const char* name, f64 default_value, f64 min, f64 max, const char* doc) noexcept
    : Tunable(name, doc, Kind::Float),
      value_(default_value),
      default_(default_value),
      min_(min),
      max_(max) {
  ENGINE_VERIFY(min <= default_value && default_value <= max,
                "tunables::Float: default outside [min, max]");
}

bool Float::set(f64 value) noexcept {
  if (std::isnan(value) || value < min_ || value > max_) return false;
  if (value_.exchange(value, std::memory_order_acq_rel) != value) changed();
  return true;
}

bool Float::set_from_text(std::string_view text, std::string* error) {
  text = trim(text);
  f64 v = 0;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
  if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
    set_error(error, *this, "expected a number");
    return false;
  }
  return set_from_json(JsonValue(v), error);
}

void Float::append_value(std::string& out) const {
  append_f64(out, get());
}
void Float::append_default(std::string& out) const {
  append_f64(out, default_);
}

bool Float::set_from_json(const JsonValue& value, std::string* error) {
  f64 v = 0;
  if (!value.get_f64(v)) {
    set_error(error, *this, "expected a number");
    return false;
  }
  if (!set(v)) {
    std::string what = "value ";
    append_f64(what, v);
    what.append(" outside range [");
    append_f64(what, min_);
    what.append(", ");
    append_f64(what, max_);
    what.push_back(']');
    set_error(error, *this, what);
    return false;
  }
  return true;
}

void Float::describe_range(JsonValue& object) const {
  object.set("min", JsonValue(min_));
  object.set("max", JsonValue(max_));
}

// ---- Bool ------------------------------------------------------------------------------------

Bool::Bool(const char* name, bool default_value, const char* doc) noexcept
    : Tunable(name, doc, Kind::Bool), value_(default_value), default_(default_value) {}

void Bool::set(bool value) noexcept {
  if (value_.exchange(value, std::memory_order_acq_rel) != value) changed();
}

bool Bool::set_from_text(std::string_view text, std::string* error) {
  text = trim(text);
  static constexpr std::string_view k_true[] = {"true", "on", "yes", "1"};
  static constexpr std::string_view k_false[] = {"false", "off", "no", "0"};
  for (const auto t : k_true) {
    if (equals_ignore_case(text, t)) {
      set(true);
      return true;
    }
  }
  for (const auto f : k_false) {
    if (equals_ignore_case(text, f)) {
      set(false);
      return true;
    }
  }
  set_error(error, *this, "expected true/false, on/off, yes/no, or 1/0");
  return false;
}

void Bool::append_value(std::string& out) const {
  out.append(get() ? "true" : "false");
}
void Bool::append_default(std::string& out) const {
  out.append(default_ ? "true" : "false");
}

bool Bool::set_from_json(const JsonValue& value, std::string* error) {
  bool v = false;
  if (!value.get_bool(v)) {
    set_error(error, *this, "expected a boolean");
    return false;
  }
  set(v);
  return true;
}

// ---- Enum ------------------------------------------------------------------------------------

EnumBase::EnumBase(const char* name, u32 default_index, std::span<const char* const> choices,
                   const char* doc) noexcept
    : Tunable(name, doc, Kind::Enum),
      index_(default_index),
      default_(default_index),
      choices_(choices) {
  ENGINE_VERIFY(!choices.empty() && default_index < choices.size(),
                "tunables::Enum: default is not one of the choices");
}

bool EnumBase::set_index(u32 index) noexcept {
  if (index >= choices_.size()) return false;
  if (index_.exchange(index, std::memory_order_acq_rel) != index) changed();
  return true;
}

bool EnumBase::find_choice(std::string_view text, u32& out) const noexcept {
  for (usize i = 0; i < choices_.size(); ++i) {
    if (equals_ignore_case(text, choices_[i])) {
      out = static_cast<u32>(i);
      return true;
    }
  }
  return false;
}

bool EnumBase::set_from_text(std::string_view text, std::string* error) {
  u32 index = 0;
  if (!find_choice(trim(text), index)) {
    std::string what = "expected one of";
    for (const char* c : choices_) {
      what.push_back(' ');
      what.append(c);
    }
    set_error(error, *this, what);
    return false;
  }
  (void)set_index(index);
  return true;
}

void EnumBase::append_value(std::string& out) const {
  out.append(choice_name(index()));
}
void EnumBase::append_default(std::string& out) const {
  out.append(choice_name(default_));
}

bool EnumBase::set_from_json(const JsonValue& value, std::string* error) {
  std::string_view text;
  if (value.get_string(text)) return set_from_text(text, error);
  u64 index = 0;
  if (value.get_u64(index) && index < choices_.size()) {
    (void)set_index(static_cast<u32>(index));
    return true;
  }
  set_error(error, *this, "expected a choice name or index");
  return false;
}

void EnumBase::describe_range(JsonValue& object) const {
  JsonValue choices = JsonValue::array();
  for (const char* c : choices_)
    choices.push_back(JsonValue(c));
  object.set("choices", std::move(choices));
}

// ---- registry --------------------------------------------------------------------------------

Tunable* first() noexcept {
  std::lock_guard lock(g_registry.lock);
  return g_registry.head;
}

Tunable* find(std::string_view name) noexcept {
  std::lock_guard lock(g_registry.lock);
  for (Tunable* t = g_registry.head; t != nullptr; t = t->next()) {
    if (name == t->name()) return t;
  }
  return nullptr;
}

usize count() noexcept {
  std::lock_guard lock(g_registry.lock);
  return g_registry.count;
}

u64 generation() noexcept {
  return g_generation.load(std::memory_order_relaxed);
}

bool apply_overrides(std::string_view spec, std::string* error) {
  bool ok = true;
  bool have_error = false;
  auto fail = [&](std::string_view message, std::string_view entry) {
    ok = false;
    if (error != nullptr && !have_error) {
      error->assign(message);
      error->append(" '");
      error->append(entry);
      error->push_back('\'');
      have_error = true;
    }
  };
  std::string_view rest = spec;
  while (!rest.empty()) {
    const usize comma = rest.find(',');
    const std::string_view entry = trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    if (entry.empty()) continue;
    const usize eq = entry.find('=');
    if (eq == std::string_view::npos) {
      fail("malformed tunable override", entry);
      continue;
    }
    const std::string_view name = trim(entry.substr(0, eq));
    Tunable* t = find(name);
    if (t == nullptr) {
      fail("unknown tunable", name);
      continue;
    }
    std::string local;
    if (!t->set_from_text(entry.substr(eq + 1), &local)) {
      ok = false;
      if (error != nullptr && !have_error) {
        *error = local;
        have_error = true;
      }
    }
  }
  return ok;
}

void reset_all() noexcept {
  for (Tunable* t : sorted_tunables())
    t->reset();
}

bool load_json(const JsonValue& object, Vector<std::string>* problems) {
  if (!object.is_object()) {
    if (problems != nullptr)
      problems->push_back("tunables: expected a JSON object of name -> value");
    return false;
  }
  bool ok = true;
  for (auto [name, value] : object.as_object()) {
    Tunable* t = find(name);
    if (t == nullptr) {
      ok = false;
      if (problems != nullptr) {
        std::string p = "unknown tunable '";
        p.append(name);
        p.push_back('\'');
        problems->push_back(std::move(p));
      }
      continue;
    }
    std::string error;
    if (!t->set_from_json(value, &error)) {
      ok = false;
      if (problems != nullptr) problems->push_back(std::move(error));
    }
  }
  return ok;
}

JsonValue save_json(bool only_modified) {
  JsonValue o = JsonValue::object();
  for (const Tunable* t : sorted_tunables()) {
    if (only_modified && t->is_default()) continue;
    o.set(t->name(), t->value_json());
  }
  return o;
}

JsonValue describe_all() {
  JsonValue a = JsonValue::array();
  for (const Tunable* t : sorted_tunables())
    a.push_back(t->describe());
  return a;
}

bool load_file(const char* path, Vector<std::string>* problems) {
  std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
  (void)fopen_s(&f, path, "rb");
#else
  f = std::fopen(path, "rb");
#endif
  if (f == nullptr) {
    if (problems != nullptr) {
      std::string p = "cannot open '";
      p.append(path);
      p.push_back('\'');
      problems->push_back(std::move(p));
    }
    return false;
  }
  std::string text;
  char buf[4096];
  for (;;) {
    const usize n = std::fread(buf, 1, sizeof(buf), f);
    if (n == 0) break;
    text.append(buf, n);
  }
  std::fclose(f);
  JsonValue root;
  const JsonParseResult r = parse_json(text, root);
  if (!r.ok) {
    if (problems != nullptr) {
      std::string p = path;
      p.append(": ");
      p.append(r.message);
      problems->push_back(std::move(p));
    }
    return false;
  }
  return load_json(root, problems);
}

bool save_file(const char* path, bool only_modified) {
  std::string text = write_json(save_json(only_modified));
  text.push_back('\n');
  std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
  (void)fopen_s(&f, path, "wb");
#else
  f = std::fopen(path, "wb");
#endif
  if (f == nullptr) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace engine::tunables
