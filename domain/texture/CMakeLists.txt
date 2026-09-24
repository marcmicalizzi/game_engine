# texture: the engine's built texture (docs/subsystems/texture.md, docs/plan/07-content-pipeline.md
# §7.2, ADR-0036). The `.tex` container, the mip chain in linear light, block compression to BC1,
# BC3, BC4, BC5 and BC7 through the vendored bc7enc/rgbcx encoders (cmake/EngineTexture.cmake),
# the derived-data cache key, and the per-slot defaults a mesh's materials ask for. The content
# build writes textures with it; the renderer reads them. Not a capability: the renderer's
# materials depend on it, and a foundation that can be switched off is one nobody can rely on.
engine_module(NAME texture LAYER domain
  DEPS base containers hash time jobs io image geometry
  EXTERNAL_DEPS bc7enc
  SOURCES src/texture_file.cpp src/texture_build.cpp src/mips.cpp src/blocks.cpp
          src/material_textures.cpp)

engine_module_tests(NAME texture
  SOURCES tests/texture_file_tests.cpp tests/mips_tests.cpp tests/blocks_tests.cpp
          tests/material_textures_tests.cpp tests/determinism_tests.cpp tests/size_table.cpp)
