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
#include <core/json/json_value.h>

#include <array>
#include <span>
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
// What `agent-default` puts in place of content above a cap, per engine category, as the owner
// decided on 2026-11-18 (ADR-0073, question 2, "Box, as proposed"): a box at the bounds for
// nudity and sexual, so an agent's picture keeps its framing, and nothing for violence.
inline constexpr std::array<Substitution, k_engine_categories> k_agent_default_substitutions = {
    Substitution::proxy, Substitution::proxy, Substitution::omit};
// A category `agent-default` does not name (a game's own) is capped at `none`, as ADR-0033
// decision 7 says, and omitted: the stricter answer for a category nobody has judged.
inline constexpr Substitution k_agent_default_game_substitution = Substitution::omit;
ViewPolicy agent_default();

// True when narrowing `policy` by `base` changes it: some category capped higher than `base`
// caps it, or `proxy` where `base` says `omit`. `wider_than(p, agent_default())` is what a host
// started under `p` says on stderr and in its start line (docs/content-classes.md, "Every output
// says what it was made under"). The parameter is not named `floor`, which names a C function.
bool wider_than(const ViewPolicy& policy, const ViewPolicy& base);

// One distinct class a scene met, and what it decided.
struct DecisionEntry {
  ContentClasses classes;
  Decision decision;

  bool operator==(const DecisionEntry&) const = default;
};

// The table a loaded scene keeps, `SceneData::decisions` in docs/content-classes.md: every
// distinct class it decided under its one policy, with the outcome. Append-only:
// an entry stays while the scene is loaded, so its length is its generation and a check can
// resume where the last one stopped. It starts with the all-`none` class admitted, so a scene
// with nothing classified holds one entry, and deciding an unclassified instance through it is
// one branch and no allocation. Cold data: one entry per distinct class, a handful a scene.
class DecisionTable {
 public:
  explicit DecisionTable(const ViewPolicy& under);

  // `decide(classes, policy())`, recorded the first time the class is met.
  Decision decide(const ContentClasses& classes);

  const ViewPolicy& policy() const { return policy_; }
  // The generation: the number of entries, the first one the all-`none` class.
  u32 size() const { return static_cast<u32>(entries_.size()); }
  const DecisionEntry& at(u32 i) const { return entries_[i]; }

 private:
  ViewPolicy policy_;
  Vector<DecisionEntry> entries_;
};

// True when `policy` decides every entry from `from` on with the outcome the table recorded: it
// admits what was admitted, proxies what was proxied and omits what was omitted. Within the cap
// is not enough: a caller whose policy omits would receive the box, and one whose policy is
// wider a picture missing what it may see. A capture is drawn for a caller only when this holds
// for the caller's policy, from 0 before the first frame and again, from that check's
// generation, after the last frame is recorded.
bool same_decisions(const DecisionTable& table, const ViewPolicy& policy, u32 from = 0);

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

  // A name a category may have: lower-case letters, digits and `_`, and not `levels` or
  // `materials`, which a sidecar's two forms reserve. So `Nudity` and `nudity ` are refused rather
  // than read as a second category beside `nudity`.
  static bool valid_name(std::string_view name);
  // Learns a category and its ladder from the data that carries them (a container's or a
  // texture's classes section), so a host needs no categories file. The same ladder again is
  // accepted; another ladder for a known name (an engine category's included), a seventeenth
  // category, or a name or ladder a categories file would refuse is refused, and the data's
  // class is then unreadable. `index` is the category's place in the table.
  bool learn(std::string_view name, const Vector<std::string>& ladder, u32& index,
             std::string& error);

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

// The class of something whose class cannot be read: every category the table knows at its
// ladder's top. Only a policy that caps every one of them at its top admits it
// (docs/content-classes.md, "Where a class is declared, and by whom").
ContentClasses unreadable(const Categories& categories);

