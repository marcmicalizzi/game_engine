#pragma once

// What the village's sources share and nothing outside the module includes.
#include <domain/village/village.h>

namespace engine::village::detail {

// **The house filler the generator plans with** (village.md, "The house seam"): the structures
// capability's, reached here and nowhere else in the module, so the plan itself knows no building.
// `context` is what the function is called with.
HouseFn house_filler(void*& context) noexcept;

}  // namespace engine::village::detail
