// End to end: the Phase 3 exit's last clause (docs/plan/10-roadmap-risks.md §10, Phase 3; npc.md,
// "Held to the clause") — a tile-streamed world with 10^5 scheduled residents, saved partway,
// loaded, and replayed from tick 0 to one persistent-state hash.
//
// The world is the generator's (systems/npc/generator.h), written into the test's scratch
// directory, never committed: 10^5 `engine.npc.Resident` records and their 30,500 places on a
// 512 m square of 64 m tiles, with the residents' routine clock at 07:00 at game time 0, so the
// first game seconds are the morning's first commutes rather than a sleeping city. It is streamed
// round two observers — one walking east at 20 m/s, so tiles go and residents leave with them, and
// one still — under rings that simulate every tile within 384 m, so every resident starts on the
// wheel.
//
// Three runs to one tick, as the replay test of save_tests.cpp:
//
//   (a) continuous  one engine-host runs 300 ticks, saves, runs 300 more
//   (b) loaded      a fresh engine-host loads the save and runs the same 300
//   (c) replayed    a third runs a copy of the starting document from tick 0 to 600 in one call
//
// The persistent-state hash covers the residents' state: the document part is every composed
// record, `state`, `next_event` and `position` among its properties, and the store's events are the
// write-backs of the transitions.
#if defined(ENGINE_CLI_TESTS_WORLD) && defined(ENGINE_CLI_TESTS_NPC)

#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>
#include <systems/npc/generator.h>
#include <systems/npc/routine.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace engine;

namespace {

const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
}

struct Host {
  platform::Process process;
  u32 next_id = 1;
  bool ok = false;

  Host() {
    const std::string_view argv[2] = {host_exe(), "--stdio"};
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv, 2), &error)) {
      FAIL("cannot spawn engine-host: " << error);
      return;
    }
    ok = true;
  }
  ~Host() {
    if (!ok) return;
    process.close_stdin();
    (void)process.wait();
  }

  JsonValue call(std::string_view method, std::string_view params) {
    JsonValue response;
    const std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id++) +
                             ",\"method\":\"" + std::string(method) +
                             "\",\"params\":" + std::string(params) + "}\n";
    if (!process.write(line)) {
      FAIL("cannot write to engine-host");
      return response;
    }
    std::string text;
    if (!process.read_line(text)) {
      FAIL("engine-host closed the connection");
      return response;
    }
    REQUIRE_MESSAGE(parse_json(text, response).ok, text);
    return response;
  }
};

std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "missing '" << std::string(key) << "'");
  return *v;
}

const JsonValue& result_of(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  REQUIRE_MESSAGE(error == nullptr,
                  (error != nullptr ? write_json(*error, JsonWriteOptions{}) : std::string()));
  return at(response, "result");
}

u64 number(const JsonValue& o, std::string_view key) {
  u64 out = 0;
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_u64(out) ? out : ~u64{0};
}

std::string text(const JsonValue& o, std::string_view key) {
  std::string_view out;
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_string(out) ? std::string(out) : std::string();
}

std::string open_session(Host& host, const std::string& dir) {
  const JsonValue opened = host.call("session.open", "{\"path\":" + json_string(dir) + "}");
  return text(result_of(opened), "session");
}

constexpr u32 k_residents = 100'000;

// The generator's parameters: the host's world seed (1, until a document carries one), 64 m tiles,
// and the routines' clock five seconds before 07:00 at game time 0, so the seven o'clock minute — where many
// rows start — falls inside the first run and its transitions (timers fired, records moved, state
// written back) are part of what every run replays; at 07:00 exactly nothing transitions in ten seconds.
npc::GeneratorParams resident_world(u32 residents) {
  npc::GeneratorParams p;
  p.seed = 1;
  p.residents = residents;
  p.min_x = -256.0;
  p.min_z = -256.0;
  p.max_x = 256.0;
  p.max_z = 256.0;
  p.tile_size = 64.0;
  p.clock_offset_us = 7 * 60 * npc::k_us_per_minute - 5 * 1'000'000;
  return p;
}

void make_world(const std::string& dir, u32 residents) {
  io::Vfs vfs;
  std::string error;
  npc::GeneratorStats stats;
  REQUIRE_MESSAGE(
      npc::generate_document(vfs, dir, resident_world(residents), nullptr, error, &stats), error);
  MESSAGE("generated " << stats.residents << " residents and " << stats.places << " places in "
                       << stats.tiles << " tiles, " << stats.ms << " ms");
}

// Rings of 2 and 6 tiles (128 and 384 m), both simulated: every resident of the 512 m square is
// within the outer ring of the still observer at the centre, so every one of them is materialized
// and scheduled; the walker takes tiles east of the square in and lets the west go.
std::string stream_params() {
  return ",\"stream\":{\"tile_size\":64,\"rings\":[2,6],\"simulated\":2,\"observers\":["
         "{\"position\":[-200.0,1.7,0.0],\"velocity\":[20.0,0.0,0.0]},"
         "{\"position\":[0.0,1.7,0.0],\"velocity\":[0.0,0.0,0.0]}]}";
}

struct Parts {
  std::string value;
  JsonValue whole;
};

Parts state_hash(Host& host, const std::string& session) {
  const JsonValue r = host.call("session.state_hash", "{\"session\":\"" + session + "\"}");
  Parts h;
  h.whole = result_of(r);
  h.value = text(h.whole, "hash");
  return h;
}

