#include <core/hash/hash.h>
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

bool wider_than(const ViewPolicy& policy, const ViewPolicy& base) {
  return narrow(policy, base) != policy;
}

DecisionTable::DecisionTable(const ViewPolicy& under) : policy_(under) {
  entries_.push_back(DecisionEntry{ContentClasses{}, Decision{}});
}

Decision DecisionTable::decide(const ContentClasses& classes) {
  // The all-`none` class is the first entry and is admitted under every policy.
  if (is_none(classes)) return entries_[0].decision;
  for (u32 i = 1; i < entries_.size(); ++i)
    if (entries_[i].classes == classes) return entries_[i].decision;
  const Decision d = content_class::decide(classes, policy_);
  entries_.push_back(DecisionEntry{classes, d});
  return d;
}

bool same_decisions(const DecisionTable& table, const ViewPolicy& policy, u32 from) {
  for (u32 i = from; i < table.size(); ++i) {
    const DecisionEntry& entry = table.at(i);
    if (decide(entry.classes, policy).outcome != entry.decision.outcome) return false;
  }
  return true;
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

// The canonical form is length-prefixed text, so no two forms run together into one.
void put_count(std::string& out, u32 n) {
  for (u32 b = 0; b < 4; ++b)
    out.push_back(static_cast<char>((n >> (8 * b)) & 0xFFu));
}

void put_text(std::string& out, std::string_view text) {
  put_count(out, static_cast<u32>(text.size()));
  out.append(text.data(), text.size());
}

// A category or level the table cannot name (a value made by hand, past its table or its
// ladder's top) is written by number, apart from every name, since a name is never `#`.
std::string category_text(const Categories& categories, u32 c) {
  return c < categories.size() ? categories.name(c) : "#" + std::to_string(c);
}

std::string level_text(const Categories& categories, u32 c, u8 level) {
  if (c < categories.size() && level <= categories.top(c)) return categories.level_name(c, level);
  return "#" + std::to_string(level);
}

// The categories above `none` in `classes`, by name.
u32 named_order(const Categories& categories, const ContentClasses& classes,
                std::array<u32, k_max_categories>& order) {
  u32 n = 0;
  for (u32 c = 0; c < k_max_categories; ++c)
    if (classes.levels[c] != k_none) order[n++] = c;
  std::sort(order.begin(), order.begin() + n, [&categories](u32 a, u32 b) {
    return category_text(categories, a) < category_text(categories, b);
  });
  return n;
}

void put_pairs(std::string& out, const Categories& categories, const ContentClasses& classes) {
  std::array<u32, k_max_categories> order{};
  const u32 n = named_order(categories, classes, order);
  put_count(out, n);
  for (u32 i = 0; i < n; ++i) {
    put_text(out, category_text(categories, order[i]));
    put_text(out, level_text(categories, order[i], classes.levels[order[i]]));
  }
}

}  // namespace

u64 class_hash(const Categories& categories, const ContentClasses& classes,
               const Vector<MaterialClass>* materials) {
  ContentClasses all = classes;
  Vector<u32> classified;  // the materials that are not `none`, by name
  if (materials) {
    for (u32 m = 0; m < materials->size(); ++m) {
      if (is_none((*materials)[m].classes)) continue;
      all = join(all, (*materials)[m].classes);
      classified.push_back(m);
    }
    std::sort(classified.begin(), classified.end(),
              [materials](u32 a, u32 b) { return (*materials)[a].name < (*materials)[b].name; });
  }
  if (is_none(all)) return 0;  // an unclassified source keeps its key

  std::string form = "engine.content_class.hash/1";
  // Each named category's ladder, so a renamed level or a reordered ladder is another class.
  std::array<u32, k_max_categories> order{};
  const u32 n = named_order(categories, all, order);
  put_count(form, n);
  for (u32 i = 0; i < n; ++i) {
    const u32 c = order[i];
    put_text(form, category_text(categories, c));
    const u32 levels = c < categories.size() ? static_cast<u32>(categories.top(c)) + 1 : 0;
    put_count(form, levels);
    for (u32 l = 0; l < levels; ++l)
      put_text(form, categories.level_name(c, static_cast<u8>(l)));
  }
  put_pairs(form, categories, classes);
  put_count(form, static_cast<u32>(classified.size()));
  for (u32 i = 0; i < classified.size(); ++i) {
    const MaterialClass& material = (*materials)[classified[i]];
    put_text(form, material.name);
    put_pairs(form, categories, material.classes);
  }
  const u64 h = hash_bytes(form.data(), form.size());
  return h == 0 ? 1 : h;
}

