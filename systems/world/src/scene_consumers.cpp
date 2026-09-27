// The scene's generator consumers in their declared order (scene_consumers.h).
#include <systems/world/scene_consumers.h>

namespace engine::world {

void add_scene_consumers(World& world, GroundTiles* ground, PlacementTiles* placements) {
  // Ground first: activation calls consumers in registration order, so a tile's ground is built
  // before the placements that stand on it, and deactivation, in reverse, drops them first.
  if (ground != nullptr && ground->valid()) world.add_consumer(ground->consumer());
  if (placements != nullptr) {
    placements->set_ring(&world.ring());
    world.add_consumer(placements->consumer());
  }
}

}  // namespace engine::world
