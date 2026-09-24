# image: PNG encoding with the engine's own deflate (RFC 1951, no compression library) for
# captures and tests, and CPU decoding of PNG/JPEG/TGA/BMP through stb_image. Block-compressed,
# mipmapped textures are domain/texture's (docs/subsystems/texture.md), which decodes through this
# module; EXR for HDR captures arrives with its consumer (docs/plan/06-tooling.md).
engine_module(NAME image LAYER foundation
  DEPS base containers io
  SOURCES src/png.cpp src/deflate.cpp src/decode.cpp src/metrics.cpp)
# stb_image.h is compiled into src/decode.cpp; third-party headers are system headers so the
# engine's warning policy applies to our code only. Nothing outside this module sees stb.
target_include_directories(engine_image SYSTEM PRIVATE "${stb_SOURCE_DIR}")

engine_module_tests(NAME image
  SOURCES tests/png_tests.cpp tests/deflate_tests.cpp tests/decode_tests.cpp
          tests/metrics_tests.cpp tests/size_table.cpp)
if(TARGET engine_image_tests)
  # stb_image_write.h writes the baseline JPEG the decode test reads back; tests only, never
  # linked into the module.
  target_include_directories(engine_image_tests SYSTEM PRIVATE "${stb_SOURCE_DIR}")
endif()

# Deflate throughput and ratio per level (docs/subsystems/image.md, "Performance notes").
engine_module_bench(NAME image
  SOURCES bench/image_bench.cpp)
