// foundation/content_class: the vocabulary and the decision (docs/subsystems/content_class.md).
// Every fixture is neutral: a made-up game category, `fixture_glow` (`none`, `dim`, `bright`),
// and the engine's three categories as names only. Nothing classified is opened or made.
#include <foundation/content_class/content_class.h>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <schemas/provenance.h>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::content_class;

namespace {

const char* const k_glow_file =
    R"({"categories":[{"name":"fixture_glow","levels":["none","dim","bright"]}]})";

Categories with_glow() {
  Categories c;
  std::string error;
  REQUIRE(c.add_json(k_glow_file, error));
  return c;
}

u8 glow() { return static_cast<u8>(k_engine_categories); }

ContentClasses classes_of(u8 category, u8 level) {
  ContentClasses c;
  c.levels[category] = level;
  return c;
}

ViewPolicy policy_of(u8 category, u8 cap, Substitution s) {
  ViewPolicy p;
  p.caps[category] = cap;
  p.substitutions[category] = s;
  return p;
}

bool read_policies(const Categories& categories, const char* json, ViewPolicies& out,
                   std::string& error) {
  return out.read_json(categories, json, error);
}

Vector<std::string> ladder_of(std::initializer_list<const char*> levels) {
  Vector<std::string> out;
  for (const char* level : levels)
    out.push_back(level);
  return out;
}

}  // namespace

TEST_CASE("content_class: the engine's three ladders are ADR-0033's, in order") {
  const Categories c;
  REQUIRE(c.size() == 3);
  CHECK(c.find("nudity") == k_nudity);
  CHECK(c.find("sexual") == k_sexual);
  CHECK(c.find("violence") == k_violence);
  CHECK(c.level(k_nudity, "none") == 0);
  CHECK(c.level(k_nudity, "partial") == 1);
  CHECK(c.level(k_nudity, "full") == 2);
  CHECK(c.level(k_sexual, "suggestive") == 1);
  CHECK(c.level(k_sexual, "explicit") == 2);
  CHECK(c.level(k_violence, "mild") == 1);
  CHECK(c.level(k_violence, "graphic") == 2);
  CHECK(c.top(k_violence) == 2);
  CHECK(c.find("fixture_glow") == -1);
}

TEST_CASE(
    "content_class: agent-default is ADR-0033 decision 7's caps and the owner's substitutions") {
  const ViewPolicy p = agent_default();
  CHECK(p.caps[k_nudity] == 0);
  CHECK(p.caps[k_sexual] == 0);
  CHECK(p.caps[k_violence] == 1);
  for (u32 i = k_engine_categories; i < k_max_categories; ++i)
    CHECK(p.caps[i] == 0);
  // The owner, 2026-11-18 (ADR-0073, question 2): a box for nudity and sexual, nothing for
  // violence; a game's category, which no answer named, omitted.
  CHECK(k_agent_default_substitutions[k_nudity] == Substitution::proxy);
  CHECK(k_agent_default_substitutions[k_sexual] == Substitution::proxy);
  CHECK(k_agent_default_substitutions[k_violence] == Substitution::omit);
  CHECK(k_agent_default_game_substitution == Substitution::omit);
  CHECK(p.substitutions[k_nudity] == Substitution::proxy);
  CHECK(p.substitutions[k_sexual] == Substitution::proxy);
  CHECK(p.substitutions[k_violence] == Substitution::omit);
  for (u32 i = k_engine_categories; i < k_max_categories; ++i)
    CHECK(p.substitutions[i] == Substitution::omit);
  // What that means for content: a nudity class above the cap is a box, violence above it and a
  // game category above none are omitted.
  CHECK(decide(classes_of(k_nudity, 1), p).outcome == Outcome::proxy);
  CHECK(decide(classes_of(k_sexual, 2), p).outcome == Outcome::proxy);
  CHECK(decide(classes_of(k_violence, 2), p).outcome == Outcome::omit);
  CHECK(decide(classes_of(glow(), 1), p).outcome == Outcome::omit);
  CHECK(decide(join(classes_of(k_nudity, 1), classes_of(k_violence, 2)), p).outcome ==
        Outcome::omit);
  const ViewPolicies set;
  REQUIRE(set.size() == 1);
  CHECK(set.at(0).name == "agent-default");
  REQUIRE(set.find("agent-default") != nullptr);
  CHECK(*set.find("agent-default") == p);
}

