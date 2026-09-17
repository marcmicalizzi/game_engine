// Size table for foundation/input (ADR-0019). Both types are copied per event and per binding
// lookup on the sim's input phase, and a replay holds one RawEvent per recorded event for the
// whole session, so their footprint is the log's footprint.
#include <core/base/size_table.h>
#include <foundation/input/input.h>

using namespace engine;

// SimTick (8, aligned 8), then the source byte, the code, the value, and the device pack into
// the next 16: 24 bytes, so a minute of a busy session at 60 Hz is well under a megabyte.
ENGINE_EXPECT_SIZE(24, 8, input::RawEvent);

// A source byte padded to the code, then two floats.
ENGINE_EXPECT_SIZE(16, 4, input::Binding);

// A Binding plus the Axis2 component it drives.
ENGINE_EXPECT_SIZE(20, 4, input::ActionBinding);
