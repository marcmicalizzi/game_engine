// Size table for foundation/scripting (ADR-0019). The handles and the argument travel through the
// per-tick call path by value; ScriptInstance is the tick driver's per-instance record, so its
// size is the host-side cost of one scripted instance (the VM-side cost is measured in the bench).
#include <core/base/size_table.h>
#include <foundation/scripting/scripting.h>

using namespace engine;

ENGINE_EXPECT_SIZE(4, 4, scripting::ScriptId);
ENGINE_EXPECT_SIZE(4, 4, scripting::FunctionId);
ENGINE_EXPECT_SIZE(8, 4, scripting::BindingId);
ENGINE_EXPECT_SIZE(16, 8, scripting::ObjectRef);
// A kind byte and a 16-byte payload (a borrowed string's pointer and length is the widest).
ENGINE_EXPECT_SIZE(24, 8, scripting::Arg);
ENGINE_EXPECT_SIZE(16, 4, scripting::ScriptInstance);
