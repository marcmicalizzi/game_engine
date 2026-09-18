# anim: skeletons, poses, clips, and the standard skeleton (docs/plan/05-simulation.md §5.11).
# The data half of animation — sampling, blending, and the matrices a skinned mesh is deformed by
# — with no system, no scheduler, and no GPU. The blend graphs and state machines the plan names
# belong to a `systems/animation` capability that builds on this.
#
# It depends on `geometry` because the per-vertex `SkinBinding` is geometry's format and
# `skin_positions` is the CPU reference for the shader that reads it; duplicating an eight-byte
# record here to avoid a same-layer dependency would be a copy that can drift.
engine_module(NAME anim LAYER domain
  DEPS base containers math time log geometry
  SOURCES src/skeleton.cpp src/clip.cpp src/standard_skeleton.cpp)

engine_module_tests(NAME anim
  SOURCES tests/anim_tests.cpp tests/retarget_tests.cpp tests/size_table.cpp)