TEST_CASE("content_class: decide, the table of cases") {
  const u8 g = glow();
  struct Case {
    const char* what;
    ContentClasses classes;
    ViewPolicy policy;
    Decision expected;
  };
  ViewPolicy mixed;  // glow capped at none with proxy; violence at none with omit
  mixed.substitutions[g] = Substitution::proxy;
  const Case cases[] = {
      {"nothing classified, narrowest policy", {}, {}, {}},
      {"nothing classified, agent-default", {}, agent_default(), {}},
      {"within the cap", classes_of(k_violence, 1), agent_default(), {}},
      {"at a game category's cap", classes_of(g, 1), policy_of(g, 1, Substitution::omit), {}},
      {"above the cap, omit",
       classes_of(k_violence, 2),
       agent_default(),
       {Outcome::omit, k_violence, 2, 1}},
      {"above the cap, proxy",
       classes_of(g, 2),
       policy_of(g, 1, Substitution::proxy),
       {Outcome::proxy, g, 2, 1}},
      {"a game category beside the engine's, the game's decides",
       join(classes_of(k_violence, 1), classes_of(g, 1)),
       agent_default(),
       {Outcome::omit, g, 1, 0}},
      {"omit is stricter than proxy whatever the order",
       join(classes_of(k_violence, 1), classes_of(g, 2)),
       mixed,
       {Outcome::omit, k_violence, 1, 0}},
      {"proxy alone", classes_of(g, 2), mixed, {Outcome::proxy, g, 2, 0}},
      {"a level past a ladder's top is above every cap",
       classes_of(k_nudity, 200),
       policy_of(k_nudity, 2, Substitution::proxy),
       {Outcome::proxy, k_nudity, 200, 2}},
      {"a category the policy does not name is none with omit",
       classes_of(g, 1),
       policy_of(k_violence, 2, Substitution::proxy),
       {Outcome::omit, g, 1, 0}},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    CHECK(decide(c.classes, c.policy) == c.expected);
    CHECK(within(c.classes, c.policy) == (c.expected.outcome == Outcome::admit));
  }
}

TEST_CASE(
    "content_class: read_classes refuses an unknown category and reads an unknown level as the "
    "top") {
  const Categories categories = with_glow();
  std::string error;
  ContentClasses out;

  content::ContentClasses declared;
  declared.levels.insert(std::string("fixture_glow"), std::string("dim"));
  declared.levels.insert(std::string("violence"), std::string("mild"));
  REQUIRE(read_classes(categories, declared, out, error));
  CHECK(out.levels[glow()] == 1);
  CHECK(out.levels[k_violence] == 1);
  CHECK(out.levels[k_nudity] == 0);

  content::ContentClasses unknown_level;
  unknown_level.levels.insert(std::string("fixture_glow"), std::string("blinding"));
  REQUIRE(read_classes(categories, unknown_level, out, error));
  CHECK(out.levels[glow()] == categories.top(glow()));

  content::ContentClasses unknown_category;
  unknown_category.levels.insert(std::string("fixture_hum"), std::string("none"));
  out = classes_of(k_nudity, 1);
  CHECK_FALSE(read_classes(categories, unknown_category, out, error));
  CHECK(error.find("fixture_hum") != std::string::npos);
  // A refusal leaves the unreadable class, never the caller's value: a caller that ignores the
  // answer still fails closed.
  CHECK(out == unreadable(categories));
  CHECK(decide(out, agent_default()).outcome == Outcome::omit);

  // The game's category is unknown to a table that was not given the game's file.
  const Categories engine_only;
  out = ContentClasses{};
  CHECK_FALSE(read_classes(engine_only, declared, out, error));
  CHECK(out == unreadable(engine_only));
  CHECK_FALSE(is_none(out));

  // Declaring nothing is none everywhere.
  REQUIRE(read_classes(categories, content::ContentClasses{}, out, error));
  CHECK(is_none(out));
}

TEST_CASE("content_class: the unreadable class is every known category at its top") {
  const Categories categories = with_glow();
  const ContentClasses u = unreadable(categories);
  CHECK(u.levels[k_nudity] == 2);
  CHECK(u.levels[k_sexual] == 2);
  CHECK(u.levels[k_violence] == 2);
  CHECK(u.levels[glow()] == 2);
  for (u32 i = categories.size(); i < k_max_categories; ++i)
    CHECK(u.levels[i] == 0);
  // Only a policy capping every known category at its top admits it.
  ViewPolicy everything;
  for (u32 c = 0; c < categories.size(); ++c)
    everything.caps[c] = categories.top(c);
  CHECK(decide(u, everything).outcome == Outcome::admit);
  ViewPolicy one_short = everything;
  one_short.caps[glow()] = 1;
  one_short.substitutions[glow()] = Substitution::proxy;
  CHECK(decide(u, one_short).outcome == Outcome::proxy);

  // A refused material leaves no per-material classes behind it either.
  content::ContentClasses declared;
  content::MaterialContentClasses hum;
  hum.levels.insert(std::string("fixture_hum"), std::string("none"));
  declared.materials.insert(std::string("m0"), std::move(hum));
  ContentClasses out;
  Vector<MaterialClass> materials;
  materials.push_back(MaterialClass{"stale", classes_of(k_nudity, 1)});
  std::string error;
  CHECK_FALSE(read_classes(categories, declared, out, error, &materials));
  CHECK(out == u);
  CHECK(materials.empty());
}

