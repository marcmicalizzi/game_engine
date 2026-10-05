// Size table for domain/physics (ADR-0019). The pinned types are the ones that exist per
// contact, per cage element, or per body handle, so their footprint is the module's footprint.
#include <core/base/size_table.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>

using namespace engine;

// A handle is a SlotMap handle and nothing else: passing one is passing eight bytes.
ENGINE_EXPECT_SIZE(8, 4, physics::BodyId);
ENGINE_EXPECT_SIZE(8, 4, physics::ShapeId);
ENGINE_EXPECT_SIZE(8, 4, physics::SoftBodyId);

// One per reported contact per step. 80, not 64, since the contact's point became a `WorldPos`
// (ADR-0053): twelve more bytes for the position and four of alignment after the phase. The user
// data still rides inline, because a lookup per event costs more than the second line a buffer of
// them streams through; the event is drained once a step by the caller, not walked in a hot loop.
ENGINE_EXPECT_SIZE(80, 8, physics::ContactEvent);

// Query results are returned by value into a caller's stack slot. A hit's point is a `WorldPos`
// (ADR-0053), so both grew by its twelve bytes and their alignment is a double's.
ENGINE_EXPECT_SIZE(48, 8, physics::RayHit);
ENGINE_EXPECT_SIZE(56, 8, physics::ShapeHit);

// Where a body is: a `WorldPos` and a quaternion, no scale. Returned per body by the renderer's
// batch read (`World::read_transforms`).
ENGINE_EXPECT_SIZE(40, 8, physics::BodyTransform);

// Cage elements: ADR-0026 budgets about 32 bytes per element of simulation state, and these
// are the description that feeds it.
ENGINE_EXPECT_SIZE(12, 4, physics::SoftEdge);
ENGINE_EXPECT_SIZE(20, 4, physics::SoftVolumeConstraint);
// 32, not 24: an attachment gained a kind and a follow rate when the spring kind landed
// (plan 05 §5.14's "a stiff spring rather than a weld"). There is one per attached particle and
// they are walked once per step, so the extra eight bytes are a step's read bandwidth and not a
// per-element cost of the cage.
ENGINE_EXPECT_SIZE(32, 4, physics::SoftAttachment);

// A character between two steps: what a replay hashes and a host copies once a tick. 64, not 48,
// since the feet became a `WorldPos` (ADR-0053): twelve bytes of position and four more of the
// named padding before the tick.
ENGINE_EXPECT_SIZE(64, 8, physics::CharacterState);
