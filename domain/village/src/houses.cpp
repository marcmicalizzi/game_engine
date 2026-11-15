// The house filler the generator plans with (docs/subsystems/village.md, "The house seam"): the one
// file of the module that knows what a house is made by.
#include "detail.h"

namespace engine::village {

namespace detail {

HouseFn house_filler(void*& context) noexcept {
  context = nullptr;
  return &box_house;
}

}  // namespace detail

bool plan_scene_village(const VillageLayer& layer, const scene_gen::Context& context,
                        VillagePlan& out, std::string& error) {
  void* house_context = nullptr;
  const HouseFn house = detail::house_filler(house_context);
  return plan_village(layer, context.uncut(), context, house, house_context, out, error);
}

}  // namespace engine::village