TEST_CASE("content_class: a class declared per material is read and joined into the asset's") {
  const Categories categories = with_glow();
  std::string error;

  // A class only under `materials`: before the mend it read as none and was admitted.
  content::ContentClasses declared;
  content::MaterialContentClasses bright;
  bright.levels.insert(std::string("fixture_glow"), std::string("bright"));
  declared.materials.insert(std::string("m0"), std::move(bright));
  content::MaterialContentClasses plain;
  plain.levels.insert(std::string("violence"), std::string("none"));
  declared.materials.insert(std::string("m1"), std::move(plain));
  ContentClasses out;
  Vector<MaterialClass> materials;
  REQUIRE(read_classes(categories, declared, out, error, &materials));
  CHECK(out.levels[glow()] == 2);
  CHECK(decide(out, agent_default()).outcome == Outcome::omit);
  REQUIRE(materials.size() == 2);
  CHECK(materials[0].name == "m0");
  CHECK(materials[0].classes == classes_of(glow(), 2));
  CHECK(materials[1].name == "m1");
  CHECK(is_none(materials[1].classes));

  // Joined with the asset's own levels, per category.
  declared.levels.insert(std::string("violence"), std::string("mild"));
  REQUIRE(read_classes(categories, declared, out, error));
  CHECK(out == join(classes_of(glow(), 2), classes_of(k_violence, 1)));

  // A material's level its ladder does not have reads as the top.
  content::ContentClasses unknown_level;
  content::MaterialContentClasses blinding;
  blinding.levels.insert(std::string("fixture_glow"), std::string("blinding"));
  unknown_level.materials.insert(std::string("m0"), std::move(blinding));
  REQUIRE(read_classes(categories, unknown_level, out, error));
  CHECK(out.levels[glow()] == categories.top(glow()));

  // An unknown category under a material is refused as under the asset's levels.
  content::ContentClasses unknown_category;
  content::MaterialContentClasses hum;
  hum.levels.insert(std::string("fixture_hum"), std::string("none"));
  unknown_category.materials.insert(std::string("m0"), std::move(hum));
  CHECK_FALSE(read_classes(categories, unknown_category, out, error));
  CHECK(error.find("fixture_hum") != std::string::npos);
  CHECK(error.find("materials") != std::string::npos);
}

TEST_CASE(
    "content_class: a key given twice is refused in a sidecar, a categories file and a "
    "policies file") {
  const Categories categories = with_glow();
  std::string error;

  // A sidecar that declares a class and then, by a merge or an append, `none`: before the
  // mend the last won and the class was lost.
  ContentClasses out;
  CHECK_FALSE(read_sidecar_classes(categories,
                                   R"({"schema":"engine.generation.provenance/1","name":"cube",
          "content_classes":{"levels":{"fixture_glow":"bright","fixture_glow":"none"}}})",
                                   out, error));
  CHECK(error.find("'fixture_glow' is given twice") != std::string::npos);
  CHECK(out == unreadable(categories));
  // Anywhere in the sidecar, not only under content_classes.
  CHECK_FALSE(read_sidecar_classes(categories, R"({"name":"a","name":"b"})", out, error));
  CHECK(error.find("'name' is given twice") != std::string::npos);
  // The same sidecar without the second key reads.
  REQUIRE(read_sidecar_classes(
      categories, R"({"name":"cube","content_classes":{"levels":{"fixture_glow":"bright"}}})", out,
      error));
  CHECK(out == classes_of(glow(), 2));
  // No content_classes at all is none; not JSON is unreadable.
  REQUIRE(read_sidecar_classes(categories, R"({"name":"cube"})", out, error));
  CHECK(is_none(out));
  CHECK_FALSE(read_sidecar_classes(categories, "{", out, error));
  CHECK(out == unreadable(categories));

  // A categories file with a key given twice, refused whole.
  Categories more;
  CHECK_FALSE(more.add_json(
      R"({"categories":[{"name":"fixture_hum","name":"fixture_buzz","levels":["none"]}]})", error));
  CHECK(error.find("'name' is given twice") != std::string::npos);
  CHECK(more.size() == k_engine_categories);

  // A policies file whose cap is given twice: before the mend it read as the second, `full`.
  ViewPolicies set;
  CHECK_FALSE(set.read_json(
      categories,
      R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":"none","cap":"full","substitution":"omit"}]}]})",
      error));
  CHECK(error.find("'cap' is given twice") != std::string::npos);
  CHECK(set.size() == 1);
  CHECK(set.find("a") == nullptr);
}

