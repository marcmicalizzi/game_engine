// Size table for systems/scene_collision (ADR-0019). Nothing here is per piece or per sample: a
// tile's pieces are the backend's compound and its heights plain floats. What a host hands the
// consumer every tick is the drawn ground's time, so that is what is pinned.
#include <core/base/size_table.h>
#include <systems/scene_collision/scene_collision.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 8, scene_collision::GroundTime);
// A door, kept per door and walked by `step_doors` every tick whether or not its tile is held: what
// a building's doors keep resident (scene_collision.md, "What props and doors keep resident").
// 88 since a door may come from a placed piece's declared leaf (S91): a 64-bit key from its place,
// the piece's turn (`base`) and where the leaf's middle stands through its thickness (`face_z`).
ENGINE_EXPECT_SIZE(88, 8, scene_collision::DoorBody);
