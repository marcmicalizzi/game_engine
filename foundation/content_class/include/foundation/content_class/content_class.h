// Content classes and view policies: the vocabulary, and the one decision every output calls
// (ADR-0033, ADR-0073; docs/content-classes.md, docs/subsystems/content_class.md).
//
// A class is a level per category on content; a policy is a cap and a substitution per category
// on an output. Both are dense arrays indexed by category, so a join, a narrowing and a decision
// are a loop over a fixed number of bytes, and the all-`none` class an unclassified asset has is
// all zeros. Names are read once, when a sidecar, a categories file or a policies file is read,
// against a `Categories` table: the engine's three ladders, then a game's.
#pragma once

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <array>
#include <string>
#include <string_view>

namespace engine::content {
struct ContentCategoriesFile;
struct ContentClasses;
struct ViewPoliciesFile;
}  // namespace engine::content

namespace engine::content_class {

// The engine's categories and the game's share one index space of this many. A categories file
// that would pass it is refused, with this number in the reason: it is the size of every class
// and policy value, chosen so that a class is 16 bytes.
inline constexpr u32 k_max_categories = 16;
// The longest ladder a category may have, `none` included: a level is a byte.
inline constexpr u32 k_max_levels = 255;

// The engine's three, compiled in, in this order (ADR-0033 decision 1). A game's follow them.
inline constexpr u8 k_nudity = 0;
inline constexpr u8 k_sexual = 1;
inline constexpr u8 k_violence = 2;
inline constexpr u32 k_engine_categories = 3;

// Level 0 of every ladder is `none`.
inline constexpr u8 k_none = 0;

// A level per category, indexed by the category's place in a `Categories` table. All zeros is
// `none` everywhere, which is what an asset that declares nothing has.
struct ContentClasses {
  std::array<u8, k_max_categories> levels{};

  bool operator==(const ContentClasses&) const = default;
};

// True when every category is `none`: the one test an unclassified asset costs.
inline bool is_none(const ContentClasses& c) {
  u8 any = 0;
  for (u32 i = 0; i < k_max_categories; ++i)
    any |= c.levels[i];
  return any == 0;
}

// The per-category maximum: what a thing built from both inputs carries.
ContentClasses join(const ContentClasses& a, const ContentClasses& b);

// What stands in for content above a cap. Only the two the engine builds; `cover` and
// `obscure at capture` are names a policies file is refused for until the character work builds
// them (ADR-0073 decision 5). `omit` is zero so that a policy's untouched category is `omit`.
enum class Substitution : u8 { omit = 0, proxy = 1 };

// A cap and a substitution per category. Zero-initialized it is the narrowest policy there is:
// every category capped at `none` with `omit`, which is what ADR-0033 decision 2 says a category
// a policy does not mention gets.
struct ViewPolicy {
  std::array<u8, k_max_categories> caps{};
  std::array<Substitution, k_max_categories> substitutions{};

  bool operator==(const ViewPolicy&) const = default;
};

// True when every category's level is at or under the policy's cap.
bool within(const ContentClasses& classes, const ViewPolicy& policy);

// The policy a call may narrow its effective one to: per category the lower cap, and the
// stricter substitution (`omit` over `proxy`). Never wider than either input, whatever they are.
ViewPolicy narrow(const ViewPolicy& a, const ViewPolicy& b);

enum class Outcome : u8 { admit = 0, omit = 1, proxy = 2 };

// What to do with one mesh's instances under one policy, and why. `category`, `level` and
// `cap` name the category that decided (the lowest-indexed one whose substitution is the
// outcome's) and are zero for `admit`.
struct Decision {
  Outcome outcome = Outcome::admit;
  u8 category = 0;
  u8 level = 0;
  u8 cap = 0;

  bool operator==(const Decision&) const = default;
};

// The one decision function, which every output calls per mesh at load (docs/content-classes.md):
// within every cap, `admit`; otherwise `omit` if any category above its cap is substituted by
// `omit`, else `proxy`. Pure, total, and allocation-free; an all-`none` class is one branch.
Decision decide(const ContentClasses& classes, const ViewPolicy& policy);

// The policy every host runs that was not told otherwise (ADR-0033 decision 7): `nudity: none`,
// `sexual: none`, `violence: mild`, every game category `none`.
inline constexpr std::string_view k_agent_default_name = "agent-default";
// What `agent-default` puts in place of content above a cap. The owner has not decided this
// (ADR-0073, question 2: the design recommends `proxy` for nudity and sexual, `omit` for
// violence); until they do, the stricter answer, `omit`, for every category.
inline constexpr Substitution k_agent_default_substitution = Substitution::omit;
ViewPolicy agent_default();

// The categories a build or a host knows: the engine's three, then a game's. Names are read
// against it once; a value holds indexes. Cold data, built at start.
class Categories {
 public:
  // The engine's three ladders and nothing else.
  Categories();

  // Adds a game's categories. Refused whole, the table unchanged, with the reason: a name
  // empty or already known (the engine's included), a ladder empty, not starting at `none`,
  // naming a level twice or longer than `k_max_levels`, or more categories than
  // `k_max_categories` in all.
  bool add(const content::ContentCategoriesFile& file, std::string& error);
  // The same from a categories file's JSON text (unknown fields refused).
  bool add_json(std::string_view json, std::string& error);

  u32 size() const { return static_cast<u32>(names_.size()); }
  // The category's index, or -1 when neither the engine nor the game defines it.
  i32 find(std::string_view name) const;
  const std::string& name(u32 category) const { return names_[category]; }
  // The ladder's highest level.
  u8 top(u32 category) const { return static_cast<u8>(ladders_[category].size() - 1); }
  // The level's index in the category's ladder, or -1 when the ladder does not have it.
  i32 level(u32 category, std::string_view name) const;
  const std::string& level_name(u32 category, u8 level) const { return ladders_[category][level]; }

 private:
  Vector<std::string> names_;
  Vector<Vector<std::string>> ladders_;
};

// Reads what an asset declares (a sidecar's `content_classes`, the whole asset's levels). A
// category neither table defines is refused (a level that cannot be ordered cannot be capped);
// a level its ladder does not have is read as the ladder's highest, so an unknown level fails
// closed. `error` names the category.
bool read_classes(const Categories& categories, const content::ContentClasses& declared,
                  ContentClasses& out, std::string& error);

// The operator's policies, with `agent-default` compiled in. Cold data, built at start.
struct NamedPolicy {
  std::string name;
  ViewPolicy policy;
};

class ViewPolicies {
 public:
  // `agent-default` alone, which is what a host with no policies file has.
  ViewPolicies();

  // Reads a policies file, refused whole (the set unchanged) with the reason: a policy with no
  // name, a name given twice or `agent-default` (compiled in, never redefined, wider or not),
  // a category neither table defines or named twice in one policy, a cap its ladder does not
  // have, a substitution that is not one of the four ADR-0033 names, and `cover` or
  // `obscure at capture`, which are not built yet.
  bool read(const Categories& categories, const content::ViewPoliciesFile& file,
            std::string& error);
  // The same from the file's JSON text (unknown fields refused).
  bool read_json(const Categories& categories, std::string_view json, std::string& error);

  // The policy by name, or null.
  const ViewPolicy* find(std::string_view name) const;
  u32 size() const { return static_cast<u32>(policies_.size()); }
  const NamedPolicy& at(u32 i) const { return policies_[i]; }

 private:
  Vector<NamedPolicy> policies_;
};

}  // namespace engine::content_class
