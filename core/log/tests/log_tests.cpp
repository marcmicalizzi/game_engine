#include <core/ids/id128.h>
#include <core/log/log.h>
#include <core/platform/thread.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::log;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(test_alpha, "test.alpha");
ENGINE_LOG_CATEGORY_DEFINE(test_beta, "test.beta");

// Registers a ring sink for the test's duration and restores level state afterwards.
struct Capture {
  RingSink ring;
  explicit Capture(u32 capacity = 64) : ring(capacity) {
    reset_levels();
    set_global_min_level(Level::Trace);
    add_sink(&ring);
  }
  ~Capture() {
    remove_sink(&ring);
    reset_levels();
  }
  ENGINE_NON_COPYABLE(Capture);

  // Copies the retained entries' scalar parts and strings for inspection outside the lock.
  struct Snapshot {
    u64 sequence;
    Level level;
    const Category* category;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;  // key -> rendered value (JSON)
    u32 thread_index;
    u32 line;
  };
  std::vector<Snapshot> entries(u64 since = 0) const {
    std::vector<Snapshot> out;
    ring.for_each(since, [&](const RingSink::Entry& e) {
      Snapshot s{e.sequence, e.level,        e.category, std::string(e.message),
                 {},         e.thread_index, e.line};
      for (const Field& f : e.fields) {
        Record r{};
        r.category = e.category;
        r.fields = std::span<const Field>{&f, 1};
        std::string json;
        format_json_line(r, json);
        const auto at = json.find("\"fields\":{");
        std::string value = json.substr(at + 10);
        value = value.substr(value.find(':') + 1);
        value.resize(value.size() - 2);  // strip "}}"
        s.fields.emplace_back(std::string(f.key), value);
      }
      out.push_back(std::move(s));
    });
    return out;
  }
};

Record sample_record(std::span<const Field> fields, Level level = Level::Info) {
  Record r{};
  r.monotonic_ns = 5;
  r.wall_unix_us = 1'700'000'000'123'456;  // 2023-11-14 22:13:20.123456 UTC
  r.category = &test_alpha;
  r.message = "hello";
  r.fields = fields;
  r.file = "a.cpp";
  r.line = 12;
  r.thread_index = 7;
  r.level = level;
  return r;
}

}  // namespace

TEST_CASE("log: level names round-trip and parse leniently") {
  for (int i = 0; i <= 6; ++i) {
    const auto level = static_cast<Level>(i);
    Level parsed{};
    CHECK(parse_level(level_name(level), parsed));
    CHECK(parsed == level);
  }
  Level l{};
  CHECK(parse_level("WARNING", l));
  CHECK(l == Level::Warn);
  CHECK(parse_level("Err", l));
  CHECK(l == Level::Error);
  CHECK(parse_level("none", l));
  CHECK(l == Level::Off);
  CHECK_FALSE(parse_level("loud", l));
  CHECK_FALSE(parse_level("", l));
  CHECK_FALSE(parse_level("informational", l));
}

TEST_CASE("log: categories register at static initialization and are found by name") {
  CHECK(find_category("test.alpha") == &test_alpha);
  CHECK(find_category("test.beta") == &test_beta);
  CHECK(find_category("test.gamma") == nullptr);
  bool saw_alpha = false;
  for (const Category* c = first_category(); c != nullptr; c = c->next()) {
    if (c == &test_alpha) saw_alpha = true;
  }
  CHECK(saw_alpha);
}

TEST_CASE("log: level specs apply to existing and later categories") {
  reset_levels();
  std::string error;
  CHECK(apply_level_spec(" warn , test.alpha=trace, test.b*=error", &error));
  CHECK(error.empty());
  CHECK(global_min_level() == Level::Warn);
  CHECK(test_alpha.min_level() == Level::Trace);
  CHECK(test_beta.min_level() == Level::Error);
  // The global floor applies on top of the category level.
  CHECK_FALSE(test_alpha.enabled(Level::Info));
  CHECK(test_alpha.enabled(Level::Warn));
  CHECK_FALSE(test_beta.enabled(Level::Warn));
  CHECK(test_beta.enabled(Level::Error));

  {
    Category late{"test.brand_new"};
    CHECK(late.min_level() == Level::Error);  // matched test.b* at registration
    CHECK(find_category("test.brand_new") == &late);
  }
  CHECK(find_category("test.brand_new") == nullptr);  // unregistered on destruction

  CHECK_FALSE(apply_level_spec("test.alpha=loud", &error));
  CHECK(error.find("loud") != std::string::npos);
  CHECK_FALSE(apply_level_spec("=info", &error));

  reset_levels();
  CHECK(test_alpha.min_level() == Level::Trace);
  CHECK(test_beta.min_level() == Level::Trace);
  CHECK(global_min_level() == (ENGINE_DEBUG ? Level::Debug : Level::Info));
  Category later{"test.b_after_reset"};
  CHECK(later.min_level() == Level::Trace);
}

