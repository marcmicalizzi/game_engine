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
ENGINE_EXPECT_SIZE(136, 8, hotload::HostApi);  // ABI 2: the game runner's five services
ENGINE_EXPECT_SIZE(16, 8, hotload::ModuleContext);
ENGINE_EXPECT_SIZE(24, 8, hotload::Export);
ENGINE_EXPECT_SIZE(56, 8, hotload::ModuleDesc);
