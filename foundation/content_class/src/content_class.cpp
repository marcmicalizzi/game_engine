#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <foundation/content_class/content_class.h>

#include <algorithm>
#include <schemas/provenance.h>

namespace engine::content_class {

namespace {

// The ADR-0033 decision 1 ladders, lowest first.
const char* const k_engine_names[k_engine_categories] = {"nudity", "sexual", "violence"};
const char* const k_engine_ladders[k_engine_categories][3] = {
    {"none", "partial", "full"},
    {"none", "suggestive", "explicit"},
    {"none", "mild", "graphic"},
};

std::string schema_error(const schema::ReadContext& ctx) {
  if (ctx.diagnostics.empty()) return "unreadable";
  const schema::Diagnostic& d = ctx.diagnostics[0];
  return d.path.empty() ? d.message : d.path + ": " + d.message;
}

// Every file this module reads is parsed strictly: a key given twice is refused, because the
// parser's ordinary rule (the last one wins) would let a text merge or a tool's append declare a
// class, or a cap, and silently lose it.
bool parse_strict(std::string_view json, JsonValue& value, const char* what, std::string& error) {
  JsonParseOptions options;
  options.refuse_duplicate_keys = true;
  const JsonParseResult r = parse_json(json, value, options);
  if (r.ok) return true;
  if (!r.duplicate_key.empty()) {
    error = std::string(what) + ": the key '" + r.duplicate_key +
            "' is given twice in one object (line " + std::to_string(r.line) + ")";
  } else {
    error =
        std::string(what) + " is not JSON (line " + std::to_string(r.line) + ": " + r.message + ")";
  }
  return false;
}

template <class T>
bool read_file_json(std::string_view json, T& out, const char* what, std::string& error) {
  JsonValue value;
  if (!parse_strict(json, value, what, error)) return false;
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx) || !ctx.ok()) {
    error = std::string(what) + ": " + schema_error(ctx);
    return false;
  }
  return true;
}

}  // namespace

ContentClasses join(const ContentClasses& a, const ContentClasses& b) {
  ContentClasses out;
  for (u32 i = 0; i < k_max_categories; ++i)
    out.levels[i] = std::max(a.levels[i], b.levels[i]);
  return out;
}

bool within(const ContentClasses& classes, const ViewPolicy& policy) {
  bool ok = true;
  for (u32 i = 0; i < k_max_categories; ++i)
    ok &= classes.levels[i] <= policy.caps[i];
  return ok;
}

ViewPolicy narrow(const ViewPolicy& a, const ViewPolicy& b) {
  ViewPolicy out;
  for (u32 i = 0; i < k_max_categories; ++i) {
    out.caps[i] = std::min(a.caps[i], b.caps[i]);
    // `omit` is zero and the stricter: the lower of the two.
    out.substitutions[i] = static_cast<Substitution>(
        std::min(static_cast<u8>(a.substitutions[i]), static_cast<u8>(b.substitutions[i])));
  }
  return out;
}

Decision decide(const ContentClasses& classes, const ViewPolicy& policy) {
  // An unclassified asset, which is nearly every asset: one test of sixteen bytes.
  if (is_none(classes)) return {};
  Decision proxy;
  for (u32 i = 0; i < k_max_categories; ++i) {
    const u8 level = classes.levels[i];
    const u8 cap = policy.caps[i];
    if (level <= cap) continue;
    const u8 category = static_cast<u8>(i);
    if (policy.substitutions[i] == Substitution::omit) return {Outcome::omit, category, level, cap};
    if (proxy.outcome == Outcome::admit) proxy = {Outcome::proxy, category, level, cap};
  }
  return proxy;
}

ViewPolicy agent_default() {
  ViewPolicy p;            // every category `none`, game categories included
  p.caps[k_violence] = 1;  // `mild`
  for (u32 i = 0; i < k_max_categories; ++i)
    p.substitutions[i] = i < k_engine_categories ? k_agent_default_substitutions[i]
                                                 : k_agent_default_game_substitution;
  return p;
}

Categories::Categories() {
  for (u32 c = 0; c < k_engine_categories; ++c) {
    names_.push_back(k_engine_names[c]);
    Vector<std::string> ladder;
    for (const char* level : k_engine_ladders[c])
      ladder.push_back(level);
    ladders_.push_back(std::move(ladder));
  }
}

