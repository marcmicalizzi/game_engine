// Size table for domain/texture (ADR-0019). These four are the `.tex` file itself, so their size is
// the format: a change is a version bump of the container, not a refactor.
#include <core/base/size_table.h>
#include <domain/texture/texture_file.h>

using namespace engine;

// The same header and section record the cluster container uses, so one reader of one reads the
// other's table.
ENGINE_EXPECT_SIZE(32, 8, texture::TextureFileHeader);

ENGINE_EXPECT_SIZE(24, 8, texture::TextureFileSection);

// Eight words: format, colour space, the level-0 extent, the level count, flags, what the source
// stored, and one reserved word that keeps the record a whole 32 bytes.
ENGINE_EXPECT_SIZE(32, 4, texture::TextureFileDesc);

// One mip level: its extent and where its blocks are in the payload.
ENGINE_EXPECT_SIZE(24, 8, texture::TextureFileLevel);
