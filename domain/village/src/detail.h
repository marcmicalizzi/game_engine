#pragma once

// What the village's sources share and nothing outside the module includes.
#include <domain/dwellings/furnish.h>
#include <domain/village/village.h>

namespace engine::village::detail {

// **The entry's kit as the furnisher reads it** (village.md, "A kit"): a row a kit piece that
// stands in for one of the furnisher's own (`KitPiece::replaces`), its id its row in the entry's
// kit, its foot and height in whole centimetres, rounded up so the mesh fits the box the furnisher
// keeps clear for it.
struct HouseKit {
  const VillageLayer* layer = nullptr;
  Vector<dwellings::KitRow> rows;
};
void house_kit(const VillageLayer& layer, HouseKit& out);

// **The house filler the generator plans with** (village.md, "The house seam"): the structures
// capability's, reached here and nowhere else in the module, so the plan itself knows no building.
// `context` is what the function is called with: `kit`, which must outlive the plan's making.
HouseFn house_filler(const HouseKit& kit, void*& context) noexcept;

}  // namespace engine::village::detail
