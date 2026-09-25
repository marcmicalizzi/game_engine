// The table a `materialize` declaration compiles to, and the two directions of one row
// (core/schema/materialize.h; docs/subsystems/schema.md, "Materialization").
#include <core/schema/materialize.h>

#include <doctest/doctest.h>

#include <cmath>
#include <schemas/materialize_test.h>
#include <string>

using namespace engine;
using namespace engine::schema;
using namespace engine::schema_test::world;

namespace {

const MaterializeInfo& crate_mapping() {
  const MaterializeInfo* info =
      MaterializeRegistry::global().find("engine.schema_test.world.Crate");
  REQUIRE(info != nullptr);
  return *info;
}

const MaterializeField& row(const MaterializeInfo& info, const char* field) {
  for (const MaterializeField& f : info.fields) {
    if (std::string(f.field) == field) return f;
  }
  FAIL("no row for " << field);
  return info.fields[0];
}

constexpr f64 k_pi_f64 = 3.14159265358979323846;

}  // namespace

TEST_CASE("schema: a materialize declaration compiles to a registered table") {
  const MaterializeInfo& info = crate_mapping();
  CHECK(info.record == &type_of<Crate>());
  CHECK(std::string(info.doc) == "A crate is placed, warm, and marked.");
  CHECK(info.parent == MaterializeParent::ChildOf);
  CHECK(info.tiers == 0b0111u);

  // Components in the order the declaration first names them, the bare one included.
  REQUIRE(info.components.size() == 3);
  CHECK(info.components[0] == &type_of<Place>());
  CHECK(info.components[1] == &type_of<Warmth>());
  CHECK(info.components[2] == &type_of<Marker>());

  // Field rows grouped by component, in declaration order within it.
  REQUIRE(info.fields.size() == 5);
  const char* fields[] = {"position", "heading", "mass", "kelvin", "label"};
  const TypeInfo* owners[] = {&type_of<Place>(), &type_of<Place>(), &type_of<Place>(),
                              &type_of<Warmth>(), &type_of<Warmth>()};
  for (u32 i = 0; i < 5; ++i) {
    CHECK(std::string(info.fields[i].field) == fields[i]);
    CHECK(info.fields[i].component == owners[i]);
  }

  // What the units made of each row.
  const MaterializeField& position = row(info, "position");
  CHECK(std::string(position.property) == "position");
  CHECK(position.flags == (MaterializeFlag::convert | MaterializeFlag::writeback));
  CHECK(position.scale == doctest::Approx(0.01));
  CHECK(position.offset == 0.0);
  CHECK(row(info, "heading").scale == doctest::Approx(k_pi_f64 / 180.0));
  CHECK(row(info, "mass").scale == doctest::Approx(0.001));
  const MaterializeField& kelvin = row(info, "kelvin");
  CHECK(kelvin.flags == MaterializeFlag::convert);
  CHECK(kelvin.scale == 1.0);
  CHECK(kelvin.offset == doctest::Approx(273.15));
  CHECK(row(info, "label").flags == 0);

  // A record type with no declaration has no table: it stays document-only by design.
  CHECK(MaterializeRegistry::global().find("engine.schema_test.world.Note") == nullptr);
  bool listed = false;
  for (const MaterializeInfo* m : MaterializeRegistry::global().all())
    listed = listed || m == &info;
  CHECK(listed);
}

