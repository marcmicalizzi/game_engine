# atlas: the content build's atlas step (docs/subsystems/atlas.md, docs/plan/07-content-pipeline.md
# §7.4). Re-charts an imported mesh with xatlas, packs one square atlas per textured material, and
# rebakes the material's textures from the old UV space into the new one, so that a generator's
# fragmented atlas stops deciding how far the cluster LOD can collapse (E10,
# docs/experiments/e10-generated-props.md). A content-build step, not a runtime one: nothing in the
# renderer links it.
engine_module(NAME atlas LAYER domain
  DEPS base containers math hash time jobs io image geometry assets
  EXTERNAL_DEPS xatlas meshoptimizer
  SOURCES src/repack.cpp src/rebake.cpp src/proxy_charts.cpp)

engine_module_tests(NAME atlas
  SOURCES tests/rebake_tests.cpp tests/repack_tests.cpp)