namespace {

const char* const k_name_rule =
    "lower-case letters, digits and '_', and not 'levels' or 'materials'";

// A ladder as a categories file or data carries it: starting at `none`, no level empty or named
// twice, at most `k_max_levels`.
template <class Levels>
bool check_ladder(const std::string& name, const Levels& levels, const std::string& where,
                  std::string& error) {
  if (levels.empty() || levels[0] != "none") {
    error = where + ": '" + name + "' does not start at 'none'";
    return false;
  }
  if (levels.size() > k_max_levels) {
    error = where + ": '" + name + "' has more than " + std::to_string(k_max_levels) + " levels";
    return false;
  }
  for (u32 a = 0; a < levels.size(); ++a) {
    if (levels[a].empty()) {
      error = where + ": '" + name + "' has an empty level";
      return false;
    }
    for (u32 b = 0; b < a; ++b) {
      if (levels[a] == levels[b]) {
        error = where + ": '" + name + "' names level '" + levels[a] + "' twice";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool Categories::valid_name(std::string_view name) {
  if (name.empty() || name == "levels" || name == "materials") return false;
  for (const char ch : name) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) return false;
  }
  return true;
}

bool Categories::learn(std::string_view name, const Vector<std::string>& ladder, u32& index,
                       std::string& error) {
  const std::string owned(name);
  const std::string where = "category '" + owned + "'";
  if (!valid_name(name)) {
    error = where + ": not a category name (" + k_name_rule + ")";
    return false;
  }
  if (!check_ladder(owned, ladder, where, error)) return false;
  const i32 known = find(name);
  if (known >= 0) {
    const Vector<std::string>& mine = ladders_[static_cast<u32>(known)];
    bool same = mine.size() == ladder.size();
    for (u32 l = 0; same && l < ladder.size(); ++l)
      same = mine[l] == ladder[l];
    if (!same) {
      // The first ladder met for a name is that name's; an engine category's is compiled in.
      error = where + ": its ladder differs from the one this table already holds for it";
      return false;
    }
    index = static_cast<u32>(known);
    return true;
  }
  if (size() >= k_max_categories) {
    error = where + ": a class holds at most " + std::to_string(k_max_categories) + " categories";
    return false;
  }
  index = size();
  names_.push_back(owned);
  ladders_.push_back(ladder);
  return true;
}

bool Categories::add(const content::ContentCategoriesFile& file, std::string& error) {
  const u32 total = size() + static_cast<u32>(file.categories.size());
  if (total > k_max_categories) {
    error = "categories: " + std::to_string(total) + " in all, more than the " +
            std::to_string(k_max_categories) + " a class holds";
    return false;
  }
  for (u32 i = 0; i < file.categories.size(); ++i) {
    const content::ContentCategory& c = file.categories[i];
    const std::string where = "categories[" + std::to_string(i) + "]";
    if (c.name.empty()) {
      error = where + ": a category has no name";
      return false;
    }
    if (!valid_name(c.name)) {
      error = where + ": '" + c.name + "' is not a category name (" + k_name_rule + ")";
      return false;
    }
    bool known = find(c.name) >= 0;
    for (u32 j = 0; j < i; ++j)
      known |= file.categories[j].name == c.name;
    if (known) {
      error = where + ": '" + c.name + "' is already a category";
      return false;
    }
    if (!check_ladder(c.name, c.levels, where, error)) return false;
  }
  for (u32 i = 0; i < file.categories.size(); ++i) {
    const content::ContentCategory& c = file.categories[i];
    names_.push_back(c.name);
    Vector<std::string> ladder;
    for (u32 l = 0; l < c.levels.size(); ++l)
      ladder.push_back(c.levels[l]);
    ladders_.push_back(std::move(ladder));
  }
  return true;
}

bool Categories::add_json(std::string_view json, std::string& error) {
  content::ContentCategoriesFile file;
  if (!read_file_json(json, file, "categories file", error)) return false;
  return add(file, error);
}

i32 Categories::find(std::string_view name) const {
  for (u32 i = 0; i < size(); ++i)
    if (names_[i] == name) return static_cast<i32>(i);
  return -1;
}

i32 Categories::level(u32 category, std::string_view name) const {
  const Vector<std::string>& ladder = ladders_[category];
  for (u32 i = 0; i < ladder.size(); ++i)
    if (ladder[i] == name) return static_cast<i32>(i);
  return -1;
}

namespace {

// One map of category name to level name, as a sidecar declares it, read into `into`.
template <class LevelMap>
bool read_levels(const Categories& categories, const LevelMap& levels, const std::string& where,
                 ContentClasses& into, std::string& error) {
  for (u32 i = 0; i < levels.size(); ++i) {
    const std::string& name = levels.key_at(i);
    const i32 category = categories.find(name);
    if (category < 0) {
      error = where + ": '" + name + "' is not a category the engine or the game defines";
      return false;
    }
    const u32 c = static_cast<u32>(category);
    const i32 level = categories.level(c, levels.value_at(i));
    // A level the ladder does not have fails closed: the ladder's highest.
    into.levels[c] = level < 0 ? categories.top(c) : static_cast<u8>(level);
  }
  return true;
}

}  // namespace

ContentClasses unreadable(const Categories& categories) {
  ContentClasses out;
  for (u32 c = 0; c < categories.size(); ++c)
    out.levels[c] = categories.top(c);
  return out;
}

bool read_classes(const Categories& categories, const content::ContentClasses& declared,
                  ContentClasses& out, std::string& error, Vector<MaterialClass>* materials) {
  // Every refusal leaves the unreadable class behind it, never the caller's value.
  const auto refuse = [&]() {
    out = unreadable(categories);
    if (materials) materials->clear();
    return false;
  };
  ContentClasses classes;
  if (!read_levels(categories, declared.levels, "content_classes", classes, error)) return refuse();
  Vector<MaterialClass> per_material;
  for (u32 m = 0; m < declared.materials.size(); ++m) {
    MaterialClass material{declared.materials.key_at(m), ContentClasses{}};
    const std::string where = "content_classes.materials['" + material.name + "']";
    if (!read_levels(categories, declared.materials.value_at(m).levels, where, material.classes,
                     error))
      return refuse();
    // v0 decides per mesh: a material's class is the asset's too.
    classes = join(classes, material.classes);
    per_material.push_back(std::move(material));
  }
  out = classes;
  if (materials) *materials = std::move(per_material);
  return true;
}

bool read_sidecar_classes(const Categories& categories, std::string_view sidecar_json,
                          ContentClasses& out, std::string& error,
                          Vector<MaterialClass>* materials) {
  const auto refuse = [&]() {
    out = unreadable(categories);
    if (materials) materials->clear();
    return false;
  };
  JsonValue sidecar;
  if (!parse_strict(sidecar_json, sidecar, "sidecar", error)) return refuse();
  if (!sidecar.is_object()) {
    error = "sidecar: not a JSON object";
    return refuse();
  }
  const JsonValue* declared = sidecar.find("content_classes");
  if (declared == nullptr) {  // declaring nothing is none everywhere
    out = ContentClasses{};
    if (materials) materials->clear();
    return true;
  }
  if (!declared->is_object()) {
    error = "sidecar: content_classes is not an object";
    return refuse();
  }
  const JsonValue::Object& members = declared->as_object();
  const bool schema_form = declared->contains("levels") || declared->contains("materials");
  content::ContentClasses typed;
  if (schema_form) {
    for (u32 i = 0; i < members.size(); ++i) {
      const std::string& key = members.key_at(i);
      if (key != "levels" && key != "materials") {
        error = "sidecar: content_classes mixes the schema's form with a category ('" + key + "')";
        return refuse();
      }
    }
    schema::ReadContext ctx;
    if (!schema::from_json(typed, *declared, ctx) || !ctx.ok()) {
      error = "sidecar: content_classes: " + schema_error(ctx);
      return refuse();
    }
  } else {
    // The generation tools' flat map, category to level, read as the asset's levels.
    for (u32 i = 0; i < members.size(); ++i) {
      const JsonValue& level = members.value_at(i);
      if (!level.is_string()) {
        error = "sidecar: content_classes: '" + members.key_at(i) + "' is not a level name";
        return refuse();
      }
      typed.levels.insert(members.key_at(i), std::string(level.as_string()));
    }
  }
  return read_classes(categories, typed, out, error, materials);
}

namespace {

NamedPolicy compiled_in_default() {
  NamedPolicy named;
  named.name = std::string(k_agent_default_name);
  named.policy = agent_default();
  return named;
}

}  // namespace

ViewPolicy resolve(const NamedPolicy& named, const Categories& categories) {
  // A game category's cap comes from its by-name cap alone, so a policy resolved against one
  // table and then another keeps nothing of the first.
  ViewPolicy out = named.policy;
  for (u32 c = k_engine_categories; c < k_max_categories; ++c) {
    out.caps[c] = k_none;
    out.substitutions[c] = Substitution::omit;
  }
  for (u32 k = 0; k < named.by_name.size(); ++k) {
    const NamedCap& cap = named.by_name[k];
    const i32 category = categories.find(cap.category);
    if (category < 0) continue;  // not met yet: `none` with `omit`, as an unnamed category
    const u32 c = static_cast<u32>(category);
    const i32 level = categories.level(c, cap.cap);
    if (level < 0) {
      // A level the ladder does not have is a cap the operator did not mean, so the stricter
      // reading is taken for all of it.
      out.caps[c] = k_none;
      out.substitutions[c] = Substitution::omit;
    } else {
      out.caps[c] = static_cast<u8>(level);
      out.substitutions[c] = cap.substitution;
    }
  }
  return out;
}

ViewPolicies::ViewPolicies() { policies_.push_back(compiled_in_default()); }

void ViewPolicies::resolve(const Categories& categories) {
  for (u32 i = 0; i < policies_.size(); ++i)
    policies_[i].policy = content_class::resolve(policies_[i], categories);
}

bool ViewPolicies::read(const Categories& categories, const content::ViewPoliciesFile& file,
                        std::string& error) {
  Vector<NamedPolicy> parsed;
  parsed.push_back(compiled_in_default());
  for (u32 i = 0; i < file.policies.size(); ++i) {
    const content::ViewPolicy& p = file.policies[i];
    const std::string where = "policies[" + std::to_string(i) + "]";
    if (p.name.empty()) {
      error = where + ": a policy has no name";
      return false;
    }
    if (p.name == k_agent_default_name) {
      error = where + ": 'agent-default' is compiled in and may not be redefined";
      return false;
    }
    for (u32 j = 0; j < parsed.size(); ++j) {
      if (parsed[j].name == p.name) {
        error = where + ": '" + p.name + "' is named twice";
        return false;
      }
    }
    NamedPolicy named;
    named.name = p.name;
    for (u32 k = 0; k < p.caps.size(); ++k) {
      const content::ViewPolicyCap& cap = p.caps[k];
      const std::string at = where + " '" + p.name + "' caps[" + std::to_string(k) + "]";
      if (!Categories::valid_name(cap.category)) {
        error = at + ": '" + cap.category + "' is not a category name (" + k_name_rule + ")";
        return false;
      }
      for (u32 j = 0; j < k; ++j) {
        if (p.caps[j].category == cap.category) {
          error = at + ": '" + cap.category + "' is capped twice";
          return false;
        }
      }
      // An engine category's ladder is compiled in, so its cap is checked now; any other
      // category is capped by name and resolved when its ladder is in the table.
      const i32 category = categories.find(cap.category);
      const bool engine = category >= 0 && static_cast<u32>(category) < k_engine_categories;
      const i32 level = engine ? categories.level(static_cast<u32>(category), cap.cap) : 0;
      if (level < 0) {
        error = at + ": '" + cap.cap + "' is not a level of '" + cap.category + "'";
        return false;
      }
      Substitution substitution = Substitution::omit;
      if (cap.substitution == "omit") {
        substitution = Substitution::omit;
      } else if (cap.substitution == "proxy") {
        substitution = Substitution::proxy;
      } else if (cap.substitution == "cover" || cap.substitution == "obscure at capture") {
        error = at + ": '" + cap.substitution +
                "' is not built yet (it comes with the character work); use 'omit' or 'proxy'";
        return false;
      } else {
        error = at + ": '" + cap.substitution +
                "' is not a substitution ('omit', 'proxy', 'cover', 'obscure at capture')";
        return false;
      }
      if (engine) {
        const u32 c = static_cast<u32>(category);
        named.policy.caps[c] = static_cast<u8>(level);
        named.policy.substitutions[c] = substitution;
      } else {
        named.by_name.push_back(NamedCap{cap.category, cap.cap, substitution});
      }
    }
    named.policy = content_class::resolve(named, categories);
    parsed.push_back(std::move(named));
  }
  policies_ = std::move(parsed);
  return true;
}

bool ViewPolicies::read_json(const Categories& categories, std::string_view json,
                             std::string& error) {
  content::ViewPoliciesFile file;
  if (!read_file_json(json, file, "policies file", error)) return false;
  return read(categories, file, error);
}

const ViewPolicy* ViewPolicies::find(std::string_view name) const {
  for (u32 i = 0; i < policies_.size(); ++i)
    if (policies_[i].name == name) return &policies_[i].policy;
  return nullptr;
}

}  // namespace engine::content_class
