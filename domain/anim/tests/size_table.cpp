// Size table for domain/anim (ADR-0019). A pose's channels are plain math types the math module
// already pins; what is pinned here is what crosses a boundary.
#include <core/base/size_table.h>
#include <domain/anim/clip.h>
#include <domain/anim/skeleton.h>
#include <domain/anim/standard_skeleton.h>

using namespace engine;

// The skinning matrix is a GPU record: `deform.slang` reads an array of these through a device
// address, so its layout is the shader contract. Three float4 rows, 48 bytes, no fourth row —
// a skinning matrix is affine for every joint of every pose, and the row that is always
// (0, 0, 0, 1) would be a quarter of an array uploaded per instance per frame.
ENGINE_EXPECT_SIZE(48, 4, anim::JointMatrix);

// One record per keyframe track; a clip of a hundred joints has three hundred of them, and they
// are walked in order while sampling.
ENGINE_EXPECT_SIZE(20, 4, anim::Track);

// The per-joint offsets of a retarget, one per target joint: a source index, the source's bind
// rotation inverse and translation, and the target's own bind transform.
ENGINE_EXPECT_SIZE(72, 4, anim::RetargetJoint);
