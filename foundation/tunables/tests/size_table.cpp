// Size table for foundation/tunables (ADR-0019). Tunables are cold static objects; the hot
// operation is get(), one relaxed load. Sizes are pinned so accidental growth is noticed.
#include <core/base/size_table.h>
#include <foundation/tunables/tunables.h>

using namespace engine;

ENGINE_EXPECT_SIZE(72, 8, tunables::Int);
ENGINE_EXPECT_SIZE(72, 8, tunables::Float);
ENGINE_EXPECT_SIZE(48, 8, tunables::Bool);
ENGINE_EXPECT_SIZE(64, 8, tunables::EnumBase);
// `tunables::String` is not pinned: it holds a std::mutex and two std::string, whose sizes are the
// standard library's (MSVC's debug string is 8 bytes larger than its release one), and it is read
// once at load, never in a loop.
