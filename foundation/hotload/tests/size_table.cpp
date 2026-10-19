// Size table for foundation/hotload (ADR-0019). These are the module ABI's structs: a module built
// against one layout and a host built against another must never meet, so a change here is a
// change of `k_abi_version` (module_abi.h) in the same commit. From ABI 4 a change may only append
// (a module built against `k_abi_oldest` still loads), so every struct below keeps its prefix.
#include <core/base/size_table.h>
#include <foundation/hotload/module_abi.h>

using namespace engine;

ENGINE_EXPECT_SIZE(8, 4, hotload::View);
ENGINE_EXPECT_SIZE(4, 4, hotload::FieldRef);
ENGINE_EXPECT_SIZE(48, 8, hotload::Value);
// ABI 2 added the game runner's five services (136); ABI 3 the ten through which a game acts on the
// world (props, the walker, sound), 80 bytes of function pointers after them (216); ABI 4 the HUD's
// elements and the graphics state, four more (248); ABI 5 the seven actor entries and the ground
// query, eight more (312). A module built against 3 reads the first 216, one against 4 the first
// 248.
ENGINE_EXPECT_SIZE(312, 8, hotload::HostApi);
// ABI 4's structs, each led by its own size so it can grow the way the table does: an element is
// 56 bytes of fixed fields and the text's pointer and length; the graphics state is six words, five
// 24-byte names (the longest present mode, "fifo-latest-ready", is 17) and four 96-byte reasons; a
// request four words and four names.
ENGINE_EXPECT_SIZE(72, 8, hotload::HudElement);
ENGINE_EXPECT_SIZE(528, 4, hotload::GraphicsState);
ENGINE_EXPECT_SIZE(112, 4, hotload::GraphicsRequest);
// ABI 5: an actor as the host keeps it, led by its size: two words, the f64 position, eight
// four-byte fields, the pose hash and a 64-byte clip name.
ENGINE_EXPECT_SIZE(136, 8, hotload::ActorState);
ENGINE_EXPECT_SIZE(16, 8, hotload::ModuleContext);
ENGINE_EXPECT_SIZE(24, 8, hotload::Export);
ENGINE_EXPECT_SIZE(56, 8, hotload::ModuleDesc);
