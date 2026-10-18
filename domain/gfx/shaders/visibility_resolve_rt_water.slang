// The material resolve with ray-traced shadows and the water branch: visibility_resolve.slang
// compiled with both RESOLVE_RAY_SHADOWS and RESOLVE_WATER, same entry points and ResolveParams.
// visibility_resolve_rt.slang says why the rays are a module of their own, and
// visibility_resolve_water.slang why the water is.

#define RESOLVE_RAY_SHADOWS 1
#define RESOLVE_WATER 1
#include "visibility_resolve.slang"
