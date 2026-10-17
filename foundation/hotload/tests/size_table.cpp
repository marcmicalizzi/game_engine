// Size table for foundation/hotload (ADR-0019). These are the module ABI's structs: a module built
// against one layout and a host built against another must never meet, so a change here is a
// change of `k_abi_version` (module_abi.h) in the same commit, and the host's check refuses every
// module built before it.
#include <core/base/size_table.h>
#include <foundation/hotload/module_abi.h>

using namespace engine;

ENGINE_EXPECT_SIZE(8, 4, hotload::View);
ENGINE_EXPECT_SIZE(4, 4, hotload::FieldRef);
ENGINE_EXPECT_SIZE(48, 8, hotload::Value);
// ABI 2 added the game runner's five services (136); ABI 3 the ten through which a game acts on the
// world (props, the walker, sound), 80 bytes of function pointers after them.
ENGINE_EXPECT_SIZE(216, 8, hotload::HostApi);
ENGINE_EXPECT_SIZE(16, 8, hotload::ModuleContext);
ENGINE_EXPECT_SIZE(24, 8, hotload::Export);
ENGINE_EXPECT_SIZE(56, 8, hotload::ModuleDesc);
