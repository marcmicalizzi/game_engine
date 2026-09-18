// Size table for domain/physics (ADR-0019). The pinned types are the ones that exist per
// contact, per cage element, or per body handle, so their footprint is the module's footprint.
#include <core/base/size_table.h>
#include <domain/physics/physics.h>

using namespace engine;

// A handle is a SlotMap handle and nothing else: passing one is passing eight bytes.
ENGINE_EXPECT_SIZE(8, 4, physics::BodyId);
ENGINE_EXPECT_SIZE(8, 4, physics::ShapeId);
ENGINE_EXPECT_SIZE(8, 4, physics::SoftBodyId);

// One per reported contact per step. Sixty-four bytes is one cache line, which is the reason
// the user data is carried inline instead of being looked up per event.
ENGINE_EXPECT_SIZE(64, 8, physics::ContactEvent);

// Query results are returned by value into a caller's stack slot.
ENGINE_EXPECT_SIZE(36, 4, physics::RayHit);
ENGINE_EXPECT_SIZE(40, 4, physics::ShapeHit);

// Cage elements: ADR-0026 budgets about 32 bytes per element of simulation state, and these
// are the description that feeds it.
ENGINE_EXPECT_SIZE(12, 4, physics::SoftEdge);
ENGINE_EXPECT_SIZE(20, 4, physics::SoftVolumeConstraint);
ENGINE_EXPECT_SIZE(24, 4, physics::SoftAttachment);
