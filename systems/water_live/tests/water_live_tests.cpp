// The water_live cases that open no device (docs/subsystems/water_live.md, "Invariants"); the
// reference's cases on the card are tests/water_live_device_tests.cpp.
#include <systems/water_live/water_live.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>
#include <shaders/water_step.spv.h>
#include <string_view>

using namespace engine;

TEST_CASE("water_live declares a determinism stance") {
  CHECK(std::string_view{water_live::k_determinism} == "hashed");
}

TEST_CASE("water_live: the mirrored structs keep the shader's offsets") {
  // shaders/water_step.slang reads these fields at these offsets (scalar layout of a push block and
  // of device-address structs).
  CHECK(offsetof(water_live::StepParams, ledger) == 48u);
  CHECK(offsetof(water_live::StepParams, nx) == 56u);
  CHECK(offsetof(water_live::StepParams, flags) == 72u);
  CHECK(offsetof(water_live::StepParams, max_dt) == 92u);
  CHECK(offsetof(water_live::StepParams, source_scale) == 96u);
  CHECK(offsetof(water_live::StepParams, faces_x) == 104u);
  CHECK(offsetof(water_live::StepParams, faces_y) == 112u);
  CHECK(offsetof(water_live::WaterPartial, roundoff_m3) == 16u);
  CHECK(offsetof(water_live::WaterLedger, remaining_s) == 32u);
  CHECK(offsetof(water_live::WaterLedger, dt) == 36u);
  CHECK(offsetof(water_live::WaterLedger, steps) == 40u);
}

TEST_CASE("water_live: a step that was never created refuses every call") {
  water_live::DeviceStep step;
  std::string error;
  const f64 one[1] = {0.0};
  CHECK_FALSE(step.valid());
  CHECK_FALSE(step.set_bed(one, 0.0, &error));
  CHECK_FALSE(step.run_steps(1, 1.0f, &error));
  CHECK(step.advance(1.0, &error) == 0u);
  CHECK(step.state_hash(&error) == 0u);
}

TEST_CASE("water_live: the hash is FNV-1a over the floats' bytes") {
  // FNV-1a's offset basis for no bytes, and a value that moves when one bit does.
  CHECK(water_live::hash_floats({}) == 0xcbf29ce484222325ull);
  const f32 a[2] = {1.0f, 2.0f};
  const f32 b[2] = {1.0f, 2.0000002f};
  CHECK(water_live::hash_floats(a) != water_live::hash_floats(b));
}

TEST_CASE("water_live: the step's SPIR-V forbids contraction and asks for no fused multiply-add") {
  // `precise` reaches the SPIR-V as NoContraction (decoration 42) on the results it governs, which
  // forbids a driver from fusing a multiply and an add; GLSL.std.450 Fma (instruction 50) would
  // fuse on purpose. Counted over the embedded module (docs/experiments/water-step-on-the-card.md,
  // "The same bytes on every device").
  const usize words = shaders::k_water_step_spirv_size / 4;
  REQUIRE(words > 5);
  auto word = [](usize i) {
    u32 w = 0;
    std::memcpy(&w, shaders::k_water_step_spirv + 4 * i, 4);
    return w;
  };
  REQUIRE(word(0) == 0x07230203u);
  u32 no_contraction = 0, float_arith = 0, fma = 0, glsl_set = ~0u;
  for (usize i = 5; i < words;) {
    const u32 head = word(i);
    const u32 count = head >> 16;
    const u32 op = head & 0xffffu;
    if (count == 0 || i + count > words) break;
    if (op == 71 && count >= 3 && word(i + 2) == 42) ++no_contraction;  // OpDecorate NoContraction
    if (op == 129 || op == 131 || op == 133 || op == 136) ++float_arith;  // FAdd FSub FMul FDiv
    if (op == 11) {  // OpExtInstImport: is it GLSL.std.450?
      char name[16] = {};
      std::memcpy(name, shaders::k_water_step_spirv + 4 * (i + 2), count > 2 ? 12 : 0);
      if (std::string_view{name}.starts_with("GLSL.std.450")) glsl_set = word(i + 1);
    }
    if (op == 12 && count >= 5 && word(i + 3) == glsl_set && word(i + 4) == 50) ++fma;
    i += count;
  }
  MESSAGE("water_step.spv: " << float_arith << " float add/sub/mul/div, " << no_contraction
                             << " NoContraction decorations, " << fma << " Fma");
  // `-fp-mode precise` decorates every float add, subtract, multiply and divide (and a few more
  // results), so there are at least as many decorations as those operations.
  CHECK(float_arith > 0u);
  CHECK(no_contraction >= float_arith);
  CHECK(fma == 0u);
}