TEST_CASE("schema: a row reads a record property into a component field, converting units") {
  const MaterializeInfo& info = crate_mapping();
  Place place;
  Warmth warmth;
  const TypeInfo& place_info = type_of<Place>();
  const TypeInfo& warmth_info = type_of<Warmth>();

  ReadContext ctx;
  JsonValue position = JsonValue::array();
  position.push_back(JsonValue(100.0));
  position.push_back(JsonValue(-250.0));
  position.push_back(JsonValue(5.0));
  REQUIRE(read_mapped(row(info, "position"), *place_info.find_field("position"), &place, position,
                      ctx));
  CHECK(place.position.x == doctest::Approx(1.0f));
  CHECK(place.position.y == doctest::Approx(-2.5f));
  CHECK(place.position.z == doctest::Approx(0.05f));

  REQUIRE(read_mapped(row(info, "heading"), *place_info.find_field("heading"), &place,
                      JsonValue(180.0), ctx));
  CHECK(place.heading == doctest::Approx(static_cast<f32>(k_pi_f64)));
  // f32 on the record, f64 on the component: the conversion is done in f64 and stored wide.
  REQUIRE(read_mapped(row(info, "mass"), *place_info.find_field("mass"), &place, JsonValue(1500.0),
                      ctx));
  CHECK(place.mass == doctest::Approx(1.5));
  // Temperature is affine: 20 °C is 293.15 K, which a scale alone could not say.
  REQUIRE(read_mapped(row(info, "kelvin"), *warmth_info.find_field("kelvin"), &warmth,
                      JsonValue(20.0), ctx));
  CHECK(warmth.kelvin == doctest::Approx(293.15f));
  // A direct copy is the field type's own reader.
  REQUIRE(read_mapped(row(info, "label"), *warmth_info.find_field("label"), &warmth,
                      JsonValue("fragile"), ctx));
  CHECK(warmth.label == "fragile");
  CHECK(ctx.ok());

  // A value that does not fit leaves the field as it was and says where.
  const f32 before = place.heading;
  ReadContext bad;
  CHECK_FALSE(read_mapped(row(info, "heading"), *place_info.find_field("heading"), &place,
                          JsonValue("north"), bad));
  CHECK(place.heading == before);
  CHECK_FALSE(bad.ok());
}

TEST_CASE("schema: write-back is the inverse of the row, in the record's units") {
  const MaterializeInfo& info = crate_mapping();
  Place place;
  place.position = Vec3{1.0f, -2.5f, 0.25f};
  JsonValue out;
  REQUIRE(
      write_mapped(row(info, "position"), *type_of<Place>().find_field("position"), &place, out));
  REQUIRE(out.is_array());
  REQUIRE(out.size() == 3);
  CHECK(out[0].as_float() == doctest::Approx(100.0));
  CHECK(out[1].as_float() == doctest::Approx(-250.0));
  CHECK(out[2].as_float() == doctest::Approx(25.0));

  Warmth warmth;
  warmth.kelvin = 300.0f;
  REQUIRE(write_mapped(row(info, "kelvin"), *type_of<Warmth>().find_field("kelvin"), &warmth, out));
  CHECK(out.as_float() == doctest::Approx(26.85).epsilon(1e-4));
  warmth.label = "done";
  REQUIRE(write_mapped(row(info, "label"), *type_of<Warmth>().find_field("label"), &warmth, out));
  CHECK(out.as_string() == "done");
}

TEST_CASE("schema: write-back fields are the ones that are their own bytes") {
  CHECK(is_byte_comparable(type_of<Place>().find_field("position")->type));
  CHECK(is_byte_comparable(type_of<Place>().find_field("mass")->type));
  CHECK(is_byte_comparable(type_of<Marker>().find_field("weight")->type));
  CHECK_FALSE(is_byte_comparable(type_of<Warmth>().find_field("label")->type));
}

TEST_CASE("schema: a stable type id is FNV-1a of the qualified name") {
  // Pinned: the id is written into save files and read back by other builds, so it may never
  // change for a name that already has one.
  CHECK(stable_type_id("") == 0x811C9DC5u);
  CHECK(stable_type_id("a") == 0xE40C292Cu);
  CHECK(stable_type_id("engine.world.WriteBack") == stable_type_id("engine.world.WriteBack"));
  CHECK(stable_type_id("engine.world.WriteBack") != stable_type_id("engine.world.Transform"));
}