void same_parts(const Parts& a, const Parts& b) {
  for (const char* part : {"hash", "clock", "document", "events", "projections", "snapshots"}) {
    CAPTURE(part);
    CHECK(text(a.whole, part) == text(b.whole, part));
  }
  for (const char* count : {"records", "event_count", "projection_count", "snapshot_count"}) {
    CAPTURE(count);
    CHECK(number(a.whole, count) == number(b.whole, count));
  }
}

// The number the three runs reach at tick 600: pinned from the first run on the owner's machine
// (MSVC, 2026-09-25) and reproduced by Clang 18 and GCC 13 in the Linux container and by GCC 14 at
// the v2 baseline on the server. A change to the residents' routines, the generator, the wheel, the
// document consumer or the store moves it; re-pin only when the change is the point of the commit.
constexpr const char* k_pinned = "53a8c6295e6d4e78";

}  // namespace

TEST_CASE("npc: 10^5 scheduled residents streamed, saved, loaded and replayed to one state hash") {
  const test::TempDir tmp("cli_npc_replay");
  REQUIRE(tmp.ok());
  const std::string start = tmp.file("start");
  make_world(start, k_residents);
  const std::string continuous = tmp.file("continuous");
  const std::string replayed = tmp.file("replayed");
  std::filesystem::copy(start, continuous, std::filesystem::copy_options::recursive);
  std::filesystem::copy(start, replayed, std::filesystem::copy_options::recursive);
  const std::string save = tmp.file("save-at-300");

  // (a) Continuous.
  Parts a;
  Parts at_save;
  {
    Host host;
    REQUIRE(host.ok);
    const std::string session = open_session(host, continuous);
    const std::string s = "{\"session\":\"" + session + "\"";
    const JsonValue first =
        host.call("session.run_headless", s + ",\"seconds\":5" + stream_params() + "}");
    const JsonValue& run = result_of(first);
    CHECK(number(run, "tick") == 300);
    const JsonValue& streamed = at(run, "streamed");
    // Every resident and every place within reach came in, at the first fill.
    CHECK(number(streamed, "created") >= k_residents);
    CHECK(number(streamed, "deactivated") > 0);
    MESSAGE("first 300 ticks: "
                                << number(streamed, "created") << " created, document "
                                << write_json(at(streamed, "document_ms"), JsonWriteOptions{})
                                << " ms, store "
                                << write_json(at(streamed, "store_ms"), JsonWriteOptions{})
                                << " ms");
    at_save = state_hash(host, session);
    const JsonValue saved =
        host.call("session.save_game", s + ",\"path\":" + json_string(save) + "}");
    CHECK(text(result_of(saved), "state_hash") == at_save.value);
    u64 bytes = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(save)) {
      if (e.is_regular_file()) bytes += e.file_size();
    }
    MESSAGE("save at tick 300: " << bytes << " bytes");
    const JsonValue second = host.call("session.run_headless", s + ",\"seconds\":5}");
    CHECK(number(result_of(second), "tick") == 600);
    a = state_hash(host, session);
  }
  // The transitions reached the store's log as write-back events, one per resident that moved on.
  CHECK(number(a.whole, "event_count") > 0);
  CHECK(number(a.whole, "records") >= k_residents);

  // (b) Loaded.
  Parts b;
  {
    Host host;
    REQUIRE(host.ok);
    const JsonValue loaded =
        host.call("session.load_game", "{\"path\":" + json_string(save) +
                                           ",\"dir\":" + json_string(tmp.file("loaded")) + "}");
    const JsonValue& load = result_of(loaded);
    CHECK(text(load, "state_hash") == at_save.value);
    const std::string session = text(load, "session");
    const JsonValue run =
        host.call("session.run_headless", "{\"session\":\"" + session + "\",\"seconds\":5}");
    CHECK(number(result_of(run), "tick") == 600);
    b = state_hash(host, session);
  }

  // (c) Replayed from tick 0.
  Parts c;
  {
    Host host;
    REQUIRE(host.ok);
    const std::string session = open_session(host, replayed);
    const JsonValue run =
        host.call("session.run_headless",
                  "{\"session\":\"" + session + "\",\"seconds\":10" + stream_params() + "}");
    CHECK(number(result_of(run), "tick") == 600);
    c = state_hash(host, session);
  }
  MESSAGE("continuous " << a.value << ", loaded " << b.value << ", replayed " << c.value);
  same_parts(a, b);
  same_parts(a, c);
  CHECK(a.value == b.value);
  CHECK(a.value == c.value);
  CHECK(a.value == std::string(k_pinned));
}

// The migration corpus's resident save (content/migration-corpus/README.md, "Adding a version"):
// a small resident world, streamed 300 ticks and saved, into `ENGINE_SAVE_CORPUS_NPC_OUT` when that
// is set. Without the variable it does nothing, so the suite never writes into the repository.
TEST_CASE("save corpus: write the resident save when asked") {
  const std::string out = test::detail::environment("ENGINE_SAVE_CORPUS_NPC_OUT");
  if (out.empty()) return;
  const test::TempDir tmp("cli_npc_corpus_write");
  REQUIRE(tmp.ok());
  const std::string doc = tmp.file("doc");
  make_world(doc, 400);
  Host host;
  REQUIRE(host.ok);
  const std::string s = "{\"session\":\"" + open_session(host, doc) + "\"";
  REQUIRE(number(result_of(host.call("session.run_headless",
                                     s + ",\"seconds\":5" + stream_params() + "}")),
                 "tick") == 300);
  const JsonValue saved = host.call("session.save_game", s + ",\"path\":" + json_string(out) + "}");
  MESSAGE("wrote " << out << ": " << write_json(result_of(saved), JsonWriteOptions{false}));
}

#endif