TEST_CASE("log: records carry typed fields into the ring sink") {
  Capture cap;
  const Id128 id = Id128::from_parts(1, 2);
  const std::string owned = "two words";
  ENGINE_LOG_INFO(test_alpha, "hello", field("count", 3), field("ratio", 0.5), field("name", owned),
                  field("flag", true), field("neg", -4), field("big", u64{18446744073709551615ull}),
                  field("id", id), field_pointer("p", reinterpret_cast<const void*>(0x1234)),
                  field("lit", "x"));
  ENGINE_LOG_WARN(test_beta, "no fields");

  const auto entries = cap.entries();
  REQUIRE(entries.size() == 2);
  const auto& a = entries[0];
  CHECK(a.sequence == 0);
  CHECK(a.level == Level::Info);
  CHECK(a.category == &test_alpha);
  CHECK(a.message == "hello");
  CHECK(a.thread_index == platform::current_thread_index());
  CHECK(a.line > 0);
  REQUIRE(a.fields.size() == 9);
  CHECK(a.fields[0] == std::pair<std::string, std::string>{"count", "3"});
  CHECK(a.fields[1] == std::pair<std::string, std::string>{"ratio", "0.5"});
  CHECK(a.fields[2] == std::pair<std::string, std::string>{"name", "\"two words\""});
  CHECK(a.fields[3] == std::pair<std::string, std::string>{"flag", "true"});
  CHECK(a.fields[4] == std::pair<std::string, std::string>{"neg", "-4"});
  CHECK(a.fields[5] == std::pair<std::string, std::string>{"big", "18446744073709551615"});
  CHECK(a.fields[6] ==
        std::pair<std::string, std::string>{"id", "\"00000000000000010000000000000002\""});
  CHECK(a.fields[7] == std::pair<std::string, std::string>{"p", "\"0x0000000000001234\""});
  CHECK(a.fields[8] == std::pair<std::string, std::string>{"lit", "\"x\""});
  CHECK(entries[1].sequence == 1);
  CHECK(entries[1].level == Level::Warn);
  CHECK(entries[1].message == "no fields");
  CHECK(entries[1].fields.empty());
  CHECK(sink_count() == 1);
}

TEST_CASE("log: disabled records evaluate no field arguments") {
  Capture cap;
  int evaluations = 0;
  test_alpha.set_min_level(Level::Warn);
  ENGINE_LOG_INFO(test_alpha, "skipped", field("n", ++evaluations));
  CHECK(evaluations == 0);
  CHECK(cap.ring.size() == 0);
  ENGINE_LOG_ERROR(test_alpha, "kept", field("n", ++evaluations));
  CHECK(evaluations == 1);
  CHECK(cap.ring.size() == 1);
  set_global_min_level(Level::Off);
  ENGINE_LOG_ERROR(test_alpha, "off", field("n", ++evaluations));
  CHECK(evaluations == 1);
  CHECK(cap.ring.size() == 1);
}

TEST_CASE("log: per-sink levels filter independently of categories") {
  Capture cap;
  RingSink warnings(8);
  warnings.set_min_level(Level::Warn);
  add_sink(&warnings);
  ENGINE_LOG_INFO(test_alpha, "info");
  ENGINE_LOG_WARN(test_alpha, "warn");
  remove_sink(&warnings);
  CHECK(cap.ring.size() == 2);
  CHECK(warnings.size() == 1);
  warnings.for_each(0, [](const RingSink::Entry& e) { CHECK(e.message == "warn"); });
  CHECK(sink_count() == 1);
}

TEST_CASE("log: ring sink evicts oldest and answers since-queries") {
  Capture cap(4);
  for (int i = 0; i < 10; ++i)
    ENGINE_LOG_DEBUG(test_alpha, "n", field("i", i));
  CHECK(cap.ring.size() == 4);
  CHECK(cap.ring.capacity() == 4);
  CHECK(cap.ring.next_sequence() == 10);
  CHECK(cap.ring.first_sequence() == 6);
  auto all = cap.entries();
  REQUIRE(all.size() == 4);
  CHECK(all.front().fields[0].second == "6");
  CHECK(all.back().fields[0].second == "9");
  CHECK(cap.entries(8).size() == 2);
  CHECK(cap.entries(10).empty());
  cap.ring.clear();
  CHECK(cap.ring.size() == 0);
  CHECK(cap.ring.first_sequence() == 10);
  ENGINE_LOG_DEBUG(test_alpha, "after clear");
  CHECK(cap.ring.first_sequence() == 10);
  CHECK(cap.ring.next_sequence() == 11);
}

