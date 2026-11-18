# content_class

**Layer:** foundation. **Not a capability**: no `ENGINE_WITH_` switch and in every configuration, `ENGINE_MINIMAL` included, because a build switch is what [ADR-0033](../adr/0033-content-classes-and-view-policies.md) forbids as the mechanism of a view policy ([ADR-0073](../adr/0073-classes-travel-with-derived-data-and-policies-are-the-operators.md) decision 9). Its dependencies are `base`, `containers`, `json`, `schema` and `schemas`, none of them a capability. Roadmap C61; the design and the rows that build on it are [content classes](../content-classes.md).

## Purpose

The vocabulary of content classes and view policies, and **the one decision every output calls**: given what a mesh is classified as and the policy of the view it is being instantiated for, admit it, omit it, or put a proxy in its place. Nothing calls it yet. The content build (C62), the record and the store (C63), `load_scene` (C64), the hosts (C65) and the refusals (C66) are the callers, and each calls this function rather than a test of its own, so that a policy means the same thing at every one of the twenty-four exits.

## Owned data

- **`ContentClasses`**: a level per category, sixteen bytes, one byte per category indexed by the category's place in a `Categories` table. All zeros is `none` everywhere, which is what an asset that declares nothing has. It is dense rather than a list of (category, level) pairs because a join of two dense values cannot overflow and a decision is a loop over a fixed size; the price is the limit below.
- **`ViewPolicy`**: a cap and a `Substitution` per category, thirty-two bytes. Zero-initialized it is every category capped at `none` with `omit`, which is what ADR-0033 decision 2 says a category a policy does not mention gets, so a policy read from a file starts there and only the categories it names are set.
- **`Decision`**: the outcome (`admit`, `omit`, `proxy`) and the category, level and cap that decided it, four bytes. A refusal later reports those three; it never needs anything about the content itself.
- **`Categories`**: the engine's three ladders compiled in, `nudity` (`none`, `partial`, `full`), `sexual` (`none`, `suggestive`, `explicit`), `violence` (`none`, `mild`, `graphic`), at indexes 0, 1, 2, then a game's categories from its `engine.content.ContentCategoriesFile`. Names live only here; a value holds indexes.
- **`ViewPolicies`**: the operator's named policies from an `engine.content.ViewPoliciesFile`, with `agent-default` compiled in first.

