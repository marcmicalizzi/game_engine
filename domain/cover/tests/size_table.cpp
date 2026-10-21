// Size table for domain/cover (ADR-0019): the per-triangle records the solvers keep for every
// triangle of a host, which is what a large host's memory is made of.
#include <core/base/size_table.h>
#include <domain/cover/host.h>

using namespace engine;

ENGINE_EXPECT_SIZE(24, 8, cover::P3);
ENGINE_EXPECT_SIZE(24, 4, cover::Exposure);
