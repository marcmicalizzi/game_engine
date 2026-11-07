// Size table for domain/caves (ADR-0019). The capability has no hot type: a passage is content,
// solved once into a container. Its report is pinned so a field added to it is a stated change.
#include <core/base/size_table.h>
#include <domain/caves/caves.h>

using namespace engine;

ENGINE_EXPECT_SIZE(52, 4, caves::PassageReport);
