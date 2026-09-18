# engine-image: the perceptual comparison of two frames on the command line
# (docs/plan/04-renderer.md §4.8). No GPU and no window: it reads two images, runs
# foundation/image's FLIP, PSNR, and SSIM over them, and prints the numbers.
engine_app(NAME engine_image OUTPUT engine-image
  SOURCES main.cpp
  DEPS base containers time io image
  E2E_TESTS tests/image_app_tests.cpp)
