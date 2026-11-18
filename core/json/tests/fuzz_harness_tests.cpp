// The hostile-input harness's own tests (roadmap T13, stage 0; docs/ci/fuzzing.md, "What the
// harness's own tests pin"). They live with the first target's module because the harness is test
// support, not a module, and `core/json` is the lowest module it needs.
//
// Two pins carry the stage: a planted target that asserts on one input fails its case, the input
// named and kept (the harness sees an assert); and the same seed gives the same inputs on every
// compiler (a hash of the first thousand). The planted targets that end a process run in a child
// of this executable, so this one survives to check what the child said.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/memory/memory.h>

#include <doctest/doctest.h>
#include <fuzz_runner.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <chrono>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>

using namespace engine;
using namespace engine::test::fuzz;

namespace {

bool is_planted(std::span<const u8> input) {
  return input.size() == 7 && std::memcmp(input.data(), "planted", 7) == 0;
}

Verdict planted_assert(std::span<const u8> input) {
  ENGINE_ASSERT(!is_planted(input), "the planted input: the harness must see this assert");
  return {};
}

Verdict planted_contract(std::span<const u8> input) {
  if (is_planted(input)) return Verdict::broken("the planted contract breaks on this input");
  return {};
}

Verdict planted_allocation(std::span<const u8> input) {
  if (is_planted(input)) {
    constexpr usize k_bytes = usize{4} << 20;  // past the cap's 1 MiB floor
    void* p = mem::allocate(k_bytes, 16);
    mem::deallocate(p, k_bytes, 16);
  }
  return {};
}

Verdict planted_hang(std::span<const u8> input) {
  while (is_planted(input))
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return {};
}

constexpr std::string_view k_planted_seed[] = {"planted"};

constexpr TargetSpec planted(const char* name, TargetFn one) {
  TargetSpec spec;
  spec.name = name;
  spec.one = one;
  spec.byte_iterations = 1;  // the seed itself, nothing else
  spec.json_iterations = 0;
  spec.watchdog_s = 1;
  spec.builtin_seeds = std::span<const std::string_view>(k_planted_seed);
  spec.has_content = false;
  return spec;
}

constexpr TargetSpec k_planted_assert = planted("planted_assert", &planted_assert);
constexpr TargetSpec k_planted_contract = planted("planted_contract", &planted_contract);
constexpr TargetSpec k_planted_allocation = planted("planted_allocation", &planted_allocation);
constexpr TargetSpec k_planted_hang = planted("planted_hang", &planted_hang);

std::string kept_bytes(const engine::test::TempDir& dir, const char* target) {
  Bytes bytes;
  const std::string path = dir.file(std::string(target) + "-builtin-0_0.input");
  if (!test::fuzz::detail::read_bytes(std::filesystem::path(path), bytes)) return "(not kept)";
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A child the harness ended with `std::_Exit` names every directory of its test main's that it
// removed first; each has to be gone, under the same scratch root as this case's own, and the run's
// own directory has to be among them, so the child left the root nothing.
void check_child_left_nothing(const ChildRun& child, const engine::test::TempDir& scratch) {
  constexpr std::string_view k_line = "fuzz: removed this process's test scratch ";
  const std::filesystem::path root = scratch.native().parent_path();
  u32 named = 0;
  for (usize at = child.output.find(k_line); at != std::string::npos;
       at = child.output.find(k_line, at + 1)) {
    const usize from = at + k_line.size();
    usize to = child.output.find_first_of("\r\n", from);
    if (to == std::string::npos) to = child.output.size();
    const std::string dir = child.output.substr(from, to - from);
    ++named;
    CHECK_MESSAGE(!test::path_exists(dir), dir << " is still there");
    CHECK(std::filesystem::path(dir).parent_path().lexically_normal() == root.lexically_normal());
  }
  CHECK_MESSAGE(named >= 1, "the child named no scratch of its own as removed");
}

std::string hex(u64 v) {
  static constexpr char k_digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (u32 i = 0; i < 16; ++i)
    out[15 - i] = k_digits[(v >> (4 * i)) & 0xf];
  return out;
}

// Two seeds for the pin: one JSON document, which the JSON layer works on, and one line of text
// that is not JSON, which the JSON layer hands to the byte layer.
constexpr std::string_view k_pin_json =
    R"({"id": "0123456789abcdef0123456789abcdef", "n": [0, 1, -1, 2.5, 1e21], "s": "x"})";
constexpr std::string_view k_pin_text = "not json: 1, 2, 3";

// The hash of the first thousand inputs of `k_pin_json` and `k_pin_text`, taken on MSVC
// (msvc-debug) when the harness landed; GCC and Clang have not run it yet (docs/ci/fuzzing.md,
// "What the harness's own tests pin").
constexpr u64 k_first_thousand_inputs = 0x0000000000000000ull;

}  // namespace

// The planted targets. Skipped in a normal run; the cases below start them in a child process.
TEST_CASE("fuzz: planted_assert" * doctest::skip()) { run_target(k_planted_assert); }
TEST_CASE("fuzz: planted_contract" * doctest::skip()) { run_target(k_planted_contract); }
TEST_CASE("fuzz: planted_allocation" * doctest::skip()) { run_target(k_planted_allocation); }
TEST_CASE("fuzz: planted_hang" * doctest::skip()) { run_target(k_planted_hang); }

TEST_CASE("fuzz harness: SplitMix64 gives the reference implementation's outputs") {
  u64 state = 0;
  CHECK(splitmix64(state) == 0xe220a8397b1dcdafull);
  CHECK(splitmix64(state) == 0x6e789e6aa1b965f4ull);
  CHECK(splitmix64(state) == 0x06c45d188009454full);
}

TEST_CASE("fuzz harness: an input is a function of its target, seed and iteration") {
  const Seed seeds[] = {make_seed("a", k_pin_json), make_seed("b", k_pin_text)};
  TargetSpec spec;
  spec.name = "pin";
  spec.max_len = 256;
  spec.byte_iterations = 64;
  spec.json_iterations = 64;
  CHECK(generate(spec, seeds, 0, 0) == seeds[0].bytes);  // iteration 0 is the seed itself
  u32 differs = 0;
  for (u32 i = 1; i < iterations_per_seed(spec); ++i) {
    const Bytes once = generate(spec, seeds, 0, i);
    CHECK(once == generate(spec, seeds, 0, i));
    CHECK(once.size() <= spec.max_len);
    if (once != seeds[0].bytes) ++differs;
  }
  CHECK(differs > iterations_per_seed(spec) / 2);
  TargetSpec other = spec;
  other.name = "another";
  CHECK(generate(spec, seeds, 0, 5) != generate(other, seeds, 0, 5));

  Rng rng(7);
  for (u32 i = 0; i < 1000; ++i) {
    const u64 n = 1 + (i % 13);
    CHECK(rng.below(n) < n);
  }
}

TEST_CASE("fuzz harness: the same seed gives the same inputs on every compiler") {
  const Seed seeds[] = {make_seed("pin-json", k_pin_json), make_seed("pin-text", k_pin_text)};
  TargetSpec spec;
  spec.name = "pin";
  spec.byte_iterations = 300;
  spec.json_iterations = 200;
  u64 h = k_fnv_offset;
  u32 inputs = 0;
  for (usize s = 0; s < 2; ++s) {
    for (u32 i = 0; i < iterations_per_seed(spec); ++i) {
      h = hash_input(generate(spec, seeds, s, i), h);
      ++inputs;
    }
  }
  REQUIRE(inputs == 1000);
  MESSAGE("fuzz harness: the first thousand inputs hash to 0x" << hex(h));
  CHECK(h == k_first_thousand_inputs);
}

TEST_CASE("fuzz harness: a planted target that asserts on one input fails its case") {
#if ENGINE_DEBUG
  engine::test::TempDir scratch("fuzz-planted-assert");
  const ChildRun child = run_child_case("fuzz: planted_assert", scratch.path(), "");
  INFO(child.output);
  CHECK(child.code == k_exit_assert);
  CHECK(child.output.find("fuzz finding: planted_assert builtin-0#0: ENGINE_ASSERT(") !=
        std::string::npos);
  CHECK(kept_bytes(scratch, "planted_assert") == "planted");
  check_child_left_nothing(child, scratch);
#else
  MESSAGE("ENGINE_ASSERT is compiled out of this configuration: no assert to plant");
#endif
}

TEST_CASE("fuzz harness: a broken contract and an allocation past the cap fail their cases") {
  engine::test::TempDir scratch("fuzz-planted");
  const ChildRun contract = run_child_case("fuzz: planted_contract", scratch.path(), "");
  CHECK_MESSAGE(contract.code != 0, contract.output);
  CHECK(contract.output.find("fuzz finding: planted_contract builtin-0#0: the planted contract") !=
        std::string::npos);
  CHECK(kept_bytes(scratch, "planted_contract") == "planted");

  const ChildRun allocation = run_child_case("fuzz: planted_allocation", scratch.path(), "");
  CHECK_MESSAGE(allocation.code != 0, allocation.output);
  CHECK(allocation.output.find("fuzz finding: planted_allocation builtin-0#0: held ") !=
        std::string::npos);
  CHECK(kept_bytes(scratch, "planted_allocation") == "planted");
}

TEST_CASE("fuzz harness: the watchdog names a hung input") {
  engine::test::TempDir scratch("fuzz-planted-hang");
  const ChildRun child = run_child_case("fuzz: planted_hang", scratch.path(), "");
  INFO(child.output);
  CHECK(child.code == k_exit_hang);
  CHECK(child.output.find("fuzz finding: planted_hang builtin-0#0: still running") !=
        std::string::npos);
  CHECK(kept_bytes(scratch, "planted_hang") == "planted");
  check_child_left_nothing(child, scratch);
}

TEST_CASE("fuzz harness: a replay runs the one input it names") {
  engine::test::TempDir scratch("fuzz-replay");
  const ChildRun child =
      run_child_case("fuzz: planted_contract", scratch.path(), "planted_contract|builtin-0#0");
  CHECK_MESSAGE(child.code != 0, child.output);
  CHECK(child.output.find("fuzz finding: planted_contract builtin-0#0") != std::string::npos);
  const ChildRun elsewhere =
      run_child_case("fuzz: planted_contract", scratch.path(), "planted_contract|builtin-0#7");
  CHECK_MESSAGE(elsewhere.code != 0, elsewhere.output);  // no such input: refused, not passed
  CHECK(elsewhere.output.find("names no input") != std::string::npos);
}