TEST_CASE("content_class: category names are [a-z0-9_], and ladders are learned from data") {
  std::string error;
  CHECK(Categories::valid_name("fixture_glow2"));
  const std::string tabbed = std::string("fixture") + static_cast<char>(9) + "glow";
  for (const std::string& bad : {std::string("Nudity"), std::string("nudity "), tabbed,
                                 std::string("levels"), std::string("materials"), std::string()}) {
    INFO(bad);
    CHECK_FALSE(Categories::valid_name(bad));
  }
  Categories file_table;
  CHECK_FALSE(
      file_table.add_json(R"({"categories":[{"name":"Nudity","levels":["none","x"]}]})", error));
  CHECK(error.find("not a category name") != std::string::npos);
  CHECK(file_table.size() == k_engine_categories);

  Categories table;  // a host's: given no categories file
  const Vector<std::string> glow_ladder = [] {
    Vector<std::string> l;
    l.push_back("none");
    l.push_back("dim");
    l.push_back("bright");
    return l;
  }();
  u32 index = 99;
  REQUIRE(table.learn("fixture_glow", glow_ladder, index, error));
  CHECK(index == k_engine_categories);
  CHECK(table.level(index, "bright") == 2);
  REQUIRE(table.learn("fixture_glow", glow_ladder, index, error));  // the same ladder again
  CHECK(index == k_engine_categories);
  CHECK(table.size() == k_engine_categories + 1);
  Vector<std::string> reordered = glow_ladder;
  std::swap(reordered[1], reordered[2]);
  CHECK_FALSE(table.learn("fixture_glow", reordered, index, error));  // another ladder
  CHECK(error.find("differs") != std::string::npos);
  Vector<std::string> short_violence;
  short_violence.push_back("none");
  short_violence.push_back("graphic");
  CHECK_FALSE(table.learn("violence", short_violence, index, error));  // an engine category's
  Vector<std::string> fill;
  fill.push_back("none");
  for (u32 i = table.size(); i < k_max_categories; ++i)
    REQUIRE(table.learn("fixture_" + std::to_string(i), fill, index, error));
  CHECK(table.size() == k_max_categories);
  CHECK_FALSE(table.learn("fixture_seventeen", fill, index, error));  // a seventeenth
  CHECK(table.size() == k_max_categories);
}

TEST_CASE("content_class: a sidecar's classes are read in both forms") {
  const Categories categories = with_glow();
  std::string error;
  ContentClasses out;
  // The generation tools' flat map, read as the asset's levels.
  REQUIRE(read_sidecar_classes(
      categories,
      R"({"content_class":"general","content_classes":{"nudity":"none","sexual":"none","violence":"none"}})",
      out, error));
  CHECK(is_none(out));
  REQUIRE(read_sidecar_classes(categories, R"({"content_classes":{"fixture_glow":"dim"}})", out,
                               error));
  CHECK(out == classes_of(glow(), 1));
  // The schema's form.
  REQUIRE(read_sidecar_classes(
      categories, R"({"content_classes":{"materials":{"m0":{"levels":{"violence":"mild"}}}}})", out,
      error));
  CHECK(out == classes_of(k_violence, 1));
  // Refused, each leaving the unreadable class: a mix of the two forms, a level of the wrong
  // type, and content_classes that is not an object.
  for (const char* bad :
       {R"({"content_classes":{"levels":{},"fixture_glow":"dim"}})",
        R"({"content_classes":{"fixture_glow":2}})", R"({"content_classes":"none"})"}) {
    INFO(bad);
    out = ContentClasses{};
    CHECK_FALSE(read_sidecar_classes(categories, bad, out, error));
    CHECK(out == unreadable(categories));
  }
}

TEST_CASE("content_class: join is the per-category maximum") {
  const ContentClasses a = join(classes_of(k_nudity, 1), classes_of(glow(), 2));
  const ContentClasses b = join(classes_of(k_nudity, 2), classes_of(k_violence, 1));
  const ContentClasses j = join(a, b);
  CHECK(j.levels[k_nudity] == 2);
  CHECK(j.levels[k_violence] == 1);
  CHECK(j.levels[glow()] == 2);
  CHECK(join(a, b) == join(b, a));
  CHECK(join(a, ContentClasses{}) == a);
}

