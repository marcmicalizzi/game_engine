// The world-space cache's passes for a scene with terrain layers (R69 stage 5; renderer.md,
// "Indirect light: the world-space cache", "Stage 5"): gi_update.slang compiled with the terrain
// evaluator, so a bounce's hit on the ground takes the ground's own material
// (`terrain_hit_material`). Same entry points and ResolveParams; a scene without terrain layers
// keeps the plain module.
#define MATERIAL_TERRAIN 1
#include "gi_update.slang"
