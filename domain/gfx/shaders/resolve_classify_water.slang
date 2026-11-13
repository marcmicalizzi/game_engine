// The resolve's classify pass for a scene with water: resolve_classify.slang compiled with
// RESOLVE_WATER, so that `ResolveParams` names the shore records and the pass writes the two
// classes that need the water module — a water material's pixels, and ground within a shore's or a
// bank's reach (`classify_wet`). Same entry points (vs_classify, fs_classify).

#define RESOLVE_WATER 1
#include "resolve_classify.slang"