TEST_CASE("content_class: narrowing never widens (property)") {
  std::mt19937 rng(0x5EEDu);
  auto random_policy = [&rng]() {
    ViewPolicy p;
    for (u32 i = 0; i < k_max_categories; ++i) {
      p.caps[i] = static_cast<u8>(rng() % 4);
      p.substitutions[i] = static_cast<Substitution>(rng() % 2);
    }
    return p;
  };
  auto random_classes = [&rng]() {
    ContentClasses c;
    for (u32 i = 0; i < k_max_categories; ++i)
      c.levels[i] = rng() % 3 == 0 ? static_cast<u8>(rng() % 5) : u8{0};
    return c;
  };
  for (u32 trial = 0; trial < 2000; ++trial) {
    const ViewPolicy a = random_policy();
    const ViewPolicy b = random_policy();
    const ViewPolicy n = narrow(a, b);
    CHECK(narrow(a, b) == narrow(b, a));
    CHECK(narrow(a, a) == a);
    for (u32 k = 0; k < 8; ++k) {
      const ContentClasses c = random_classes();
      const Decision da = decide(c, a);
      const Decision dn = decide(c, n);
      // What the original refused, the narrowed refuses; what it omitted, the narrowed omits.
      if (da.outcome != Outcome::admit) CHECK(dn.outcome != Outcome::admit);
      if (da.outcome == Outcome::omit) CHECK(dn.outcome == Outcome::omit);
      if (within(c, n)) {
        CHECK(within(c, a));
        CHECK(within(c, b));
      }
    }
  }
}

TEST_CASE("content_class: a categories file that is wrong is refused whole, with the reason") {
  struct Case {
    const char* what;
    const char* json;
    const char* reason;
  };
  const Case cases[] = {
      {"not JSON", "{", "not JSON"},
      {"an unknown field", R"({"categories":[],"extra":1})", "extra"},
      {"no name", R"({"categories":[{"name":"","levels":["none"]}]})", "no name"},
      {"an engine name", R"({"categories":[{"name":"violence","levels":["none","x"]}]})",
       "already a category"},
      {"a name twice",
       R"({"categories":[{"name":"fixture_glow","levels":["none"]},{"name":"fixture_glow","levels":["none"]}]})",
       "already a category"},
      {"no levels", R"({"categories":[{"name":"fixture_glow","levels":[]}]})", "'none'"},
      {"not starting at none",
       R"({"categories":[{"name":"fixture_glow","levels":["dim","none"]}]})", "'none'"},
      {"a level twice", R"({"categories":[{"name":"fixture_glow","levels":["none","dim","dim"]}]})",
       "twice"},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    Categories categories;
    std::string error;
    CHECK_FALSE(categories.add_json(c.json, error));
    CHECK(error.find(c.reason) != std::string::npos);
    CHECK(categories.size() == k_engine_categories);  // unchanged
  }
  // More categories than a class holds.
  std::string many = R"({"categories":[)";
  for (u32 i = 0; i < k_max_categories - k_engine_categories + 1; ++i)
    many += std::string(i ? "," : "") + R"({"name":"fixture_)" + std::to_string(i) +
            R"(","levels":["none"]})";
  many += "]}";
  Categories categories;
  std::string error;
  CHECK_FALSE(categories.add_json(many, error));
  CHECK(error.find(std::to_string(k_max_categories)) != std::string::npos);
  CHECK(categories.size() == k_engine_categories);
}