std::string sidecar_path_of(std::string_view source) {
  const usize slash = source.find_last_of("/\\");
  const usize start = slash == std::string_view::npos ? 0 : slash + 1;
  const usize dot = source.find_last_of('.');
  const usize end = dot == std::string_view::npos || dot < start ? source.size() : dot;
  std::string out(source.substr(0, end));
  out += ".provenance.json";
  return out;
}

u64 classified_key(u64 key, u64 class_hash) {
  if (class_hash == 0) return key;
  const u64 words[3] = {0x636c617373656432ull /* "classed2" */, key, class_hash};
  u64 h = hash_bytes(words, sizeof(words));
  if (h == key) h ^= 0x9e3779b97f4a7c15ull;  // never the unclassified key
  return h;
}

namespace {

constexpr std::string_view k_section_form = "engine.content_classes/1";

void put_bytes(Vector<u8>& out, const std::string& form) {
  out.resize(static_cast<u32>(form.size()));
  for (u32 i = 0; i < form.size(); ++i)
    out[i] = static_cast<u8>(form[i]);
}

// A reader over `encode_classes`'s form that refuses to run past the end.
struct FormReader {
  std::span<const u8> bytes;
  usize at = 0;

  bool count(u32& n) {
    if (bytes.size() - at < 4) return false;
    n = 0;
    for (u32 b = 0; b < 4; ++b)
      n |= static_cast<u32>(bytes[at + b]) << (8 * b);
    at += 4;
    return true;
  }
  bool text(std::string& out) {
    u32 n = 0;
    if (!count(n) || bytes.size() - at < n) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data() + at), n);
    at += n;
    return true;
  }
  bool done() const { return at == bytes.size(); }
};

}  // namespace

void encode_classes(const Categories& categories, const ContentClasses& classes,
                    const Vector<MaterialClass>* materials, Vector<u8>& out) {
  out.clear();
  ContentClasses all = classes;
  Vector<u32> classified;
  if (materials) {
    for (u32 m = 0; m < materials->size(); ++m) {
      if (is_none((*materials)[m].classes)) continue;
      all = join(all, (*materials)[m].classes);
      classified.push_back(m);
    }
    std::sort(classified.begin(), classified.end(),
              [materials](u32 a, u32 b) { return (*materials)[a].name < (*materials)[b].name; });
  }
  if (is_none(all)) return;  // nothing classified: no section

  std::string form;
  put_text(form, k_section_form);
  std::array<u32, k_max_categories> order{};
  const u32 n = named_order(categories, all, order);
  put_count(form, n);
  for (u32 i = 0; i < n; ++i) {
    const u32 c = order[i];
    put_text(form, category_text(categories, c));
    const u32 levels = c < categories.size() ? static_cast<u32>(categories.top(c)) + 1 : 0;
    put_count(form, levels);
    for (u32 l = 0; l < levels; ++l)
      put_text(form, categories.level_name(c, static_cast<u8>(l)));
  }
  put_pairs(form, categories, classes);
  put_count(form, static_cast<u32>(classified.size()));
  for (u32 i = 0; i < classified.size(); ++i) {
    const MaterialClass& material = (*materials)[classified[i]];
    put_text(form, material.name);
    put_pairs(form, categories, material.classes);
  }
  put_bytes(out, form);
}

