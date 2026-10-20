#pragma once

// Material sets and material packs (docs/subsystems/materials.md): the one place a generated
// mesh's tiled PBR set is described, checked, looked up by role in a pack, and written into the
// container the mesh is built into. The tree solver (domain/flora) and the rock solver
// (domain/rocks) both link it, so a bark and a granite reach the renderer by the same code.
//
// **A set** is five images — base colour, a tangent-space normal map (OpenGL, +Y), roughness,
// metallic and height, each a greyscale or colour PNG — and the surface its one repeat covers,
// `tile_m`. A recipe names one per part in its material block, with paths relative to the recipe;
// a **pack** holds the defaults, one set per **role** (`palm/bark`, `rock/granite`), in
// `content/materials/` beside the repository's derived-data cache, with absolute paths once read.
// A part takes the recipe's set when the recipe names an image for it, else the pack's set for its
// role, else none, and draws the flat colour it always drew.
//
// **What the container records.** A set's base colour goes in the base-colour slot with a white
// factor; its roughness, alone, in the metallic-roughness slot — glTF reads roughness from G, and a
// greyscale image decodes to G = the grey — with a roughness factor of 1 and a **metallic factor of
// 0**, so the B channel the grey also fills reads as no metal: bark, leaves and stone are not
// metal, and packing a metallic channel at build time would need image io the solvers do not have.
// The normal map goes in the normal slot with the set's `normal_scale`. Metallic and height are
// carried in the pack and the recipe and read by nothing yet. For a part whose UVs are in metres,
// every sampled slot gets a texture transform of scale `1 / tile_m`; a part whose UVs are already
// in repeats, or are a whole card per image, gets none.
//
// No io except `pack_at`, which reads a pack's `pack.json` once per process; everything else is a
// function of its arguments, so a solver stays one.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_file.h>

#include <string>
#include <string_view>

namespace engine::materials {

// The format word a `pack.json` carries; a pack with another is refused.
inline constexpr const char* k_pack_format = "engine.materials.pack/1";

// The range a set's repeat and normal strength must be in (the schemas say the same).
inline constexpr f32 k_tile_min_m = 0.05f;
inline constexpr f32 k_tile_max_m = 20.0f;
inline constexpr f32 k_normal_scale_max = 4.0f;

// One set: the five image paths (empty for an image the set does not have) and its repeat. A
// recipe's paths are relative to the recipe; a pack's are absolute.
struct MaterialSet {
  std::string base_color;
  std::string normal;
  std::string roughness;
  std::string metallic;
  std::string height;
  f32 tile_m = 1.0f;
  f32 normal_scale = 1.0f;
};

// True when the set names any image — the test of whether a recipe's block for a part replaces the
// pack's default for it.
bool names_images(const MaterialSet& set) noexcept;

// The set's numbers in range: `tile_m` 0.05 to 20 m, `normal_scale` 0 to 4. False with a sentence
// naming `field` (the recipe's spelling of the block) otherwise. A NaN fails.
bool set_in_range(const MaterialSet& set, std::string_view field, std::string& error);

// How a part's UVs are laid out, which decides its sampling transform.
enum class UvLayout : u8 {
  metres,   // u and v in metres of surface: the set's repeat is a transform of 1 / tile_m
  repeats,  // already in whole repeats of the set (a rock's charts): no transform
  card,     // one image per card, whole (a leaf, a needle spray): no transform
};

// Writes `set` into material `material` of `data`: the images appended to `image_paths` (an image
// already there is shared, not repeated) with an empty `images` entry beside each, the slots, the
// factors above and, for `UvLayout::metres`, the transform. `data.materials` and
// `data.material_sampling` must hold `material`. A set that names no image changes nothing.
void apply_set(const MaterialSet& set, UvLayout layout, geometry::ClusterFileData& data,
               u32 material);

// ---- packs
// ---------------------------------------------------------------------------------------

struct PackRole {
  std::string role;  // "palm/bark"
  MaterialSet set;   // absolute paths
};

// A pack as read: where it is, its name and version, its roles sorted by name, and the hash a
// generated mesh's build key takes over it (`pack_key`).
struct Pack {
  std::string dir;
  std::string name;
  std::string version;
  Vector<PackRole> roles;
  u64 hash = 0;
};

// Parses a `pack.json`'s text for the pack in `dir`: `format`, `name`, `version` and `roles`, each
// role's `maps` (`base_color`, `normal`, `roughness`, `metallic`, `height`, each `{"file": ...}`
// relative to `dir`), `tile_m`, `normal_scale` (1 when absent) and `normal_convention`, which must
// be "opengl". A role that is malformed refuses the whole pack: a pack is fetched and verified as a
// unit. `hash` is over the text and `dir`, so a pack moved or changed is a new key. Pure.
bool parse_pack(std::string_view text, std::string_view dir, Pack& out, std::string& error);

// The pack in `dir`, read once per process and kept: null for an empty `dir`, a directory with no
// `pack.json`, or one that does not parse — the last two said once, in the log. Thread-safe.
const Pack* pack_at(std::string_view dir);

// The default pack directory for a host: `<repo>/content/materials` beside `<repo>/ddc`, the root
// `geometry::find_ddc_root(start)` finds; empty when there is no repository above `start`.
std::string default_pack_dir(std::string_view start);

// The set a pack has for `role`, or null (a null pack has none).
const MaterialSet* find_role(const Pack* pack, std::string_view role) noexcept;

// What a build key over a generated mesh takes for the pack it was built with: its hash, or 0 for
// no pack or a pack with no roles, which leaves the key what it was before packs existed.
u64 pack_key(const Pack* pack) noexcept;

// The set a part samples: the recipe's when it names an image, else the pack's for `role`, else
// null — and then one log line naming the role, once per role per process, so a scene of a hundred
// palms says once that there is no `palm/skirt` (no pack, or a pack without it). `from_pack` says
// which of the first two it was.
const MaterialSet* resolve_set(const MaterialSet* recipe, const Pack* pack, std::string_view role,
                               bool* from_pack = nullptr);

}  // namespace engine::materials
