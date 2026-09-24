// Size table for domain/tissue (ADR-0019): the container's on-disk records, which a reader memcpys
// in and out and which a change of size silently breaks for every file already written.
#include <core/base/size_table.h>
#include <domain/tissue/tissue_file.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 8, tissue::TissueFileHeader);
ENGINE_EXPECT_SIZE(32, 8, tissue::TissueFileSection);
