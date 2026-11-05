// visibility_resolve_water with stochastic mesh materials (R131; renderer.md, "Stochastic mesh materials"): the
// renderer draws with it only when a material row of the scene carries k_material_stochastic, so
// every other scene keeps the kernel it had (the stochastic reads raise the resolve's registers).
#define MATERIAL_STOCHASTIC 1
#include "visibility_resolve_water.slang"
