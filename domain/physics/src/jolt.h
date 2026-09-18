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
