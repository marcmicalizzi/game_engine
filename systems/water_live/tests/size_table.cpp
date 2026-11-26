// Size table for systems/water_live (ADR-0019): the structs the card mirrors
// (shaders/water_step.slang's WaterStepParams, WaterPartial and WaterLedger).
#include <core/base/size_table.h>
#include <systems/water_live/water_live.h>

using namespace engine;

ENGINE_EXPECT_SIZE(120, 8, water_live::StepParams);
ENGINE_EXPECT_SIZE(32, 4, water_live::WaterPartial);
ENGINE_EXPECT_SIZE(48, 4, water_live::WaterLedger);