TEST_CASE(
    "content_class: a policies file is read, and a wrong one is refused whole, with the reason") {
  const Categories categories = with_glow();
  ViewPolicies set;
  std::string error;
  REQUIRE(read_policies(categories,
                        R"({"policies":[{"name":"fixture-review","caps":[
                          {"category":"fixture_glow","cap":"dim","substitution":"proxy"},
                          {"category":"violence","cap":"graphic","substitution":"omit"}]}]})",
                        set, error));
  REQUIRE(set.size() == 2);
  REQUIRE(set.find("fixture-review") != nullptr);
  const ViewPolicy& p = *set.find("fixture-review");
  CHECK(p.caps[glow()] == 1);
  CHECK(p.substitutions[glow()] == Substitution::proxy);
  CHECK(p.caps[k_violence] == 2);
  CHECK(p.caps[k_nudity] == 0);  // not named: none with omit
  CHECK(p.substitutions[k_nudity] == Substitution::omit);
  CHECK(*set.find("agent-default") == agent_default());

  struct Case {
    const char* what;
    const char* json;
    const char* reason;
  };
  const Case cases[] = {
      {"not JSON", "[", "not JSON"},
      {"an unknown field", R"({"policies":[{"name":"a","caps":[],"view":1}]})", "view"},
      {"an unknown substitution",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":"none","substitution":"blur"}]}]})",
       "not a substitution"},
      {"cover, not built yet",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":"none","substitution":"cover"}]}]})",
       "not built yet"},
      {"obscure at capture, not built yet",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":"none","substitution":"obscure at capture"}]}]})",
       "not built yet"},
      {"an engine category's cap its ladder does not have",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":"blinding","substitution":"omit"}]}]})",
       "not a level"},
      {"a category name outside [a-z0-9_]",
       R"({"policies":[{"name":"a","caps":[{"category":"Nudity","cap":"none","substitution":"omit"}]}]})",
       "not a category name"},
      {"a reserved name",
       R"({"policies":[{"name":"a","caps":[{"category":"levels","cap":"none","substitution":"omit"}]}]})",
       "not a category name"},
      {"a game category capped twice",
       R"({"policies":[{"name":"a","caps":[{"category":"fixture_hum","cap":"none","substitution":"omit"},{"category":"fixture_hum","cap":"loud","substitution":"proxy"}]}]})",
       "capped twice"},
      {"a number for a cap",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":0,"substitution":"omit"}]}]})",
       "cap"},
      {"an object for a cap",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":{},"substitution":"omit"}]}]})",
       "cap"},
      {"a category capped twice",
       R"({"policies":[{"name":"a","caps":[{"category":"nudity","cap":"none","substitution":"omit"},{"category":"nudity","cap":"none","substitution":"proxy"}]}]})",
       "capped twice"},
      {"a duplicate name", R"({"policies":[{"name":"a","caps":[]},{"name":"a","caps":[]}]})",
       "named twice"},
      {"no name", R"({"policies":[{"name":"","caps":[]}]})", "no name"},
      {"agent-default redefined wider",
       R"({"policies":[{"name":"agent-default","caps":[{"category":"violence","cap":"graphic","substitution":"omit"}]}]})",
       "compiled in"},
      {"agent-default redefined narrower", R"({"policies":[{"name":"agent-default","caps":[]}]})",
       "compiled in"},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    ViewPolicies before;
    REQUIRE(
        read_policies(categories, R"({"policies":[{"name":"kept","caps":[]}]})", before, error));
    CHECK_FALSE(read_policies(categories, c.json, before, error));
    CHECK(error.find(c.reason) != std::string::npos);
    // Refused whole: what the set held before is what it holds.
    CHECK(before.size() == 2);
    CHECK(before.find("kept") != nullptr);
  }
}

TEST_CASE("content_class: the class hash is over names, and nothing classified has none") {
  std::string error;
  const auto at = [](const Categories& t, const char* name) {
    REQUIRE(t.find(name) >= 0);
    return static_cast<u8>(t.find(name));
  };
  // Two tables holding the same two game categories in the other order.
  Categories glow_first;
  REQUIRE(glow_first.add_json(
      R"({"categories":[{"name":"fixture_glow","levels":["none","dim","bright"]},{"name":"fixture_hum","levels":["none","loud"]}]})",
      error));
  Categories hum_first;
  REQUIRE(hum_first.add_json(
      R"({"categories":[{"name":"fixture_hum","levels":["none","loud"]},{"name":"fixture_glow","levels":["none","dim","bright"]}]})",
      error));
  const ContentClasses a =
      join(join(classes_of(at(glow_first, "fixture_glow"), 2), classes_of(at(glow_first, "fixture_hum"), 1)),
           classes_of(k_violence, 1));
  const ContentClasses b =
      join(join(classes_of(at(hum_first, "fixture_glow"), 2), classes_of(at(hum_first, "fixture_hum"), 1)),
           classes_of(k_violence, 1));
  CHECK(a != b);  // the same class by names, at other indexes
  const u64 ha = class_hash(glow_first, a);
  CHECK(ha != 0);
  CHECK(ha == class_hash(hum_first, b));

  // Nothing classified has no hash, so an unclassified source keeps its key.
  CHECK(class_hash(glow_first, ContentClasses{}) == 0);
  Vector<MaterialClass> neutral;
  neutral.push_back(MaterialClass{"m0", ContentClasses{}});
  CHECK(class_hash(glow_first, ContentClasses{}, &neutral) == 0);

  // A renamed level, or a reordered ladder, is another class; so is another level.
  Categories renamed;
  REQUIRE(renamed.add_json(
      R"({"categories":[{"name":"fixture_glow","levels":["none","faint","bright"]},{"name":"fixture_hum","levels":["none","loud"]}]})",
      error));
  CHECK(class_hash(renamed, a) != ha);
  Categories reordered;
  REQUIRE(reordered.add_json(
      R"({"categories":[{"name":"fixture_glow","levels":["none","bright","dim"]},{"name":"fixture_hum","levels":["none","loud"]}]})",
      error));
  ContentClasses bright_first = a;
  bright_first.levels[at(reordered, "fixture_glow")] = 1;  // `bright`, by name, as in `a`
  CHECK(class_hash(reordered, bright_first) != ha);
  ContentClasses dimmer = a;
  dimmer.levels[at(glow_first, "fixture_glow")] = 1;
  CHECK(class_hash(glow_first, dimmer) != ha);

  // A classified material is part of it, by name, in whatever order the materials are given; a
  // material at `none` adds nothing; a class only on a material is classified.
  Vector<MaterialClass> two;
  two.push_back(MaterialClass{"m1", classes_of(at(glow_first, "fixture_glow"), 1)});
  two.push_back(MaterialClass{"m0", classes_of(at(glow_first, "fixture_hum"), 1)});
  Vector<MaterialClass> swapped;
  swapped.push_back(two[1]);
  swapped.push_back(two[0]);
  const u64 hm = class_hash(glow_first, a, &two);
  CHECK(hm != ha);
  CHECK(hm == class_hash(glow_first, a, &swapped));
  Vector<MaterialClass> with_neutral = two;
  with_neutral.push_back(MaterialClass{"m2", ContentClasses{}});
  CHECK(class_hash(glow_first, a, &with_neutral) == hm);
  CHECK(class_hash(glow_first, ContentClasses{}, &two) != 0);
  Vector<MaterialClass> renamed_material = two;
  renamed_material[0].name = "m9";
  CHECK(class_hash(glow_first, a, &renamed_material) != hm);
}