TEST_CASE("log: text format is pinned") {
  const std::string owned = "two words";
  const Field fields[] = {field("count", 3),
                          field("ratio", 0.5),
                          field("name", owned),
                          field("flag", true),
                          field("empty", ""),
                          field("q", "a\"b"),
                          field("id", Id128::from_parts(1, 2)),
                          field_pointer("p", reinterpret_cast<const void*>(0x1234)),
                          field("nan", std::numeric_limits<f64>::quiet_NaN())};
  std::string out;
  format_text(sample_record(fields), out);
  CHECK(out ==
        "22:13:20.123 info  test.alpha [t7] hello count=3 ratio=0.5 name=\"two words\" "
        "flag=true empty=\"\" q=\"a\\\"b\" id=00000000000000010000000000000002 "
        "p=0x0000000000001234 nan=nan");
  out.clear();
  format_text(sample_record({}, Level::Error), out);
  CHECK(out == "22:13:20.123 error test.alpha [t7] hello");
}

TEST_CASE("log: JSON line format is pinned and escapes") {
  const Field fields[] = {field("count", 3),
                          field("ratio", 0.5),
                          field("name", "two words"),
                          field("flag", true),
                          field("neg", -4),
                          field("big", u64{18446744073709551615ull}),
                          field("nan", std::numeric_limits<f64>::quiet_NaN()),
                          field("ctl", std::string_view{"a\x01\n\t", 4}),
                          field("id", Id128::from_parts(1, 2)),
                          field_pointer("p", reinterpret_cast<const void*>(0x1234))};
  Record r = sample_record(fields, Level::Warn);
  r.message = "he\"llo\n";
  std::string out;
  format_json_line(r, out);
  CHECK(out ==
        "{\"t\":5,\"wall\":1700000000123456,\"level\":\"warn\",\"cat\":\"test.alpha\","
        "\"thread\":7,\"msg\":\"he\\\"llo\\n\",\"file\":\"a.cpp\",\"line\":12,\"fields\":{"
        "\"count\":3,\"ratio\":0.5,\"name\":\"two words\",\"flag\":true,\"neg\":-4,"
        "\"big\":18446744073709551615,\"nan\":null,\"ctl\":\"a\\u0001\\n\\t\","
        "\"id\":\"00000000000000010000000000000002\",\"p\":\"0x0000000000001234\"}}");
  // No file, no fields: both keys are omitted.
  Record bare = sample_record({});
  bare.file = nullptr;
  out.clear();
  format_json_line(bare, out);
  CHECK(out ==
        "{\"t\":5,\"wall\":1700000000123456,\"level\":\"info\",\"cat\":\"test.alpha\","
        "\"thread\":7,\"msg\":\"hello\"}");
}

TEST_CASE("log: stream sink writes one JSON line per record to a file") {
  Capture cap;
  const test::TempDir tmp("engine_log");
  const std::string path = tmp.file("stream_sink.jsonl");
  {
    std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
    (void)fopen_s(&f, path.c_str(), "wb");
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    REQUIRE(f != nullptr);
    StreamSink file_sink(f, StreamSink::Format::JsonLines, /*close_on_destroy=*/true);
    add_sink(&file_sink);
    ENGINE_LOG_INFO(test_alpha, "one", field("k", 1));
    ENGINE_LOG_ERROR(test_beta, "two", field("k", 2));
    remove_sink(&file_sink);
  }
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);)
    lines.push_back(line);
  in.close();
  REQUIRE(lines.size() == 2);
  CHECK(lines[0].find("\"msg\":\"one\"") != std::string::npos);
  CHECK(lines[0].find("\"fields\":{\"k\":1}") != std::string::npos);
  CHECK(lines[0].back() == '}');
  CHECK(lines[1].find("\"level\":\"error\"") != std::string::npos);
  CHECK(lines[1].find("\"cat\":\"test.beta\"") != std::string::npos);
  CHECK(cap.ring.size() == 2);
}

TEST_CASE("log: concurrent emitters produce contiguous sequences") {
  Capture cap(4096);
  constexpr int k_threads = 8;
  constexpr int k_per_thread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < k_threads; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < k_per_thread; ++i) {
        ENGINE_LOG_DEBUG(test_alpha, "burst", field("t", t), field("i", i));
      }
    });
  }
  for (auto& th : threads)
    th.join();
  CHECK(cap.ring.size() == k_threads * k_per_thread);
  std::vector<u64> sequences;
  cap.ring.for_each(0, [&](const RingSink::Entry& e) { sequences.push_back(e.sequence); });
  std::sort(sequences.begin(), sequences.end());
  for (usize i = 0; i < sequences.size(); ++i)
    CHECK(sequences[i] == i);
}

TEST_CASE("log: compile-time floor is within range") {
  static_assert(ENGINE_LOG_MIN_LEVEL >= 0 && ENGINE_LOG_MIN_LEVEL <= 5);
  CHECK(true);
}
