// Size table for systems/animation (ADR-0019).
//
// What is pinned and why. The three components are per entity and sit in flecs tables that every
// query over them walks, so their size is table bandwidth: at 10^4 animated instances a byte here
// is 10 KB of every archetype scan. `PosePool::Slot` is per pool slot, which is per instance
// again. The pose channels and `anim::JointMatrix` are pinned by `domain/anim`'s own table, and
// the arithmetic that matters — 40 bytes of pose and 48 of matrix per joint — is on the docs page.
#include <core/base/size_table.h>
#include <systems/animation/animation.h>
#include <systems/animation/graph.h>
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

// ---- graphs (S35 stage 1; docs/experiments/animation-graphs.md, "Parameters are components") ----

// A graph-driven entity's two persisted components, per entity in a flecs table the control pass
// walks: 16 (graph) + 8 parameters' f32 + the trigger bits, padded to the id's alignment ...
ENGINE_EXPECT_SIZE(56, 8, animation::AnimationGraphInput);
// ... and 16 (graph) + two machines' state, next (u16 each), elapsed, duration, time, next_time
// (f32 each) + two sync groups' phase. 120 bytes an instance between the two.
ENGINE_EXPECT_SIZE(64, 8, animation::AnimationGraphState);

// The compiled control table, read per instance per tick: a condition, a transition, a state's run
// of transitions, a machine. (The design page's names for the first two are its record types';
// the compiled ones are `Control*` so the two can live in one namespace.)
ENGINE_EXPECT_SIZE(8, 4, animation::ControlCondition);
ENGINE_EXPECT_SIZE(16, 4, animation::ControlTransition);
ENGINE_EXPECT_SIZE(4, 2, animation::ControlState);
ENGINE_EXPECT_SIZE(16, 4, animation::ControlMachine);
// A machine's step, written per instance per machine per tick by the batch control pass.
ENGINE_EXPECT_SIZE(12, 4, animation::GraphStep);

// ---- the pose program (S35 stage 2; animation.md, "Graphs: the pose program") ------------------

// One op, read per op per batch: kind, three registers and flags, the state and the clip within
// it, the library clip index and a rate or a duration (the design's `GraphOp`, 16/4).
ENGINE_EXPECT_SIZE(16, 4, animation::GraphOp);
// A state's pose, read per instance per tick by the control pass's leader rule and the program's
// weights: the first clip, the count and flags, the Blend1D's parameter, the first op.
ENGINE_EXPECT_SIZE(8, 2, animation::PoseState);
// A clip a state samples: the library index, the rate, the duration, the threshold.
ENGINE_EXPECT_SIZE(16, 4, animation::PoseClip);
// A layer (S35 stage 3; animation.md, "Graphs: layers and masks"), read per layer op per batch: its
// mask's run (first joint, full and partial counts), the reference pose's first joint, the weight's
// parameter, the kind, the machine, the weight and the op.
ENGINE_EXPECT_SIZE(24, 4, animation::GraphLayer);
// One instance of a batch the program runs over: its input and state, the tick's lead, the LOD
// flag, and its output pose as three spans (48).
ENGINE_EXPECT_SIZE(72, 8, animation::GraphInstance);