bool decode_classes(Categories& categories, std::span<const u8> bytes, ContentClasses& out,
                    std::string& error, Vector<MaterialClass>* materials) {
  const auto refuse = [&](const std::string& why) {
    error = "classes section: " + why;
    out = unreadable(categories);
    if (materials) materials->clear();
    return false;
  };
  if (bytes.empty()) {  // no section: nothing classified
    out = ContentClasses{};
    if (materials) materials->clear();
    return true;
  }
  FormReader r{bytes};
  std::string form;
  if (!r.text(form) || form != k_section_form)
    return refuse("not an engine.content_classes/1 record");
  u32 n = 0;
  if (!r.count(n) || n > k_max_categories)
    return refuse("its category count is not one a class holds");
  Vector<std::string> names;
  std::array<u32, k_max_categories> index{};
  for (u32 i = 0; i < n; ++i) {
    std::string name;
    u32 levels = 0;
    if (!r.text(name) || !r.count(levels) || levels > k_max_levels)
      return refuse("a category's record runs short");
    Vector<std::string> ladder;
    for (u32 l = 0; l < levels; ++l) {
      std::string level;
      if (!r.text(level)) return refuse("category '" + name + "': its ladder runs short");
      ladder.push_back(std::move(level));
    }
    std::string why;
    if (!categories.learn(name, ladder, index[i], why)) return refuse(why);
    names.push_back(std::move(name));
  }
  // One scope's pairs: every category must be one the record listed, every level on its ladder.
  const auto read_pairs = [&](ContentClasses& into, std::string& why) {
    u32 pairs = 0;
    if (!r.count(pairs) || pairs > n) {
      why = "a scope's pair count runs past the categories it lists";
      return false;
    }
    for (u32 p = 0; p < pairs; ++p) {
      std::string category;
      std::string level;
      if (!r.text(category) || !r.text(level)) {
        why = "a scope's pairs run short";
        return false;
      }
      u32 i = 0;
      while (i < names.size() && names[i] != category)
        ++i;
      if (i == names.size()) {
        why = "a pair names '" + category + "', which the record does not list";
        return false;
      }
      const i32 l = categories.level(index[i], level);
      if (l < 0) {
        why = "category '" + category + "': '" + level + "' is not on its recorded ladder";
        return false;
      }
      into.levels[index[i]] = static_cast<u8>(l);
    }
    return true;
  };
  std::string why;
  ContentClasses classes;
  if (!read_pairs(classes, why)) return refuse(why);
  u32 count = 0;
  if (!r.count(count)) return refuse("its material count runs short");
  Vector<MaterialClass> per_material;
  for (u32 m = 0; m < count; ++m) {
    MaterialClass material;
    if (!r.text(material.name)) return refuse("a material's name runs short");
    if (!read_pairs(material.classes, why))
      return refuse("material '" + material.name + "': " + why);
    classes = join(classes, material.classes);  // the asset's, as the reader joined it
    per_material.push_back(std::move(material));
  }
  if (!r.done()) return refuse("bytes follow the record");
  out = classes;
  if (materials) *materials = std::move(per_material);
  return true;
}

bool join_encoded(Categories& categories, std::span<const std::span<const u8>> inputs,
                  Vector<u8>& out, std::string& error) {
  ContentClasses all;
  for (const std::span<const u8> input : inputs) {
    ContentClasses one;
    if (!decode_classes(categories, input, one, error)) {
      encode_classes(categories, unreadable(categories), nullptr, out);
      return false;
    }
    all = join(all, one);
  }
  encode_classes(categories, all, nullptr, out);
  return true;
}

namespace {

JsonValue pairs_json(const Categories& categories, const ContentClasses& classes) {
  JsonValue levels = JsonValue::object();
  std::array<u32, k_max_categories> order{};
  const u32 n = named_order(categories, classes, order);
  for (u32 i = 0; i < n; ++i)
    levels.set(category_text(categories, order[i]),
               JsonValue(level_text(categories, order[i], classes.levels[order[i]])));
  return levels;
}

}  // namespace

JsonValue classes_json(const Categories& categories, const ContentClasses& classes,
                       const Vector<MaterialClass>* materials) {
  ContentClasses all = classes;
  if (materials)
    for (u32 m = 0; m < materials->size(); ++m)
      all = join(all, (*materials)[m].classes);
  JsonValue out = JsonValue::object();
  JsonValue named = JsonValue::array();
  std::array<u32, k_max_categories> order{};
  const u32 n = named_order(categories, all, order);
  for (u32 i = 0; i < n; ++i) {
    const u32 c = order[i];
    JsonValue category = JsonValue::object();
    category.set("name", JsonValue(category_text(categories, c)));
    JsonValue ladder = JsonValue::array();
    if (c < categories.size())
      for (u32 l = 0; l <= categories.top(c); ++l)
        ladder.push_back(JsonValue(categories.level_name(c, static_cast<u8>(l))));
    category.set("levels", std::move(ladder));
    named.push_back(std::move(category));
  }
  out.set("categories", std::move(named));
  out.set("levels", pairs_json(categories, classes));
  JsonValue per_material = JsonValue::object();
  if (materials)
    for (u32 m = 0; m < materials->size(); ++m)
      if (!is_none((*materials)[m].classes))
        per_material.set((*materials)[m].name, pairs_json(categories, (*materials)[m].classes));
  out.set("materials", std::move(per_material));
  return out;
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
