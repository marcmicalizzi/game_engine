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

template <class T>
bool read_file_json(std::string_view json, T& out, const char* what, std::string& error) {
  JsonValue value;
  if (!parse_json(json, value).ok) {
    error = std::string(what) + " is not JSON";
    return false;
  }
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
    p.substitutions[i] = k_agent_default_substitution;
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
    bool known = find(c.name) >= 0;
    for (u32 j = 0; j < i; ++j)
      known |= file.categories[j].name == c.name;
    if (known) {
      error = where + ": '" + c.name + "' is already a category";
      return false;
    }
    if (c.levels.empty() || c.levels[0] != "none") {
      error = where + ": '" + c.name + "' does not start at 'none'";
      return false;
    }
    if (c.levels.size() > k_max_levels) {
      error =
          where + ": '" + c.name + "' has more than " + std::to_string(k_max_levels) + " levels";
      return false;
    }
    for (u32 a = 0; a < c.levels.size(); ++a) {
      if (c.levels[a].empty()) {
        error = where + ": '" + c.name + "' has an empty level";
        return false;
      }
      for (u32 b = 0; b < a; ++b) {
        if (c.levels[a] == c.levels[b]) {
          error = where + ": '" + c.name + "' names level '" + c.levels[a] + "' twice";
          return false;
        }
      }
    }
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

i32 Categories::level(u32 category, std::string_view level) const {
  const Vector<std::string>& ladder = ladders_[category];
  for (u32 i = 0; i < ladder.size(); ++i)
    if (ladder[i] == level) return static_cast<i32>(i);
  return -1;
}

bool read_classes(const Categories& categories, const content::ContentClasses& declared,
                  ContentClasses& out, std::string& error) {
  ContentClasses classes;
  for (u32 i = 0; i < declared.levels.size(); ++i) {
    const std::string& name = declared.levels.key_at(i);
    const i32 category = categories.find(name);
    if (category < 0) {
      error = "content_classes: '" + name + "' is not a category the engine or the game defines";
      return false;
    }
    const u32 c = static_cast<u32>(category);
    const i32 level = categories.level(c, declared.levels.value_at(i));
    // A level the ladder does not have fails closed: the ladder's highest.
    classes.levels[c] = level < 0 ? categories.top(c) : static_cast<u8>(level);
  }
  out = classes;
  return true;
}

ViewPolicies::ViewPolicies() {
  policies_.push_back(NamedPolicy{std::string(k_agent_default_name), agent_default()});
}

bool ViewPolicies::read(const Categories& categories, const content::ViewPoliciesFile& file,
                        std::string& error) {
  Vector<NamedPolicy> read;
  read.push_back(NamedPolicy{std::string(k_agent_default_name), agent_default()});
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
    for (u32 j = 0; j < read.size(); ++j) {
      if (read[j].name == p.name) {
        error = where + ": '" + p.name + "' is named twice";
        return false;
      }
    }
    NamedPolicy named{p.name, ViewPolicy{}};
    bool seen[k_max_categories] = {};
    for (u32 k = 0; k < p.caps.size(); ++k) {
      const content::ViewPolicyCap& cap = p.caps[k];
      const std::string at = where + " '" + p.name + "' caps[" + std::to_string(k) + "]";
      const i32 category = categories.find(cap.category);
      if (category < 0) {
        error = at + ": '" + cap.category + "' is not a category the engine or the game defines";
        return false;
      }
      const u32 c = static_cast<u32>(category);
      if (seen[c]) {
        error = at + ": '" + cap.category + "' is capped twice";
        return false;
      }
      seen[c] = true;
      const i32 level = categories.level(c, cap.cap);
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
      named.policy.caps[c] = static_cast<u8>(level);
      named.policy.substitutions[c] = substitution;
    }
    read.push_back(std::move(named));
  }
  policies_ = std::move(read);
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
