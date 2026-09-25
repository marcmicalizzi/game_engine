// Size table for core/schema (ADR-0019). Descriptors are cold, constant-initialized data;
// sizes are pinned so growth is noticed.
#include <core/base/size_table.h>
#include <core/schema/materialize.h>
#include <core/schema/type_info.h>

using namespace engine;
using namespace engine::schema;

ENGINE_EXPECT_SIZE(72, 8,
                   TypeRef);  // kind+count, size, align, type, element, key, three ops pointers
ENGINE_EXPECT_SIZE(96, 8, FieldInfo);
ENGINE_EXPECT_SIZE(24, 8, EnumValueInfo);
ENGINE_EXPECT_SIZE(32, 8, StructOps);
// The materialization table (materialize.h). A row is read once per mapped field per record
// materialized, so it is the one warm type here: component, two names, scale, offset, flags.
ENGINE_EXPECT_SIZE(48, 8, MaterializeField);
ENGINE_EXPECT_SIZE(56, 8, MaterializeInfo);
