# content_build: the content build as a library (docs/plan/07-content-pipeline.md §7.3,
# docs/subsystems/content_build.md) — import, validate, repair, repack, weld, cluster, page and
# write one mesh; a manifest of them with the identity skip; and the metrics of a built container.
# engine-content's `build`, `build-all` and `stats` and engine-host's `content.build` are its two
# hosts, which is why it is a module and not a file inside one of them.
#
# After atlas and texture: the build calls the atlas step, which depends on assets and geometry,
# and the texture step, so it cannot live in any of those and sits one step above them. Not
# OPTIONAL: nothing it links is a capability, and the content build is not a feature a
# configuration leaves out.
engine_module(NAME content_build LAYER domain
  DEPS base containers math hash time jobs log io json geometry assets atlas texture
  SOURCES src/content_build.cpp src/container_stats.cpp)

# The protocol's `ContentBuildOptions` states the same defaults `MeshOptions` has, and the test
# that holds them together reads both, so it links the generated protocol types.
engine_module_tests(NAME content_build
  SOURCES tests/content_build_tests.cpp
  DEPS schemas schema)
