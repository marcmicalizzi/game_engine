// foundation/content_class: the vocabulary and the decision (docs/subsystems/content_class.md).
// Every fixture is neutral: a made-up game category, `fixture_glow` (`none`, `dim`, `bright`),
// and the engine's three categories as names only. Nothing classified is opened or made.
#include <foundation/content_class/content_class.h>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <schemas/provenance.h>
#include <string>

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
  CHECK(out == classes_of(k_nudity, 1));  // untouched

  // The game's category is unknown to a table that was not given the game's file.
  const Categories engine_only;
  CHECK_FALSE(read_classes(engine_only, declared, out, error));

  // Declaring nothing is none everywhere.
  REQUIRE(read_classes(categories, content::ContentClasses{}, out, error));
  CHECK(is_none(out));
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
      {"a cap its ladder does not have",
       R"({"policies":[{"name":"a","caps":[{"category":"fixture_glow","cap":"blinding","substitution":"omit"}]}]})",
       "not a level"},
      {"a category neither defines",
       R"({"policies":[{"name":"a","caps":[{"category":"fixture_hum","cap":"none","substitution":"omit"}]}]})",
       "not a category"},
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
