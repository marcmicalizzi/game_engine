// Size table for domain/water_state (ADR-0019). The state has no per-cell struct: it is structure
// of arrays of double (docs/subsystems/water_state.md, "The state's layout"), so a cell's footprint
// is a count of arrays the page states, not a type here. What is pinned is the account every case
// and the card's step compare against, so a field added to it is a decision someone sees.
#include <core/base/size_table.h>
#include <domain/water_state/water_state.h>

using namespace engine;

ENGINE_EXPECT_SIZE(48, 8, water_state::Ledger);
ENGINE_EXPECT_SIZE(4, 1, water_state::Edges);