// One material's own classes, as an asset declared them, for the part-wise substitution the
// character work builds (C11, C12).
struct MaterialClass {
  std::string name;
  ContentClasses classes;
};

// Reads what an asset declares (a sidecar's `content_classes`): the whole asset's levels, and
// every material's, each validated alike and joined into `out`, because v0 decides per mesh and
// a class declared only on a material is the asset's too. A category neither table defines is
// refused (a level that cannot be ordered cannot be capped); a level its ladder does not have is
// read as the ladder's highest, so an unknown level fails closed. `error` names the category and
// where it was. `materials`, when given, receives each material's own classes, by name.
// **On a refusal `out` is `unreadable(categories)`** and `materials` is emptied, so a caller that
// ignores the answer still fails closed: a class that cannot be read is never `none`.
bool read_classes(const Categories& categories, const content::ContentClasses& declared,
                  ContentClasses& out, std::string& error,
                  Vector<MaterialClass>* materials = nullptr);

// Reads a provenance sidecar's text (`<stem>.provenance.json`) and the classes it declares: the
// one reader the content build, the load and the store call, so that a sidecar cannot mean one
// thing to one and another to the next. Absent `content_classes` is `none` everywhere. Refused,
// with `out` the unreadable class: text that is not JSON, a key given twice in one object, a
// `content_classes` of the wrong shape or type, and whatever `read_classes` refuses.
bool read_sidecar_classes(const Categories& categories, std::string_view sidecar_json,
                          ContentClasses& out, std::string& error,
                          Vector<MaterialClass>* materials = nullptr);

// A policy's cap on a category that is not the engine's, kept by name as the file gave it: a
// host learns a game's category from the data that carries it, so the category may not be in
// the table when the file is read (docs/content-classes.md, "Where a policy lives, and what keeps
// it from a client").
struct NamedCap {
  std::string category;
  std::string cap;
  Substitution substitution = Substitution::omit;
};

// The hash of a class's canonical form, which a cache key mixes in (docs/content-classes.md,
// "How it travels through derived data"). Names, never indexes, so a game that reorders its
// categories keeps the hash and one that renames a level or reorders a ladder changes it: every
// category above `none` in the class or in a material, sorted by name, with its whole ladder
// lowest first; then the class's (category, level) names sorted by category; then every
// material whose class is not `none`, sorted by name, with its own pairs. **Zero when nothing is
// classified** (the class and every material all `none`), so an unclassified source's key does
// not move; never zero otherwise. The same bytes on every toolchain (`core/hash`'s
// `hash_bytes` over that form).
u64 class_hash(const Categories& categories, const ContentClasses& classes,
               const Vector<MaterialClass>* materials = nullptr);

// ---- carried through derived data (docs/content-classes.md, "How it travels through derived
// data"; C62) ----------------------------------------------------------------------------------
//
// What a built thing records about its class, and how its cache key holds it. The containers
// (`.clusters` section 54, `.tex` section 6) keep the bytes `encode_classes` makes and know
// nothing about them, so the form is this module's alone, as the reader is.

// Where a source's sidecar is: `<stem>.provenance.json` beside it, the stem being the file name
// without its last extension (`cube.glb` to `cube.provenance.json`), which is the name
// `tools/generate.ps1` and `tools/blender-run.ps1` write (docs/content-generation.md).
std::string sidecar_path_of(std::string_view source);

// A cache key that holds a class: `key` itself when `class_hash` is zero, so an unclassified
// source keeps the key, and every existing entry, golden and pin, it had; otherwise a key of its
// own over both, never equal to `key`. Both cache keys are moved by this one function.
u64 classified_key(u64 key, u64 class_hash);

// The record a classified thing carries: every category above `none` in the class or a material,
// sorted by name, each with its whole ladder by name, lowest first; the class's (category, level)
// pairs by name; and every classified material's own pairs, by material name. Length-prefixed
// text, so it is the same bytes on every toolchain. **Empty when nothing is classified**, which
// is what tells a writer not to write the section, so an unclassified container or texture is
// the bytes it was before the section existed.
void encode_classes(const Categories& categories, const ContentClasses& classes,
                    const Vector<MaterialClass>* materials, Vector<u8>& out);

