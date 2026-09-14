// Size table for core/json (ADR-0019).
#include <core/base/size_table.h>
#include <core/json/json.h>

using namespace engine;

ENGINE_EXPECT_SIZE(24, 8, JsonValue);  // kind + 16-byte payload
