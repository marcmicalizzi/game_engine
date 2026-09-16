# image: dependency-free image encoding for captures and tests. PNG with stored deflate blocks
# today; a real deflate, decoding, and EXR for HDR captures arrive with the capture API's
# consumers (docs/plan/06-tooling.md).
engine_module(NAME image LAYER foundation
  DEPS base containers io
  SOURCES src/png.cpp)

engine_module_tests(NAME image
  SOURCES tests/png_tests.cpp)
