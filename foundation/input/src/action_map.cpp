#include <core/hash/hash.h>
#include <core/json/json.h>
#include <foundation/input/input.h>

#include <bit>
#include <utility>

namespace engine::input {

namespace {

constexpr const char* k_source_names[k_source_count] = {
    "key",          "mouse_button",  "mouse_axis",      "gamepad_button",
    "gamepad_axis", "joystick_axis", "joystick_button", "joystick_hat"};
constexpr const char* k_kind_names[3] = {"button", "axis", "axis2"};

// Floats go into the digest by their bits so that a scale of -1 and a scale of 1 cannot
// collide; -0 is folded onto 0 first so two maps that compare equal also hash equal.
u32 float_bits(f32 v) noexcept {
  if (v == 0.0f) v = 0.0f;
  return std::bit_cast<u32>(v);
}

bool read_f32(const JsonValue& object, const char* key, f32& out, std::string* error) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) return true;  // absent: keep the default
  f64 v = 0.0;
  if (!member->get_f64(v)) {
    if (error != nullptr) *error = std::string("input: '") + key + "' is not a number";
    return false;
  }
  out = static_cast<f32>(v);
  return true;
}

bool read_u32(const JsonValue& object, const char* key, u32& out, bool required,
              std::string* error) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    if (!required) return true;
    if (error != nullptr) *error = std::string("input: '") + key + "' is missing";
    return false;
  }
  u64 v = 0;
  if (!member->get_u64(v) || v > 0xFFFFFFFFull) {
    if (error != nullptr) *error = std::string("input: '") + key + "' is not a 32-bit number";
    return false;
  }
  out = static_cast<u32>(v);
  return true;
}

}  // namespace

const char* source_name(Source source) noexcept {
  const u32 index = static_cast<u32>(source);
  return index < k_source_count ? k_source_names[index] : "unknown";
}

bool source_from_name(std::string_view name, Source& out) noexcept {
  for (u32 i = 0; i < k_source_count; ++i) {
    if (name == k_source_names[i]) {
      out = static_cast<Source>(i);
      return true;
    }
  }
  return false;
}

const char* action_kind_name(ActionKind kind) noexcept {
  const u32 index = static_cast<u32>(kind);
  return index < 3 ? k_kind_names[index] : "unknown";
}

bool action_kind_from_name(std::string_view name, ActionKind& out) noexcept {
  for (u32 i = 0; i < 3; ++i) {
    if (name == k_kind_names[i]) {
      out = static_cast<ActionKind>(i);
      return true;
    }
  }
  return false;
}

ActionId ActionMap::add_action(std::string_view name, ActionKind kind) {
  if (name.empty()) return k_invalid_action;
  if (const u32* existing = by_name_.find_value(name); existing != nullptr) {
    return actions_[*existing].kind == kind ? *existing : k_invalid_action;
  }
  const ActionId id = actions_.size();
  Action action;
  action.name.assign(name);
  action.kind = kind;
  actions_.push_back(std::move(action));
  by_name_.insert_or_assign(std::string(name), id);
  return id;
}

ActionId ActionMap::find_action(std::string_view name) const noexcept {
  const u32* found = by_name_.find_value(name);
  return found != nullptr ? *found : k_invalid_action;
}

std::string_view ActionMap::action_name(ActionId action) const noexcept {
  return valid(action) ? std::string_view(actions_[action].name) : std::string_view();
}

ActionKind ActionMap::action_kind(ActionId action) const noexcept {
  return valid(action) ? actions_[action].kind : ActionKind::Button;
}

void ActionMap::bind(ActionId action, const Binding& binding, u32 component) {
  ENGINE_ASSERT(valid(action), "input::ActionMap::bind: unknown action");
  if (!valid(action)) return;
  actions_[action].list.push_back(ActionBinding{binding, component});
  ++bound_[signal_key(binding.source, binding.code)];
}

std::span<const ActionBinding> ActionMap::bindings(ActionId action) const noexcept {
  if (!valid(action)) return {};
  const Vector<ActionBinding>& list = actions_[action].list;
  return {list.data(), list.size()};
}

void ActionMap::clear_bindings(ActionId action) {
  if (!valid(action)) return;
  for (const ActionBinding& ab : actions_[action].list) {
    const u64 key = signal_key(ab.binding.source, ab.binding.code);
    u32* count = bound_.find_value(key);
    if (count == nullptr) continue;
    if (--*count == 0) bound_.erase(key);
  }
  actions_[action].list.clear();
}

void ActionMap::clear() noexcept {
  actions_.clear();
  by_name_.clear();
  bound_.clear();
}

bool ActionMap::is_bound(Source source, u32 code) const noexcept {
  return bound_.contains(signal_key(source, code));
}