// Reads `encode_classes`'s bytes back, learning each category's ladder into `categories` as it
// goes (`Categories::learn`), so a host needs no categories file. Empty bytes are `none`
// everywhere. Refused, with `out` the unreadable class (after whatever was learned) and the
// materials emptied: bytes that are not the record, a ladder the table holds otherwise for the
// name, a pair naming a category the record did not list or a level its ladder lacks, a category
// given twice in one scope (the asset's or a material's), and anything after the end.
bool decode_classes(Categories& categories, std::span<const u8> bytes, ContentClasses& out,
                    std::string& error, Vector<MaterialClass>* materials = nullptr);

// The record a node built from several inputs carries (a merged mesh, a tile set over its forms,
// an impostor over the meshes it was baked from): the join of every input's class, as a whole
// asset. Materials are not carried across, since two inputs' materials of one name are two
// materials. An input that cannot be decoded makes the output the unreadable class, written, and
// the answer false, so a caller that ignores it still carries a class that fails closed.
bool join_encoded(Categories& categories, std::span<const std::span<const u8>> inputs,
                  Vector<u8>& out, std::string& error);

// A class as data, for `engine-content info` and `content.build`'s result: `categories` (each
// named one with its `levels`, lowest first), `levels` (category to level) and `materials`
// (material to its own map), by name. Numbers and names only, which ADR-0033 decision 6 allows.
JsonValue classes_json(const Categories& categories, const ContentClasses& classes,
                       const Vector<MaterialClass>* materials = nullptr);

// The operator's policies, with `agent-default` compiled in. Cold data, built at start.
struct NamedPolicy {
  std::string name;
  // Resolved against the table it was last resolved with: an engine category's cap always, a
  // game category's once the table holds it, every other category `none` with `omit`.
  ViewPolicy policy;
  // Every cap the file gave a category that is not the engine's, by name.
  Vector<NamedCap> by_name;
};

// `named.policy` with every by-name cap whose category the table now holds applied: its cap's
// level, or `none` with `omit` when the ladder does not have that level, so a misspelt level
// narrows and never widens. Every other category past the engine's three is `none` with `omit`,
// whatever an earlier resolution set. Pure; the caller resolves again after a ladder arrives
// (`Categories::learn`), before it decides a class that names the new category.
ViewPolicy resolve(const NamedPolicy& named, const Categories& categories);

class ViewPolicies {
 public:
  // `agent-default` alone, which is what a host with no policies file has.
  ViewPolicies();

  // Reads a policies file, refused whole (the set unchanged) with the reason: a policy with no
  // name, a name given twice or `agent-default` (compiled in, never redefined, wider or not), a
  // category name outside `Categories::valid_name` or named twice in one policy, an engine
  // category's cap its ladder does not have, a substitution that is not one of the four ADR-0033
  // names, and `cover` or `obscure at capture`, which are not built yet. A category that is not
  // the engine's is capped by name (`NamedCap`), whether the table knows it yet or not, and
  // resolved against `categories` before the read returns.
  bool read(const Categories& categories, const content::ViewPoliciesFile& file,
            std::string& error);
  // The same from the file's JSON text (unknown fields refused).
  bool read_json(const Categories& categories, std::string_view json, std::string& error);

  // Resolves every policy's by-name caps against `categories` again, after the table learned a
  // ladder from data. Idempotent.
  void resolve(const Categories& categories);

  // The policy by name, as last resolved, or null.
  const ViewPolicy* find(std::string_view name) const;
  u32 size() const { return static_cast<u32>(policies_.size()); }
  const NamedPolicy& at(u32 i) const { return policies_[i]; }

 private:
  Vector<NamedPolicy> policies_;
};

}  // namespace engine::content_class
