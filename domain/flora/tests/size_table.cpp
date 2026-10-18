// Size table for domain/flora (ADR-0019). The solver runs at content-build time and keeps no
// per-instance or per-frame state, so it has no hot type; the one record a caller keeps — the
// budget's account of a palm — is pinned so that a field added to it is a decision.
#include <core/base/size_table.h>
#include <domain/flora/palm.h>

using namespace engine;

ENGINE_EXPECT_SIZE(52, 4, flora::PalmReport);
