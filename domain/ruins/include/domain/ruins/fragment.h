#pragma once

// The assembler's output as a scene fragment: an `engine.scene.Scene` whose meshes are the kit's
// member files and whose instances are the pieces, each carrying its `RuinTag`, with the
// buildings in `ruin_sites` and the sand their walls ask for in `sand_drifts` (schemas/
// scene.schema). It is what `engine-content ruins` writes — the derived-node form for an authored
// place, whose footprint and seed are the document's parameters — and `engine-view --scene` reads
// it like any other scene. Every number in it comes from integer centimetres, the sixteen-step
// table and the height query, so a fragment written with flat ground is the same bytes from every
// toolchain.

#include <core/base/types.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>

#include <schemas/scene.h>
#include <string>
#include <string_view>

namespace engine::ruins {

// Fills `scene` from `out`. Mesh paths are written relative to `fragment_dir` when the kit's are
// on the same volume, absolute otherwise, so the fragment reads from where it is written.
void make_fragment(const Kit& kit, const Output& out, std::string_view fragment_dir,
                   std::string_view name, scene::Scene& scene);

// Writes the fragment as JSON to `path`.
bool write_fragment(const std::string& path, const Kit& kit, const Output& out,
                    std::string_view name, std::string* error);

// The same for buildings laid block by block (blocks.h): the block kit's meshes, one instance per
// block — standing or fallen — whose `RuinTag` carries its `block` (role, course, fallen,
// eroded), a standing block tagged as a section's stone and a fallen one as debris. `kit` is the
// section kit the footprint came from, for the wall's height.
void make_block_fragment(const Kit& kit, const BlockKit& blocks, const BlockOutput& out,
                         std::string_view fragment_dir, std::string_view name, scene::Scene& scene);
bool write_block_fragment(const std::string& path, const Kit& kit, const BlockKit& blocks,
                          const BlockOutput& out, std::string_view name, std::string* error);

}  // namespace engine::ruins
