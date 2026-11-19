// The schema walk over JSON text held to the tree path over the engine's own schemas (roadmap D40;
// tests/support/schema_text_property.h): `core/schema`'s tests run the same property over its
// feature types, and cannot link these, which are declared after it. One type of every schema file
// is named here so the linker keeps every file's registrations.

#include <core/schema/type_info.h>

#include <doctest/doctest.h>

#include <schema_text_property.h>
#include <schemas/doc.h>
#include <schemas/gfx.h>
#include <schemas/heightfield.h>
#include <schemas/protocol.h>
#include <schemas/provenance.h>
#include <schemas/scene.h>
#include <schemas/tissue.h>
#include <schemas/world.h>

using namespace engine;

TEST_CASE("json text: the engine's own schemas read the same both ways, and write as write_json") {
  const schema::TypeInfo* named[] = {
      &schema::type_of<content::AssetProvenance>(), &schema::type_of<doc::LayerFile>(),
      &schema::type_of<gfx::QueueFamilyInfo>(),     &schema::type_of<scene::Scene>(),
      &schema::type_of<protocol::RpcError>(),       &schema::type_of<tissue::Block>(),
      &schema::type_of<world::Transform>(),         &schema::type_of<terrain::Heightfield>()};
  for (const schema::TypeInfo* info : named)
    REQUIRE(info != nullptr);
  u32 types = 0;
  u32 texts = 0;
  test::schema_text::run_property(0xD40D0C5ull, 3, types, texts);
  MESSAGE(types << " types, " << texts << " texts");
  CHECK(types >= 100);
}
