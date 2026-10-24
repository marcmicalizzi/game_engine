// Size table for domain/authored (ADR-0019). The generator keeps one record per instance of the set
// for as long as a streamed scene is open — 7,804 of them for the v8 forest's authored trees and
// more for a whole region — so that record is pinned, and a field added to it is a decision.
#include <core/base/size_table.h>
#include <schemas/scene.h>

using namespace engine;

ENGINE_EXPECT_SIZE(64, 8, scene::AuthoredInstance);
