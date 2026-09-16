// Size table for foundation/window (ADR-0019).
#include <core/base/size_table.h>
#include <foundation/window/window.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 4, window::Event);
ENGINE_EXPECT_SIZE(24, 8, window::Window);
