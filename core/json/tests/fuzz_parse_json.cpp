// The hostile-input target for `parse_json` (roadmap T13, stage 0; docs/ci/fuzzing.md). Every
// message, file and parameter the engine reads goes through this parser first
// (docs/experiments/fuzzing-design.md, "The protocol's front door"), so it is the first target.
//
// The contract it holds every input to:
//   - a refusal leaves the value null, says why in a message that is not empty, and points at a
//     line and a column (1-based) and an offset inside the input;
//   - an accepted input, written back by the canonical writer, pretty and compact, parses again to
//     an equal value, and that value writes the same text again: an accepted document survives its
//     own writing, which is what the authoring document and the protocol rely on.
//
// Compiled twice by engine_module_fuzz() (cmake/EngineFuzz.cmake): into engine_json_tests as the
// doctest case "fuzz: parse_json", and, under the linux-clang-fuzz preset, into
// engine_fuzz_parse_json as libFuzzer's entry point.

#include <core/json/json.h>

#include <fuzz_target.h>

#include <span>
#include <string>
#include <string_view>

namespace {

using engine::JsonParseResult;
using engine::JsonValue;
using engine::JsonWriteOptions;
using engine::test::fuzz::Verdict;

Verdict fuzz_parse_json(std::span<const engine::u8> input) {
  const std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
  JsonValue value(engine::i64{99});  // not null, so a refusal that leaves it alone is seen
  const JsonParseResult r = engine::parse_json(text, value);
  if (!r.ok) {
    if (!value.is_null()) return Verdict::broken("a refusal left a value behind");
    if (r.message == nullptr || r.message[0] == '\0') {
      return Verdict::broken("a refusal says nothing of why");
    }
    if (r.offset > text.size()) return Verdict::broken("a refusal's offset is past the input");
    if (r.line == 0 || r.column == 0) return Verdict::broken("a refusal's line or column is 0");
    return {};
  }
  for (const bool pretty : {true, false}) {
    const std::string written = engine::write_json(value, JsonWriteOptions{pretty});
    JsonValue back;
    if (!engine::parse_json(written, back).ok) {
      return Verdict::broken(pretty ? "an accepted input's pretty writing is refused"
                                    : "an accepted input's compact writing is refused");
    }
    if (!(back == value)) {
      return Verdict::broken("an accepted input reads back from its own writing as another value");
    }
    if (engine::write_json(back, JsonWriteOptions{pretty}) != written) {
      return Verdict::broken("an accepted input's writing is not canonical: written again, it differs");
    }
  }
  return {};
}

// What a run has when content/fuzz/parse_json/ is not there (a test bundle that does not carry
// it): one document of every kind, and one the parser refuses.
constexpr std::string_view k_builtin_seeds[] = {
    R"({"a": [1, 2.5, -3, true, false, null, "s\n\"q\""], "b": {"c": {}}, "d": []})",
    R"([0, -0.0, 1e3, 18446744073709551615, -9223372036854775808, "é😀"])",
    R"({"truncated": [1, 2)",
};

// 512 inputs a seed from the byte layer and 256 from the JSON layer: counts, not a time, chosen so
// the case stays inside the 10 s a target that docs/ci/what-to-run.md gives it in a serial
// msvc-debug run.
constexpr engine::test::fuzz::TargetSpec k_parse_json = [] {
  engine::test::fuzz::TargetSpec spec;
  spec.name = "parse_json";
  spec.one = &fuzz_parse_json;
  spec.max_len = 64 * 1024;
  spec.byte_iterations = 512;
  spec.json_iterations = 256;
  spec.builtin_seeds = std::span<const std::string_view>(k_builtin_seeds);
  return spec;
}();

}  // namespace

ENGINE_FUZZ_TARGET(parse_json, k_parse_json)