u64 ActionMap::hash() const noexcept {
  u64 h = hash_combine(k_hash_seed, actions_.size());
  for (const Action& action : actions_) {
    h = hash_combine(h, hash_bytes(action.name.data(), action.name.size()));
    h = hash_combine(h, static_cast<u64>(action.kind));
    h = hash_combine(h, action.list.size());
    for (const ActionBinding& ab : action.list) {
      h = hash_combine(h, static_cast<u64>(ab.binding.source));
      h = hash_combine(h, ab.binding.code);
      h = hash_combine(h, float_bits(ab.binding.scale));
      h = hash_combine(h, float_bits(ab.binding.deadzone));
      h = hash_combine(h, ab.component);
    }
  }
  return h;
}

JsonValue ActionMap::to_json() const {
  JsonValue root = JsonValue::object();
  root.set("version", static_cast<u64>(1));
  JsonValue array = JsonValue::array();
  for (const Action& action : actions_) {
    JsonValue entry = JsonValue::object();
    entry.set("name", action.name);
    entry.set("kind", action_kind_name(action.kind));
    JsonValue list = JsonValue::array();
    for (const ActionBinding& ab : action.list) {
      JsonValue b = JsonValue::object();
      b.set("source", source_name(ab.binding.source));
      b.set("code", static_cast<u64>(ab.binding.code));
      b.set("scale", static_cast<f64>(ab.binding.scale));
      // Defaults stay out of the file so a rebind diff shows only what the player changed.
      if (ab.binding.deadzone != 0.0f) b.set("deadzone", static_cast<f64>(ab.binding.deadzone));
      if (ab.component != 0) b.set("component", static_cast<u64>(ab.component));
      list.push_back(std::move(b));
    }
    entry.set("bindings", std::move(list));
    array.push_back(std::move(entry));
  }
  root.set("actions", std::move(array));
  return root;
}

bool ActionMap::from_json(const JsonValue& value, std::string* error) {
  clear();
  if (!value.is_object()) {
    if (error != nullptr) *error = "input: an action map is a JSON object";
    return false;
  }
  const JsonValue* actions = value.find("actions");
  if (actions == nullptr || !actions->is_array()) {
    if (error != nullptr) *error = "input: 'actions' is missing or not an array";
    return false;
  }
  for (const JsonValue& entry : actions->as_array()) {
    if (!entry.is_object()) {
      if (error != nullptr) *error = "input: an action is a JSON object";
      clear();
      return false;
    }
    std::string_view name;
    const JsonValue* name_value = entry.find("name");
    if (name_value == nullptr || !name_value->get_string(name) || name.empty()) {
      if (error != nullptr) *error = "input: an action needs a non-empty 'name'";
      clear();
      return false;
    }
    std::string_view kind_name;
    ActionKind kind = ActionKind::Button;
    const JsonValue* kind_value = entry.find("kind");
    if (kind_value == nullptr || !kind_value->get_string(kind_name) ||
        !action_kind_from_name(kind_name, kind)) {
      if (error != nullptr) {
        *error = "input: action '" + std::string(name) + "' has no known 'kind'";
      }
      clear();
      return false;
    }
    const ActionId id = add_action(name, kind);
    if (id == k_invalid_action) {
      if (error != nullptr) *error = "input: duplicate action '" + std::string(name) + "'";
      clear();
      return false;
    }
    const JsonValue* list = entry.find("bindings");
    if (list == nullptr) continue;
    if (!list->is_array()) {
      if (error != nullptr) {
        *error = "input: 'bindings' of '" + std::string(name) + "' is not an array";
      }
      clear();
      return false;
    }
    for (const JsonValue& b : list->as_array()) {
      if (!b.is_object()) {
        if (error != nullptr) *error = "input: a binding is a JSON object";
        clear();
        return false;
      }
      std::string_view source_text;
      Binding binding;
      const JsonValue* source_value = b.find("source");
      if (source_value == nullptr || !source_value->get_string(source_text) ||
          !source_from_name(source_text, binding.source)) {
        if (error != nullptr) {
          *error = "input: binding of '" + std::string(name) + "' has no known 'source'";
        }
        clear();
        return false;
      }
      u32 component = 0;
      if (!read_u32(b, "code", binding.code, true, error) ||
          !read_u32(b, "component", component, false, error) ||
          !read_f32(b, "scale", binding.scale, error) ||
          !read_f32(b, "deadzone", binding.deadzone, error)) {
        clear();
        return false;
      }
      bind(id, binding, component);
    }
  }
  return true;
}

std::string ActionMap::describe() const {
  std::string text = "input::ActionMap: ";
  text += std::to_string(actions_.size());
  text += " action(s), map ";
  text += std::to_string(hash());
  text += '\n';
  text += write_json(to_json(), JsonWriteOptions{.pretty = true});
  text += '\n';
  return text;
}

}  // namespace engine::input
