// A ruin on the dune generator's ground (world.md, "The consumers"; terrain.md, "The ruins'
// ground"): compiled where both the ruins and the terrain capability are, and linked with both,
// since the world links neither — its consumers reach them through the scene-generator registry.
// A building assembled on the field's floor (`terrain::DuneField::ground_height`, which the ruins'
// `Ground` query matches without either capability depending on the other) has its debris on that
// floor, which does not move while the dunes do.
#include <domain/ruins/assembler.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/synthetic_kit.h>
#include <domain/terrain/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;

TEST_CASE("world terrain: a ruin stands on the generator's ground, the same at any time") {
  const test::TempDir tmp("world_terrain_ruins");
  std::string error;
  std::string kit_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  ruins::Kit kit;
  REQUIRE_MESSAGE(ruins::read_kit_file(kit_path, kit, error), error);
  terrain::FieldDesc desc;
  desc.seed = 2026;
  desc.wind.seed = 2026;
  const terrain::DuneField field(desc);
  ruins::Placement placement;
  placement.world_seed = 5;
  placement.ground = ruins::Ground{&terrain::DuneField::ground_height, &field};
  ruins::Assembler assembler(kit);
  ruins::Output out;
  REQUIRE(assembler.assemble(placement, ruins::TileCoord{2, -1}, out, &error));
  REQUIRE(!out.instances.empty());
  u32 debris = 0;
  for (const ruins::Instance& piece : out.instances) {
    const f32 ground =
        terrain::DuneField::ground_height(&field, piece.position.x, piece.position.z);
    if (piece.kind == static_cast<u8>(ruins::PieceKind::debris)) {
      ++debris;
      CHECK(std::fabs(piece.position.y - ground) <= 0.07f);
    }
  }
  MESSAGE(out.instances.size() << " pieces, " << debris << " debris on the generator's floor");
}