TEST_CASE(
    "content_class: a policy caps a game's category by name, resolved when its ladder arrives") {
  std::string error;
  Categories table;  // a host's: given no categories file
  ViewPolicies set;
  REQUIRE(set.read_json(table, R"({"policies":[{"name":"fixture-review","caps":[
      {"category":"fixture_glow","cap":"dim","substitution":"proxy"},
      {"category":"fixture_hum","cap":"loud","substitution":"proxy"},
      {"category":"violence","cap":"graphic","substitution":"omit"}]}]})",
                        error));
  REQUIRE(set.size() == 2);
  REQUIRE(set.at(1).by_name.size() == 2);
  const ViewPolicy* p = set.find("fixture-review");
  REQUIRE(p != nullptr);
  CHECK(p->caps[k_violence] == 2);  // an engine category's cap is resolved at once
  for (u32 c = k_engine_categories; c < k_max_categories; ++c) {
    CHECK(p->caps[c] == k_none);  // no game category is met yet
    CHECK(p->substitutions[c] == Substitution::omit);
  }

  // The ladder arrives with data, and the cap by name takes effect.
  u32 glow_index = 0;
  REQUIRE(table.learn("fixture_glow", ladder_of({"none", "dim", "bright"}), glow_index, error));
  set.resolve(table);
  p = set.find("fixture-review");
  CHECK(p->caps[glow_index] == 1);
  CHECK(p->substitutions[glow_index] == Substitution::proxy);
  CHECK(decide(classes_of(static_cast<u8>(glow_index), 1), *p).outcome == Outcome::admit);
  CHECK(decide(classes_of(static_cast<u8>(glow_index), 2), *p).outcome == Outcome::proxy);

  // A cap naming a level its ladder does not have is `none` with `omit`: it narrows.
  u32 hum_index = 0;
  REQUIRE(table.learn("fixture_hum", ladder_of({"none", "quiet"}), hum_index, error));
  set.resolve(table);
  p = set.find("fixture-review");
  CHECK(p->caps[hum_index] == k_none);
  CHECK(p->substitutions[hum_index] == Substitution::omit);
  CHECK(decide(classes_of(static_cast<u8>(hum_index), 1), *p).outcome == Outcome::omit);

  // A game category the policy does not name, added after the file was read, stays `none` with
  // `omit`; resolving again changes nothing; agent-default is what it was.
  u32 buzz_index = 0;
  REQUIRE(table.learn("fixture_buzz", ladder_of({"none", "on"}), buzz_index, error));
  set.resolve(table);
  p = set.find("fixture-review");
  CHECK(p->caps[buzz_index] == k_none);
  CHECK(p->substitutions[buzz_index] == Substitution::omit);
  const ViewPolicy resolved = *p;
  set.resolve(table);
  CHECK(*set.find("fixture-review") == resolved);
  CHECK(*set.find("agent-default") == agent_default());

  // Resolved against a table that holds the categories at other places, nothing of the first
  // resolution is left behind.
  Categories other;
  u32 other_hum = 0;
  REQUIRE(other.learn("fixture_hum", ladder_of({"none", "loud"}), other_hum, error));
  const ViewPolicy moved = resolve(set.at(1), other);
  CHECK(other_hum == glow_index);  // the same index now names another category
  CHECK(moved.caps[other_hum] == 1);
  CHECK(moved.substitutions[other_hum] == Substitution::proxy);
  for (u32 c = k_engine_categories + 1; c < k_max_categories; ++c)
    CHECK(moved.caps[c] == k_none);

  // A misspelt game category is capped by name and never met: it narrows, never widens.
  ViewPolicies typo;
  REQUIRE(typo.read_json(
      table,
      R"({"policies":[{"name":"typo","caps":[{"category":"fixture_glw","cap":"bright","substitution":"proxy"}]}]})",
      error));
  CHECK(decide(classes_of(static_cast<u8>(glow_index), 1), *typo.find("typo")).outcome ==
        Outcome::omit);
}

