#pragma once

// <Jolt/Jolt.h> must be included before any other Jolt header: it selects the SIMD width,
// defines JPH_NAMESPACE_BEGIN and the warning suppression every other Jolt header opens with,
// and fails loudly ("#error Undefined") if something else got there first.
//
// That rule and clang-format do not get along: the engine's style sorts includes inside a
// block, and "Jolt/Core/..." sorts before "Jolt/Jolt.h". So the rule lives in this one header,
// which every file under src/ includes before it names anything from JPH, instead of in a
// comment that the next `tools/dev.ps1 format` quietly undoes.

#include <Jolt/Jolt.h>

// The backend is built in double precision and only so (ADR-0053; cmake/EnginePhysics.cmake): a
// body's position is an `RVec3` of doubles, and the conversions in backend.h are written for that.
// A build that reached here without it would compile `RVec3` as floats and put every far position
// back on a float's grid, which is the thing the decision exists to stop.
#ifndef JPH_DOUBLE_PRECISION
#error \
    "domain/physics is built against Jolt in double precision (ADR-0053); JPH_DOUBLE_PRECISION is not defined"
#endif
