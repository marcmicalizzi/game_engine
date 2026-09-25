// The whole-island plan (docs/subsystems/city.md, "The plan", "Invariants"): determinism on one
// thread and on a pool, the golden hash every toolchain reproduces, the plan's structure, its
// districts, parks and civic reservations, the per-tile query and the identity between a tile's
// content and the whole plan's restricted to it, authored overrides, and the plan as a file.
#include "city_fixtures.h"

#include <core/jobs/job_system.h>
#include <domain/city/city.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <string>

using namespace engine;
using namespace engine::city;
using engine::city::test::default_plan;
using engine::city::test::k_golden_seed;
using engine::city::test::make_params;
using engine::city::test::make_plan;
using engine::city::test::small_plan;

namespace {

// The default island at seed 2026: a plan made of integers, the engine's hash and nothing else, so
// the same number on MSVC, GCC and Clang, at x86-64-v2 and v3. A change to the plan's rules or
// draws moves it: say so in the commit, and take the new number from the failure message.
// Taken on Clang 18 and reproduced by GCC 13 on 2026-09-25; MSVC has not run it yet.
constexpr u64 k_plan_golden = 0x0a734487921d1072ull;

u32 count_if_lots(const Plan& plan, auto&& pred) {
  u32 n = 0;
  for (const Lot& l : plan.lots)
    n += pred(l) ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("city plan: the same parameters are the same plan, on any thread count") {
  const Params p = make_params(k_golden_seed, 1200.0f);
  const u64 once = hash_plan(make_plan(p));
  CHECK(hash_plan(make_plan(p)) == once);
  for (const u32 workers : {1u, 5u}) {
    jobs::JobSystem js(jobs::JobSystemConfig{
        .performance_workers = workers, .efficiency_workers = 1, .pin_threads = false});
    Plan plan;
    std::string error;
    REQUIRE_MESSAGE(generate_plan(p, plan, error, &js), error);
    CHECK_MESSAGE(hash_plan(plan) == once, "on " << workers << " workers and the caller");
  }
  // A different seed is a different island.
  CHECK(hash_plan(make_plan(make_params(k_golden_seed + 1, 1200.0f))) != once);
}

TEST_CASE("city plan: the default island at the golden seed hashes to the committed number") {
  const u64 h = hash_plan(default_plan());
  char text[19];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(h));
  MESSAGE("plan hash " << text << ", " << default_plan().lots.size() << " lots");
  CHECK_MESSAGE(h == k_plan_golden, "this build's plan hash is " << text);
}

TEST_CASE("city plan: blocks, lots and streets are well formed") {
  const Plan& plan = default_plan();
  REQUIRE(!plan.lots.empty());
  CHECK(std::is_sorted(plan.streets.begin(), plan.streets.end(),
                       [](const Street& a, const Street& b) { return a.id < b.id; }));
  CHECK(std::is_sorted(plan.blocks.begin(), plan.blocks.end(),
                       [](const Block& a, const Block& b) { return a.id < b.id; }));
  CHECK(std::is_sorted(plan.lots.begin(), plan.lots.end(),
                       [](const Lot& a, const Lot& b) { return a.id < b.id; }));
  for (const Block& b : plan.blocks) {
    // A block's id is its superblock's and its place in it.
    CHECK(b.id / 256 == b.superblock);
    CHECK(b.buildable.width() > 0);
    CHECK(b.buildable.depth() > 0);
    // Its lots are inside what the streets leave of it, and never overlap one another.
    for (u32 i = b.first_lot; i < b.first_lot + b.lot_count; ++i) {
      const Lot& l = plan.lots[i];
      CHECK(l.id / 1024 == b.id);
      CHECK(l.rect.x0 >= b.buildable.x0);
      CHECK(l.rect.z0 >= b.buildable.z0);
      CHECK(l.rect.x1 <= b.buildable.x1);
      CHECK(l.rect.z1 <= b.buildable.z1);
      for (u32 j = i + 1; j < b.first_lot + b.lot_count; ++j) {
        const Lot& m = plan.lots[j];
        const bool apart = l.rect.x1 <= m.rect.x0 || m.rect.x1 <= l.rect.x0 ||
                           l.rect.z1 <= m.rect.z0 || m.rect.z1 <= l.rect.z0;
        CHECK_MESSAGE(apart, "lots " << l.id << " and " << m.id << " overlap");
      }
    }
    // Every block is on land; a city block is short of the city limit at every corner, a
    // greenbelt block at one at least short of the mountain.
    CHECK(on_land(plan, b.rect.x0, b.rect.z0));
    CHECK(on_land(plan, b.rect.x1, b.rect.z1));
    i64 nearest = along_mountain(plan, b.rect.x0, b.rect.z0), farthest = nearest;
    for (const i64 m :
         {along_mountain(plan, b.rect.x1, b.rect.z0), along_mountain(plan, b.rect.x0, b.rect.z1),
          along_mountain(plan, b.rect.x1, b.rect.z1)}) {
      nearest = std::min(nearest, m);
      farthest = std::max(farthest, m);
    }
    if (b.park == ParkKind::Greenbelt)
      CHECK(nearest < plan.mountain_start_cm);
    else
      CHECK(farthest < plan.city_limit_cm);
  }
  // Every segment lies along its street's line, between two distinct nodes.
  for (const Segment& s : plan.segments) {
    const Street& st = plan.streets[s.street];
    const Node& a = plan.nodes[s.a];
    const Node& b = plan.nodes[s.b];
    CHECK(s.a != s.b);
    if (st.along_x != 0) {
      CHECK(a.z == st.line_cm);
      CHECK(b.z == st.line_cm);
    } else {
      CHECK(a.x == st.line_cm);
      CHECK(b.x == st.line_cm);
    }
  }
}

TEST_CASE("city plan: the default island has every district, park and civic kind") {
  const Plan& plan = default_plan();
  PlanStats st;
  plan_stats(plan, st);
  for (u32 k = 0; k < k_district_kinds; ++k)
    CHECK_MESSAGE(st.districts_by_kind[k] > 0,
                  std::string(district_kind_name(static_cast<DistrictKind>(k))));
  for (u32 k = 1; k < 7; ++k)
    CHECK_MESSAGE(st.parks_by_kind[k] > 0, std::string(park_kind_name(static_cast<ParkKind>(k))));
  for (u32 k = 1; k < 5; ++k)
    CHECK_MESSAGE(st.civic_by_kind[k] > 0, std::string(civic_kind_name(static_cast<CivicKind>(k))));
  for (u32 a = 0; a < k_archetypes; ++a)
    CHECK_MESSAGE(st.lots_by_archetype[a] > 0,
                  std::string(archetype_name(static_cast<Archetype>(a))));
  for (u32 c = 0; c < k_street_classes; ++c)
    CHECK_MESSAGE(st.streets_by_class[c] > 0,
                  std::string(street_class_name(static_cast<StreetClass>(c))));
  // Civic reservations are never subdivided: a hospital, a station or a government building is its
  // block's only lot; a campus's lots are all university.
  for (const Lot& l : plan.lots) {
    if (l.use != LotUse::Civic) continue;
    CHECK(l.archetype == Archetype::CivicShell);
    const Block& b = plan.blocks[l.block];
    if (plan.districts[l.district].kind != DistrictKind::Campus)
      CHECK(b.lot_count == 1);
    else
      CHECK(l.civic == CivicKind::University);
  }
  // The district mix's shares are roughly what the island got: residential the most, downtown a
  // compact core.
  CHECK(st.district_area_cm2[static_cast<u32>(DistrictKind::Residential)] >
        st.district_area_cm2[static_cast<u32>(DistrictKind::Downtown)]);
}

TEST_CASE("city plan: the plan's own validators pass on islands of three sizes and five seeds") {
  for (const f32 radius : {900.0f, 2000.0f, 3000.0f}) {
    for (u64 seed = 1; seed <= 5; ++seed) {
      if (radius > 2500.0f && seed > 2)
        continue;  // the large island twice is enough in a debug run
      const Plan plan = make_plan(make_params(seed, radius));
      Report report;
      validate_plan(plan, report);
      std::string rules;
      for (const Finding& f : report.findings)
        rules += std::string(rule_name(f.rule)) + " " + std::to_string(f.subject) + "; ";
      CHECK_MESSAGE(report.passed(), "radius " << radius << " seed " << seed << ": " << rules);
    }
  }
}

TEST_CASE("city plan: a tile's lots are the lots whose closed rectangle touches it") {
  const Plan& plan = small_plan();
  Vector<u32> got;
  for (const Lot& lot : plan.lots) {
    // Every lot is in its owner tile, and in each tile its corners are in.
    const TileCoord own = owner_tile(plan, lot);
    lots_in_tile(plan, own, got);
    const u32 index = static_cast<u32>(&lot - plan.lots.data());
    CHECK(std::find(got.begin(), got.end(), index) != got.end());
  }
  // And nothing else: a brute-force pass over a band of tiles.
  const i32 t = plan.params.tile_cm;
  for (i32 tz = -3; tz <= 3; ++tz) {
    for (i32 tx = -3; tx <= 3; ++tx) {
      const Rect r = tile_rect(plan, TileCoord{tx, tz});
      Vector<u32> want;
      for (u32 l = 0; l < plan.lots.size(); ++l) {
        const Rect& q = plan.lots[l].rect;
        if (q.x0 <= r.x1 && r.x0 <= q.x1 && q.z0 <= r.z1 && r.z0 <= q.z1) want.push_back(l);
      }
      lots_in_tile(plan, TileCoord{tx, tz}, got);
      CHECK(got == want);
    }
  }
  CHECK(t > 0);
}

TEST_CASE("city plan: a tile's content is the whole plan's content restricted to the tile") {
  const Plan& plan = small_plan();
  for (const Stage detail : {Stage::Massing, Stage::Floors}) {
    // The whole plan's content once: every building's proxies and the ground's.
    Vector<Proxy> whole;
    Grammar grammar(plan);
    Building b;
    std::string error;
    for (u32 l = 0; l < plan.lots.size(); ++l) {
      if (plan.lots[l].use == LotUse::Park) continue;
      REQUIRE_MESSAGE(grammar.generate(l, detail, b, &error), error);
      append_building_proxies(plan, b, detail, whole);
    }
    const Rect everything{-2000000000, -2000000000, 2000000000, 2000000000};
    append_ground_proxies(plan, everything, whole);
    // Every tile the plan touches, materialized alone.
    Rect bounds{0, 0, 0, 0};
    for (const Proxy& p : whole) {
      bounds.x0 = std::min(bounds.x0, p.x);
      bounds.z0 = std::min(bounds.z0, p.z);
      bounds.x1 = std::max(bounds.x1, p.x);
      bounds.z1 = std::max(bounds.z1, p.z);
    }
    const TileCoord lo = tile_of(plan, bounds.x0, bounds.z0);
    const TileCoord hi = tile_of(plan, bounds.x1, bounds.z1);
    u64 total = 0;
    u32 tiles = 0;
    Vector<Proxy> tile, restricted;
    for (i32 tz = lo.z; tz <= hi.z; ++tz) {
      for (i32 tx = lo.x; tx <= hi.x; ++tx) {
        REQUIRE(tile_proxies(plan, TileCoord{tx, tz}, detail, tile, &error));
        // Twice: materializing a tile again yields the same content.
        Vector<Proxy> again;
        REQUIRE(tile_proxies(plan, TileCoord{tx, tz}, detail, again, &error));
        CHECK(hash_proxies(again) == hash_proxies(tile));
        const Rect area = tile_rect(plan, TileCoord{tx, tz});
        restricted.clear();
        for (const Proxy& p : whole) {
          if (proxy_in(p, area)) restricted.push_back(p);
        }
        sort_proxies(tile);
        sort_proxies(restricted);
        CHECK_MESSAGE(
            hash_proxies(tile) == hash_proxies(restricted),
            "tile " << tx << "," << tz << ": " << tile.size() << " against " << restricted.size());
        total += tile.size();
        tiles += tile.empty() ? 0u : 1u;
      }
    }
    // And the tiles together are the whole, piece for piece.
    CHECK(total == whole.size());
    MESSAGE(std::string(stage_name(detail))
            << ": " << whole.size() << " proxies over " << tiles << " tiles");
  }
}

TEST_CASE("city plan: authored overrides are respected, and the rest is generated") {
  const Params base = make_params(k_golden_seed, 1200.0f);
  const Plan plain = make_plan(base);
  // Pick one of each thing to pin, apart from one another: a residential district; outside it, a
  // local street on a block's side, a building block for a park, and a lot of a third block that
  // street does not bound (a district pin renumbers what lies in its superblocks, and a street pin
  // re-cuts the lots along it).
  u32 district = k_no_id;
  for (const District& d : plain.districts) {
    if (d.kind == DistrictKind::Residential && district == k_no_id) district = d.id;
  }
  REQUIRE(district != k_no_id);
  auto elsewhere = [&](const Block& b) {
    return b.district != district && b.park == ParkKind::None && b.civic == CivicKind::None &&
           b.lot_count > 0;
  };
  u32 street = k_no_id, block = k_no_id, lot = k_no_id;
  for (const Block& b : plain.blocks) {
    if (!elsewhere(b) || street != k_no_id) continue;
    for (const u32 id : b.streets) {
      const u32 si = plain.find_street(id);
      if (si != k_no_id && plain.streets[si].cls == StreetClass::Local && street == k_no_id)
        street = id;
    }
    if (street != k_no_id) block = b.id;
  }
  REQUIRE(street != k_no_id);
  for (const Block& b : plain.blocks) {
    if (!elsewhere(b) || b.id == block || lot != k_no_id) continue;
    if (std::find(std::begin(b.streets), std::end(b.streets), street) != std::end(b.streets))
      continue;
    lot = plain.lots[b.first_lot].id;
  }
  REQUIRE(block != k_no_id);
  REQUIRE(lot != k_no_id);

  IslandParams source = base.source;
  auto pin = [&](OverrideKind what, u32 id) -> Override& {
    Override o;
    o.what = what;
    o.id = id;
    source.overrides.push_back(o);
    return source.overrides.back();
  };
  pin(OverrideKind::District, district).district_kind = DistrictKind::OldTown;
  pin(OverrideKind::Street, street).street_class = StreetClass::Pedestrian;
  pin(OverrideKind::Park, block).park = ParkKind::Square;
  pin(OverrideKind::Civic, lot).civic = CivicKind::Hospital;
  Params p;
  std::string error;
  REQUIRE_MESSAGE(params_from_schema(source, p, error), error);
  const Plan pinned = make_plan(p);
  CHECK(pinned.key != plain.key);
  CHECK(pinned.districts[district].kind == DistrictKind::OldTown);
  CHECK(pinned.districts[district].pinned == 1);
  const u32 s = pinned.find_street(street);
  REQUIRE(s != k_no_id);
  CHECK(pinned.streets[s].cls == StreetClass::Pedestrian);
  CHECK(pinned.streets[s].width_cm == pinned.params.street_width_cm[4]);
  const u32 b = pinned.find_block(block);
  REQUIRE(b != k_no_id);
  CHECK(pinned.blocks[b].park == ParkKind::Square);
  CHECK(pinned.blocks[b].lot_count == 0);
  const u32 l = pinned.find_lot(lot);
  REQUIRE(l != k_no_id);
  CHECK(pinned.lots[l].use == LotUse::Civic);
  CHECK(pinned.lots[l].civic == CivicKind::Hospital);
  CHECK(pinned.lots[l].archetype == Archetype::CivicShell);
  // Everything no pin reaches is what the rules made: the same districts elsewhere.
  for (u32 k = 0; k < plain.districts.size(); ++k) {
    if (k != district) CHECK(pinned.districts[k].kind == plain.districts[k].kind);
  }
  // A pin naming nothing is refused with a sentence.
  IslandParams bad = base.source;
  Override o;
  o.what = OverrideKind::Street;
  o.id = 0x7fffffffu;
  bad.overrides.push_back(o);
  REQUIRE(params_from_schema(bad, p, error));
  Plan refused;
  CHECK_FALSE(generate_plan(p, refused, error));
  CHECK(error.find("no street") != std::string::npos);
}

TEST_CASE("city plan: a park pin of None keeps the park rules off a block") {
  const Params base = make_params(k_golden_seed, 1200.0f);
  const Plan plain = make_plan(base);
  u32 park_block = k_no_id;
  for (const Block& b : plain.blocks) {
    if (b.park == ParkKind::District && park_block == k_no_id) park_block = b.id;
  }
  REQUIRE(park_block != k_no_id);
  IslandParams source = base.source;
  Override o;
  o.what = OverrideKind::Park;
  o.id = park_block;
  o.park = ParkKind::None;
  source.overrides.push_back(o);
  Params p;
  std::string error;
  REQUIRE(params_from_schema(source, p, error));
  const Plan pinned = make_plan(p);
  const u32 b = pinned.find_block(park_block);
  REQUIRE(b != k_no_id);
  CHECK(pinned.blocks[b].park == ParkKind::None);
  CHECK(pinned.blocks[b].lot_count > 0);
}

TEST_CASE("city plan: the plan file reads back to the same plan, and refuses a tampered one") {
  const engine::test::TempDir tmp("city_plan_file");
  const Plan& plan = small_plan();
  const std::string path = tmp.file("plan/plan.json");
  std::string error;
  REQUIRE_MESSAGE(write_plan_file(path, plan, &error), error);
  Plan back;
  REQUIRE_MESSAGE(read_plan_file(path, back, error), error);
  CHECK(hash_plan(back) == hash_plan(plan));
  CHECK(back.segments.size() == plan.segments.size());
  CHECK(back.tile_index.size() == plan.tile_index.size());
  // A lot moved by a centimetre no longer hashes to what the file says.
  PlanFile file;
  plan_to_schema(plan, file);
  file.lots[0].rect[0] += 1;
  CHECK_FALSE(plan_from_schema(file, back, error));
  CHECK(error.find("hash") != std::string::npos);
  // Another generator's plan is refused rather than trusted.
  plan_to_schema(plan, file);
  file.generator += 1;
  CHECK_FALSE(plan_from_schema(file, back, error));
  CHECK(error.find("generator") != std::string::npos);
}

TEST_CASE("city plan: the key is the parameters' and not their spelling") {
  // A file that leaves the tables empty and one that spells the defaults out are one island.
  IslandParams sparse;
  sparse.format = "engine.city-params.v1";
  sparse.name = "island";
  Params a, b;
  std::string error;
  REQUIRE_MESSAGE(params_from_schema(sparse, a, error), error);
  REQUIRE_MESSAGE(params_from_schema(default_island_params(), b, error), error);
  CHECK(plan_key(a.source) == plan_key(b.source));
  // A changed number is a changed key, and the cache directory follows it.
  IslandParams other = default_island_params();
  other.radius = 2100.0f;
  Params c;
  REQUIRE(params_from_schema(other, c, error));
  CHECK(plan_key(c.source) != plan_key(b.source));
  CHECK(plan_cache_dir("ddc", plan_key(b.source)).find("ddc/city/") == 0);
}

TEST_CASE("city params: out-of-range parameters are refused with a sentence naming them") {
  IslandParams p = default_island_params();
  Params out;
  std::string error;
  p.radius = 10.0f;
  CHECK_FALSE(params_from_schema(p, out, error));
  CHECK(error.find("radius") != std::string::npos);
  p = default_island_params();
  p.styles.push_back(default_style(DistrictKind::Downtown));
  CHECK_FALSE(params_from_schema(p, out, error));
  CHECK(error.find("styles") != std::string::npos);
  p = default_island_params();
  p.styles[0].floors_max = 0;
  CHECK_FALSE(params_from_schema(p, out, error));
  p = default_island_params();
  p.building.unit_depth_max = 1.0f;
  CHECK_FALSE(params_from_schema(p, out, error));
  CHECK(error.find("building") != std::string::npos);
  // And the defaults, written and read, are the defaults.
  const engine::test::TempDir tmp("city_params");
  REQUIRE(write_params_file(tmp.file("params.json"), default_island_params(), &error));
  Params read;
  REQUIRE_MESSAGE(read_params_file(tmp.file("params.json"), read, error), error);
  Params direct;
  REQUIRE(params_from_schema(default_island_params(), direct, error));
  CHECK(plan_key(read.source) == plan_key(direct.source));
  CHECK(count_if_lots(small_plan(), [](const Lot& l) { return l.use == LotUse::Building; }) > 0);
}
