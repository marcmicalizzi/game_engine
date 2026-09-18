// Size table for systems/animation (ADR-0019).
//
// What is pinned and why. The three components are per entity and sit in flecs tables that every
// query over them walks, so their size is table bandwidth: at 10^4 animated instances a byte here
// is 10 KB of every archetype scan. `PosePool::Slot` is per pool slot, which is per instance
// again. The pose channels and `anim::JointMatrix` are pinned by `domain/anim`'s own table, and
// the arithmetic that matters — 40 bytes of pose and 48 of matrix per joint — is on the docs page.
#include <core/base/size_table.h>
#include <systems/animation/animation.h>
#include <systems/animation/pose_pool.h>

using namespace engine;

// 16 (clip) + 16 (fade_clip) + 6 * 4 (the playhead floats) + 2 bools, padded to the id128's
// 8-byte alignment: one cache line, and it is the only one of the three a save contains.
ENGINE_EXPECT_SIZE(64, 8, animation::AnimationPlayer);

// 16 (skeleton) + 5 * 4 (the slot, the joint count, and the three resolved indices).
ENGINE_EXPECT_SIZE(40, 8, animation::SkeletonInstance);

// 8 (frozen_at_us) + tier, divisor, phase and the two flags.
ENGINE_EXPECT_SIZE(16, 8, animation::AnimationLod);

// The pool's per-slot record: first joint, joint count, skeleton, live.
ENGINE_EXPECT_SIZE(16, 4, animation::PosePool::Slot);

// The LOD plan is a value a hot loop copies; four bytes keeps it in a register.
ENGINE_EXPECT_SIZE(4, 1, animation::LodPlan);
