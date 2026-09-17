# image: PNG encoding without a compression library (stored deflate blocks) for captures and
# tests, and CPU decoding of PNG/JPEG/TGA/BMP through stb_image. A real deflate for encoding,
# KTX2 with block-compressed formats, and EXR for HDR captures arrive with the capture and
# texture pipelines' consumers (docs/plan/06-tooling.md).
engine_module(NAME image LAYER foundation
  DEPS base containers io
  SOURCES src/png.cpp src/decode.cpp)
# stb_image.h is compiled into src/decode.cpp; third-party headers are system headers so the
# engine's warning policy applies to our code only. Nothing outside this module sees stb.
target_include_directories(engine_image SYSTEM PRIVATE "${stb_SOURCE_DIR}")

engine_module_tests(NAME image
  SOURCES tests/png_tests.cpp tests/decode_tests.cpp tests/size_table.cpp)
if(TARGET engine_image_tests)
  # stb_image_write.h writes the baseline JPEG the decode test reads back; tests only, never
  # linked into the module.
  target_include_directories(engine_image_tests SYSTEM PRIVATE "${stb_SOURCE_DIR}")
endif()