The file shapes are schema types in `schemas/provenance.schema` (`ContentClasses` and `MaterialContentClasses` for a sidecar's `content_classes`, `ContentCategory` and `ContentCategoriesFile`, `ViewPolicyCap`, `ViewPolicy` and `ViewPoliciesFile`). None of them is a document record (`@kind(record)`), so no protocol method can make one in a document.

**Limits, stated**: at most `k_max_categories` (16) categories in all, the engine's three included, and at most 255 levels to a ladder. A categories file that would pass either is refused with the number in the reason.

## Invariants

- **`decide` is pure, total and allocation-free.** Within every cap it admits. Otherwise it omits if any category above its cap is substituted by `omit`, and puts a proxy in place only when every category above its cap says `proxy`: `omit` is the stricter, and wins whatever the order. A level past a ladder's top is above every cap.
- **An all-`none` class is one branch.** `decide` tests the sixteen bytes first and returns before it looks at the policy.
- **`join` is the per-category maximum**, so a thing built from several inputs is at least as classified as each.
- **`narrow` never widens.** Per category it takes the lower cap and the stricter substitution, so a narrowed policy admits nothing either input refused and omits whatever either would omit. A per-call narrowing (C65) is this function with the caller's effective policy.
- **A category neither table defines is refused** when a sidecar's classes are read (`read_classes`), because a level that cannot be ordered cannot be capped. **A level a ladder does not have is read as its highest**: an unknown level fails closed. **As built, `read_classes` reads the whole asset's `levels` alone and a refusal leaves its output untouched**, and every reader keeps the last of two duplicate keys: three faults, below, that its next leg mends.
- **A game's ladder starts at `none`**, because absent means `none` (ADR-0033 decision 1). A game cannot redefine an engine category: its name is already taken, and the file is refused.
- **A policies file that is wrong is refused whole**, the set unchanged, with the reason: a policy with no name, a name given twice, a category neither table defines or capped twice in one policy, a cap its ladder does not have, a substitution that is not one of ADR-0033's four names, and `cover` or `obscure at capture`, which are not built until the character work (C11, C12) builds slots and section masks ([content classes](../content-classes.md#what-decided-at-instantiation-means)). Unknown fields are refused too.
- **`agent-default` is never redefined by a file**, wider or narrower. The design compiles it in as the policy every host runs unless its operator says otherwise, and as the one a credentialed connection falls back to rather than the host's own ([content classes](../content-classes.md#where-a-policy-lives-and-what-keeps-it-from-a-client)); a file that could redefine it would move that fail-closed floor silently, so the name is reserved and a file naming it is refused as a duplicate. The design does not address the case in words; this is the reading that keeps its fallback where it says it is. An operator who wants more for an agent names a policy of their own.

## `agent-default`, and what waits on the owner

Its caps are ADR-0033 decision 7's: `nudity: none`, `sexual: none`, `violence: mild`, every game category `none`. **What it puts in place of content above a cap is not decided** (ADR-0073, question 2: the design recommends `proxy` for `nudity` and `sexual` and `omit` for `violence`). Until the owner answers, `k_agent_default_substitution` is `Substitution::omit` for every category, the stricter of the two answers, and the test "agent-default is ADR-0033 decision 7's caps, and omit until the owner says" pins both the constant and the policy. The owner's answer changes that constant (or, for the recommended mix, the policy's two substitutions) and the test in the same commit.

## What its next leg mends

A review of the module and of the design found three faults in the readers built here, and the design's second pass ([content classes](../content-classes.md), revised 2026-11-18) needs vocabulary the module does not have yet. Both are C61's next leg, CPU only, in this order. If the leg runs long, the second list starts the content build's row (C62) instead, because C62 is the first caller of each item.

**The three faults, each with the test that fails today:**

1. **A class declared per material is never read.** `read_classes` loops over `declared.levels` and ignores `materials`, so `{"materials": {"m0": {"levels": {"fixture_glow": "bright"}}}}` reads as `none` and `decide` admits it. The mend validates every material's levels exactly like the asset's, joins them into the asset's class (v0 decides per mesh), and gives the per-material values as a second output for C11 and C12. Tests: a class only under `materials` decides `omit` under `agent-default`; an unknown category under `materials` is refused; a material's level unknown to its ladder reads as the top.
2. **A refused read leaves its output untouched**, and the test "untouched" pins it, so a caller that starts from `ContentClasses{}` and ignores the answer admits the asset. The mend writes **the unreadable class** into the output on every refusal: every category the table knows at its ladder's top (`content_class::unreadable(categories)`, also what the load uses for a sidecar it cannot read). Tests: the "untouched" pin becomes "unreadable"; a refused read decides `omit` under `agent-default`.
3. **A duplicate key keeps the last value.** The JSON parser's `insert_or_assign` resolves `{"levels": {"fixture_glow": "bright", "fixture_glow": "none"}}` to `none`, and a policy's `"cap"` given twice to its second. The mend gives `core/json` a strict mode that refuses a duplicate key, naming it (its default unchanged, so no other reader moves), and every reader in this module uses it. Tests: a sidecar, a categories file and a policies file, each with a key given twice, refused naming the key; `core/json`'s own tests keep the default and test the mode.

**The vocabulary the second pass needs:**

- **One sidecar reader of both forms**, `read_sidecar_classes`: the schema's form (`levels`, `materials`) and the generation tools' flat map (category to level, read as `levels`); a mix of the two refused; `levels` and `materials` reserved as category names. Tests: each form read; the tools' flat map at `none` reads as `none`; a mix refused; a category named `levels` refused.
- **Category names are `[a-z0-9_]`**, in a categories file and in data. Tests: `Nudity`, `nudity ` and a name with a tab refused.
- **Ladders that arrive with data.** A host's table learns a game category and its ladder from the data that names it; the same ladder again is accepted; another ladder for a known name, or another ladder for an engine category, makes that data's class unreadable; a seventeenth category is unreadable. Tests: one each.
- **A policy caps by name.** `ViewPolicies::read` no longer refuses a category it does not know: an engine category's cap is still checked against its ladder (an unknown cap refuses the file), and a game category's cap is kept by name and resolved when its ladder arrives, `none` when the ladder lacks the level. Tests: a policy capping `fixture_glow` read with no categories file, resolved when the ladder arrives; a cap naming a level the ladder lacks resolves to `none`; an engine category's unknown cap still refuses the file.
- **The canonical class hash** the cache keys mix in: sorted (category name, level name) pairs with each game category's ladder, and per material. Tests: reordering categories leaves it; renaming a level or reordering a ladder changes it; the all-`none` class has no hash (the key is unchanged).
- **The decision table** a loaded scene keeps: distinct class to outcome, append-only, its length its generation; and `same_decisions(table, policy, from)`, true when the policy decides every entry from `from` on as the table did. Tests: true under the table's own policy; false under one that omits where the table proxied; false under a wider one; the second check over new entries only.
- **`wider_than(policy, agent_default())`**: true when narrowing by `agent-default` changes the policy. Tests: `agent-default` is not wider than itself; a higher cap is wider; `proxy` where `agent-default` omits is wider.

**Cases the tests do not cover yet**, added in the same leg: exactly sixteen categories accepted and a second `add` repeating the first refused; a 255-level ladder accepted and a 256-level one refused; a number for a level and an object for a cap refused in each file; `decide(c, p).outcome == admit` exactly when `within(c, p)`, over random classes and policies with levels up to 255; `narrow` of a policy with every category at its top.

## Public API

`foundation/content_class/content_class.h`: `ContentClasses`, `is_none`, `join`; `Substitution`, `ViewPolicy`, `within`, `narrow`; `Outcome`, `Decision`, `decide`; `k_agent_default_name`, `k_agent_default_substitution`, `agent_default`; `Categories` (`add`, `add_json`, `find`, `level`, `top`); `read_classes`; `NamedPolicy`, `ViewPolicies` (`read`, `read_json`, `find`).

Nothing in the tree reads a categories file or a policies file at run time yet, no host has a flag for one, and no policies file is committed: the tests write theirs as strings.

## What it costs

Where nothing is classified, which is every asset this repository has: sixteen zero bytes a mesh and one test. The bench `engine_content_class_bench` has two rows over 4096 meshes against `agent-default`: `content_class.decide.none` and `content_class.decide.classified`. In `msvc-debug` on 2026-11-18 they measured 262 us and 403 us a pass (about 64 ns and 98 ns a mesh), with other processes using 54% of the CPU and another row holding the GPU lock, so both are upper bounds from an unoptimized build; the test "deciding where nothing is classified costs a branch" prints the same per-mesh figure. A release measurement on a quiet machine belongs with C64's load-time measurement, where the decision's share of a scene's load is the number that matters.

## How to test

`tools/dev.ps1 test -Filter content_class`. The tests open nothing and need no device. Every fixture is neutral: a made-up game category `fixture_glow` (`none`, `dim`, `bright`) and the engine's three categories as names only. They cover the engine's ladders; `agent-default`'s values; `decide`'s table of cases (within the cap, above it under `omit` and under `proxy`, `omit` over `proxy`, a game's category beside the engine's, a level past a ladder's top, a category the policy does not name); `read_classes` refusing an unknown category and reading an unknown level as the top; `join`; a property test that narrowing never widens, over 2000 random pairs of policies; each malformation of a categories file and of a policies file, each refused whole with its reason; and the timed all-`none` decision.