TEST_CASE("content_class: the decision table, and same_decisions outcome for outcome") {
  const u8 g = glow();
  const ViewPolicy scene = agent_default();  // nudity a box, violence mild, a game's none
  DecisionTable table(scene);
  REQUIRE(table.size() == 1);  // the all-none class, admitted
  CHECK(is_none(table.at(0).classes));
  CHECK(table.at(0).decision.outcome == Outcome::admit);
  CHECK(table.decide(ContentClasses{}).outcome == Outcome::admit);
  CHECK(table.size() == 1);  // nothing classified, nothing appended
  CHECK(table.decide(classes_of(k_violence, 1)).outcome == Outcome::admit);
  CHECK(table.decide(classes_of(k_nudity, 1)).outcome == Outcome::proxy);
  CHECK(table.size() == 3);
  CHECK(table.decide(classes_of(k_nudity, 1)).outcome == Outcome::proxy);
  CHECK(table.size() == 3);  // met again, not appended
  CHECK(table.at(2) == DecisionEntry{classes_of(k_nudity, 1), decide(classes_of(k_nudity, 1), scene)});

  // The table's own policy makes its decisions; one that omits where it proxied does not, and
  // nor does a wider one, though each is "within" where the table admitted.
  CHECK(same_decisions(table, scene));
  ViewPolicy stricter = scene;
  stricter.substitutions[k_nudity] = Substitution::omit;
  CHECK_FALSE(same_decisions(table, stricter));
  ViewPolicy wider = scene;
  wider.caps[k_nudity] = 1;
  CHECK_FALSE(same_decisions(table, wider));
  // A policy that differs only where the table has met nothing makes the same decisions.
  ViewPolicy other = scene;
  other.caps[g] = 2;
  other.substitutions[g] = Substitution::proxy;
  CHECK(same_decisions(table, other));

  // The second check reads only what the table gained since the first.
  const u32 first_check = table.size();
  CHECK(same_decisions(table, other, first_check));  // nothing new yet
  CHECK(table.decide(classes_of(g, 1)).outcome == Outcome::omit);
  CHECK(table.size() == first_check + 1);
  CHECK_FALSE(same_decisions(table, other, first_check));  // `other` admits what was omitted
  CHECK(same_decisions(table, scene, first_check));
  CHECK(same_decisions(table, ViewPolicy{}, table.size()));  // past the end: nothing to check
}

TEST_CASE("content_class: wider_than agent-default") {
  const ViewPolicy d = agent_default();
  CHECK_FALSE(wider_than(d, d));
  CHECK_FALSE(wider_than(ViewPolicy{}, d));  // the narrowest there is
  ViewPolicy lower = d;
  lower.caps[k_violence] = 0;
  CHECK_FALSE(wider_than(lower, d));
  ViewPolicy higher = d;
  higher.caps[k_violence] = 2;
  CHECK(wider_than(higher, d));
  ViewPolicy game = d;
  game.caps[glow()] = 1;
  CHECK(wider_than(game, d));
  ViewPolicy boxed = d;
  boxed.substitutions[k_violence] = Substitution::proxy;  // a box where agent-default omits
  CHECK(wider_than(boxed, d));
  // Narrowed by agent-default, no policy is wider than it.
  std::mt19937 rng(0xD1DEu);
  for (u32 trial = 0; trial < 500; ++trial) {
    ViewPolicy p;
    for (u32 i = 0; i < k_max_categories; ++i) {
      p.caps[i] = static_cast<u8>(rng() % 256);
      p.substitutions[i] = static_cast<Substitution>(rng() % 2);
    }
    CHECK_FALSE(wider_than(narrow(p, d), d));
  }
}

TEST_CASE("content_class: deciding where nothing is classified costs a branch") {
  // Timed beside the bench row content_class.decide.none; the number is printed, not asserted,
  // because the machine is shared (docs/subsystems/content_class.md, "What it costs").
  static ContentClasses classes[4096] = {};
  const ViewPolicy policy = agent_default();
  u32 admitted = 0;
  const auto start = std::chrono::steady_clock::now();
  for (u32 round = 0; round < 64; ++round)
    for (const ContentClasses& c : classes)
      admitted += decide(c, policy).outcome == Outcome::admit ? 1u : 0u;
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count();
  CHECK(admitted == 64u * 4096u);
  std::printf("content_class.decide.none: %.2f ns a mesh (debug build, shared machine)\n",
              static_cast<double>(ns) / (64.0 * 4096.0));
}
