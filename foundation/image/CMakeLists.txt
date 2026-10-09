# image: PNG encoding with the engine's own deflate (RFC 1951, no compression library) for
# captures and tests, CPU decoding of PNG/JPEG/TGA/BMP through stb_image, and half-float OpenEXR
# through tinyexr for HDR captures (E39, roadmap R79; `exr.h`). Block-compressed, mipmapped
# textures are domain/texture's (docs/subsystems/texture.md), which decodes through this module.
engine_module(NAME image LAYER foundation
  DEPS base containers io
  SOURCES src/png.cpp src/deflate.cpp src/decode.cpp src/metrics.cpp src/exr.cpp)
# stb_image.h is compiled into src/decode.cpp and tinyexr into its own static library
# (cmake/EngineImage.cmake) that only src/exr.cpp includes; third-party headers are system headers
# so the engine's warning policy applies to our code only. Nothing outside this module sees either.
target_include_directories(engine_image SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_link_libraries(engine_image PRIVATE engine_tinyexr)

engine_module_tests(NAME image
  SOURCES tests/png_tests.cpp tests/deflate_tests.cpp tests/decode_tests.cpp
          tests/metrics_tests.cpp tests/exr_tests.cpp tests/size_table.cpp)
if(TARGET engine_image_tests)
  # stb_image_write.h writes the baseline JPEG the decode test reads back; tests only, never
  # linked into the module.
  target_include_directories(engine_image_tests SYSTEM PRIVATE "${stb_SOURCE_DIR}")
endif()

# Deflate throughput and ratio per level (docs/subsystems/image.md, "Performance notes").
engine_module_bench(NAME image
  SOURCES bench/image_bench.cpp)
