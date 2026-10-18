// The material resolve with the water branch: visibility_resolve.slang compiled with RESOLVE_WATER,
// same entry points (vs_fullscreen, fs_resolve) and same ResolveParams.
//
// It is a module of its own rather than a branch in every resolve because a fragment shader's
// registers are allocated for its largest path, taken or not: the water branch in the one module
// cost a ray-traced surround frame with no water in it 0.48 ms of resolve (2026-10-18,
// docs/subsystems/water.md, "Shading"). The renderer draws with this one only when its scene has a
// water material; visibility_resolve_rt_water.slang is the same with the shadow rays.

#define RESOLVE_WATER 1
#include "visibility_resolve.slang"
