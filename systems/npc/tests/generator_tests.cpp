// The generator (docs/subsystems/npc.md, "The generator"): a pure function of its parameters,
// whatever the thread count, written to a document the store reads back.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>
#include <foundation/io/vfs.h>
#include <systems/npc/generator.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::npc;

namespace {

std::string layer_text(const doc::Layer& layer) { return layer.to_json_text(); }

}  // namespace

TEST_CASE("npc generator: the layer is the same bytes at none, one and four workers") {
  GeneratorParams p;
  p.seed = 9;
  p.residents = 5000;
  p.time_us = 7 * 60 * k_us_per_minute;
  GeneratorStats serial_stats;
  const std::string serial = layer_text(generate_layer(p, nullptr, &serial_stats));
  CHECK(serial_stats.residents == 5000);
  CHECK(serial_stats.places == 1250 + 125 + 100 + 50);
  CHECK(serial_stats.tiles > 16);
  for (u32 workers : {1u, 4u}) {
    CAPTURE(workers);
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 0;
    jobs::JobSystem jobs(config);
    CHECK(layer_text(generate_layer(p, &jobs)) == serial);
  }
  // And the same bytes on every compiler: the layer's canonical JSON, hashed.
  MESSAGE("layer hash " << hash_bytes(serial.data(), serial.size()));
  CHECK(hash_bytes(serial.data(), serial.size()) == 1890793958909862474ull);
  // Another seed is another world.
  p.seed = 10;
  CHECK(layer_text(generate_layer(p)) != serial);
}

TEST_CASE("npc generator: a resident is written where its routine has it at the layer's time") {
  GeneratorParams p;
  p.seed = 3;
  p.residents = 300;
  p.time_us = 3 * k_day_us + 11 * 60 * k_us_per_minute;
  const doc::Layer layer = generate_layer(p);
  const GeneratorParams r = resolved(p);
  const u32 places = r.homes + r.workplaces + r.services + r.leisure;
  CHECK(layer.size() == places + 300);
  CHECK(layer.partitioned());
  for (u32 i = 0; i < 300; ++i) {
    const doc::ObjectRecord* record = layer.find(resident_id(p.seed, i));
    REQUIRE(record != nullptr);
    CHECK(record->type == k_resident_type);
    const JsonValue* next = record->properties.find_value("next_event");
    REQUIRE(next != nullptr);
    i64 t = 0;
    CHECK(next->get_i64(t));
    CHECK(t > p.time_us);
    // Every resident has a home, a service place and a leisure place; only the working routines a
    // job.
    CHECK(record->properties.find_value("home") != nullptr);
    CHECK(record->properties.find_value("service") != nullptr);
  }
  // Every place is a Place, and a place's id is a function of the seed and its index.
  const doc::ObjectRecord* place = layer.find(place_id(p.seed, 0));
  REQUIRE(place != nullptr);
  CHECK(place->type == k_place_type);
}

TEST_CASE("npc generator: parameters are read strictly") {
  JsonValue json;
  REQUIRE(parse_json(R"({"seed":5,"residents":100000,"places":{"homes":20000,"workplaces":2000},
                         "extent":[-512,-512,512,512],"tile_size":32,"layer":"people",
                         "time_us":21600000000,"mix":{"retiree":50}})",
                     json)
              .ok);
  GeneratorParams p;
  std::string error;
  REQUIRE(parse_params(json, p, error));
  CHECK(p.seed == 5);
  CHECK(p.residents == 100000);
  CHECK(p.homes == 20000);
  CHECK(p.workplaces == 2000);
  CHECK(p.services == 0);
  CHECK(p.min_x == -512.0);
  CHECK(p.max_z == 512.0);
  CHECK(p.layer == "people");
  CHECK(p.time_us == 21'600'000'000);
  CHECK(p.mix[static_cast<u32>(Routine::Retiree)] == 50);

  REQUIRE(parse_json(R"({"residents":10,"colour":"blue"})", json).ok);
  CHECK_FALSE(parse_params(json, p, error));
  CHECK(error.find("colour") != std::string::npos);
  REQUIRE(parse_json(R"({"residents":0})", json).ok);
  CHECK_FALSE(parse_params(json, p, error));
  REQUIRE(parse_json(R"({"extent":[1,1,0,0]})", json).ok);
  CHECK_FALSE(parse_params(json, p, error));
}

TEST_CASE("npc generator: a document written, read back, and refused a second layer of the name") {
  const test::TempDir tmp("npc_generator_document");
  REQUIRE(tmp.ok());
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  GeneratorParams p;
  p.residents = 400;
  std::string error;
  GeneratorStats stats;
  REQUIRE_MESSAGE(generate_document(vfs, "docs://world", p, nullptr, error, &stats), error);
  doc::Document loaded;
  doc::DocumentManifest manifest;
  REQUIRE_MESSAGE(doc::DocumentStore::load(vfs, "docs://world", loaded, manifest, &error), error);
  const i32 layer = loaded.find_layer("residents");
  REQUIRE(layer >= 0);
  CHECK(loaded.edit_layer() == static_cast<u32>(layer));
  CHECK(loaded.layer(static_cast<u32>(layer)).partitioned());
  CHECK(loaded.layer(static_cast<u32>(layer)) == generate_layer(p));
  CHECK(loaded.type_of(resident_id(p.seed, 7)) == k_resident_type);
  CHECK_FALSE(generate_document(vfs, "docs://world", p, nullptr, error));
  CHECK(error.find("already has a layer") != std::string::npos);
}
