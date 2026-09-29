// engine-content tissue: the tissue definition on the command line (docs/subsystems/tissue.md,
// docs/subsystems/apps.md). Compiled against domain/tissue when this build has the capability
// (ENGINE_CONTENT_TISSUE); without it the command is refused with a sentence, as engine-view
// refuses
// `--animate` without animation, and `msvc-minimal` is the proof that the rest of the app builds.
//
//   tissue import <interchange.json> <out.tissue>   check every block's size and SHA-256, write the
//                                                   container
//   tissue info <file.tissue>                       the header, every section (known or not) and
//                                                   the definition's outline
//   tissue validate <file.tissue|interchange.json>  every validator row; exit 1 on a failing error
//       --expect <expected.json>                    the fixture mode: exit 0 exactly when the rows
//                                                   that fail or are skipped are the declared ones
//       --write-expect <expected.json>              write the declaration this run would match
//   tissue report <file.tissue|interchange.json>    the rows and every number they stand on
//   tissue capabilities                             what this build reads and evaluates, one line
//   tissue example <directory>                      the synthetic definition, its ten-node
//                                                   reference-body variant and the layered model's
//                                                   three fixtures, as interchanges and containers:
//                                                   worked examples of the format
//   tissue cage <reference.tissue> <out.tissue>     a runtime cage derived from a reference body
//       [--nodes <n>] [--hero] [--region <name>]    within ADR-0029's budget (domain/tissue/cage.h)
//       [--omit-state <name>]...
//   tissue settle <cage.tissue> --state <name>      the runtime region as a soft body, settled
//       [--repeat] [--write <out.tissue>]           under the state's load at the fixed step, and
//       [--max-steps <n>] [--region <name>]         measured against the state (soft_body.h)
//
// `validate` and `report` take `--source <reference.tissue>` for a derived cage: the reference file
// its derivation names, whose SHA-256 must be the recorded one, so the `cage.*` rows can measure
// the cage against it.
//
// **Exit 3 is a capability failure**: the file requires a record, a block kind, a row or a law
// this build does not have (capabilities.h), or a fixture's declaration names a row it does not
// implement. It is never a validation result — no row is written — and the fixture mode never
// matches it against a declared failure: a missing capability is not an expected physical failure.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TISSUE
#include <core/containers/vector.h>
#include <domain/tissue/cage.h>
#include <domain/tissue/capabilities.h>
#include <domain/tissue/expect.h>
#include <domain/tissue/sha256.h>
#include <domain/tissue/soft_body.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>
#include <foundation/io/vfs.h>

#include <engine_build_stamp.h>
#endif

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
[[maybe_unused]] constexpr int k_exit_capability = 3;

const char* k_tissue_usage =
    "usage: engine-content tissue <command> ...\n"
    "  import <interchange.json> <out.tissue>   check sizes and SHA-256s, write the container\n"
    "  info <file.tissue>                       header, sections and the definition's outline\n"
    "  validate <file.tissue|interchange.json>  every validator row as one JSON line; exit 1 when\n"
    "                                           an error row fails\n"
    "      --no-modes   skip transferring each binding under the other two normal modes\n"
    "      --expect <expected.json>\n"
    "                   the fixture mode: exit 0 exactly when the rows that fail or are skipped\n"
    "                   are the ones the file declares, whatever their severity; every\n"
    "                   difference is printed to stderr and in the line's \"expect\"\n"
    "      --write-expect <expected.json>\n"
    "                   write the declaration this run would match, for review; exit 0\n"
    "      --source <reference.tissue>\n"
    "                   a derived cage's reference file, for the cage.* rows; its SHA-256 must\n"
    "                   be the one the cage records\n"
    "  report <file.tissue|interchange.json>    the rows and the numbers they stand on\n"
    "      --no-modes, --source   as above\n"
    "  cage <reference.tissue> <out.tissue>     derive a runtime cage (four-node cells, a Runtime\n"
    "                                           region) from a reference body of ten-node cells\n"
    "                                           within ADR-0029's budget, as one JSON line\n"
    "      --nodes <n>    the node budget: 256 by default, at most 256 without --hero, 800 with\n"
    "      --hero         a hero volume (ADR-0029): the budget may reach 800\n"
    "      --region <name>  the reference region, when the file has more than one\n"
    "      --omit-state <name>  leave a state out (repeatable): a state one of the ten-node\n"
    "                     cells' corner tetrahedra inverts in is refused, never carried\n"
    "  settle <cage.tissue> --state <name>      build a four-node Runtime region as the physics\n"
    "                                           module's soft body, settle it at the fixed step\n"
    "                                           under the state's load from its rest, and measure\n"
    "                                           the settled nodes against the state's; exit 1 "
    "when\n"
    "                                           it does not settle\n"
    "      --repeat       settle twice from scratch and say whether they give the same bytes\n"
    "      --write <out.tissue>  the file with the settled nodes as a state \"<name>.settled\"\n"
    "      --max-steps <n>       the step limit (3,000, 50 s at 60 Hz, by default)\n"
    "      --sub-steps <n>, --iterations <n>\n"
    "                            collision sub-steps a step and XPBD iterations a sub-step\n"
    "                            (ADR-0029's 2 and 8 by default)\n"
    "      --region <name>       the region, when the file has more than one runtime region\n"
    "  capabilities                             the schema version, every record, field, enum\n"
    "                                           and block kind this build reads, every row as\n"
    "                                           evaluated, info-only or not-implemented, and the\n"
    "                                           laws it evaluates, as one JSON line\n"
    "  example <directory>                      write the synthetic definition as\n"
    "                                           <directory>/synthetic.json with its blocks and\n"
    "                                           <directory>/synthetic.tissue, its ten-node\n"
    "                                           reference body as synthetic-quadratic.*, and the\n"
    "                                           layered model's fixtures as layered-slab.*,\n"
    "                                           layered-fusiform.* and layered-tied-slab.*\n"
    "exit 3 is a capability failure: the file requires what this build does not have, or the\n"
    "declaration names a row it does not implement; never a validation result.\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content tissue: %s\n", message);
  std::fputs(k_tissue_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content tissue: %s\n", message.c_str());
  return k_exit_error;
}

[[maybe_unused]] void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

#if ENGINE_CONTENT_TISSUE

bool ends_with(std::string_view text, std::string_view tail) {
  return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

// A capability failure: its sentence on stderr, and a JSON line that says what is missing, so a
// script reads it as readily as a report. Exit 3.
int capability_failed(const std::string& path, const tissue::CapabilityFailure& failure) {
  std::fprintf(stderr, "engine-content tissue: %s: %s\n", path.c_str(), failure.sentence.c_str());
  JsonValue out = JsonValue::object();
  out.set("file", JsonValue(path));
  JsonValue capability = JsonValue::object();
  JsonValue missing = JsonValue::array();
  for (const std::string& m : failure.missing)
    missing.push_back(JsonValue(m));
  capability.set("missing", std::move(missing));
  capability.set("sentence", JsonValue(failure.sentence));
  out.set("capability_failure", std::move(capability));
  print_json(out);
  return k_exit_capability;
}

// A container, or an interchange when the path ends in .json. `capability` says whether a refusal
// was a capability failure.
bool load(const std::string& path, tissue::TissueFile& out, std::string& error,
          tissue::CapabilityFailure& capability) {
  if (ends_with(path, ".json")) return tissue::import_interchange(path, out, &error, &capability);
  return tissue::read_tissue_file(path, out, &error, &capability);
}

JsonValue outline(const tissue::TissueDefinition& d) {
  JsonValue out = JsonValue::object();
  out.set("format", JsonValue(d.format));
  out.set("name", JsonValue(d.name));
  const auto names = [](const auto& list) {
    JsonValue array = JsonValue::array();
    for (const auto& item : list)
      array.push_back(JsonValue(item.name));
    return array;
  };
  out.set("regions", names(d.regions));
  out.set("frames", names(d.frames));
  out.set("surfaces", names(d.surfaces));
  out.set("attachments", names(d.attachments));
  out.set("bindings", names(d.bindings));
  out.set("states", names(d.states));
  out.set("blocks", JsonValue(static_cast<u64>(d.blocks.size())));
  out.set("base_id", JsonValue(d.observation.base_id));
  out.set("accepted", JsonValue(d.observation.acceptance.accepted));
  // The layered model's records, where the definition has any.
  if (!d.material_surfaces.empty()) out.set("material_surfaces", names(d.material_surfaces));
  if (!d.material_skins.empty()) out.set("material_skins", names(d.material_skins));
  if (!d.thickness_fields.empty()) out.set("thickness_fields", names(d.thickness_fields));
  if (!d.depot_partitions.empty()) out.set("depot_partitions", names(d.depot_partitions));
  if (!d.contact_pairs.empty()) out.set("contact_pairs", names(d.contact_pairs));
  if (!d.parameter_domains.empty()) out.set("parameter_domains", names(d.parameter_domains));
  if (!d.parameter_samples.empty()) out.set("parameter_samples", names(d.parameter_samples));
  if (!d.certificates.empty()) out.set("certificates", names(d.certificates));
  return out;
}

int import_command(const std::string& input, const std::string& output) {
  tissue::TissueFile file;
  std::string error;
  tissue::CapabilityFailure capability;
  if (!tissue::import_interchange(input, file, &error, &capability))
    return capability.failed() ? capability_failed(input, capability)
                               : failed(input + ": " + error);
  if (!tissue::write_tissue_file(output, file, &error)) return failed(output + ": " + error);
  std::string bytes;
  io::read_file(output, bytes);
  JsonValue out = JsonValue::object();
  out.set("input", JsonValue(input));
  out.set("output", JsonValue(output));
  out.set("bytes", JsonValue(static_cast<u64>(bytes.size())));
  out.set("blocks", JsonValue(static_cast<u64>(file.blocks.size())));
  out.set("definition", outline(file.definition));
  print_json(out);
  return k_exit_ok;
}

int info_command(const std::string& path) {
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok)
    return failed("cannot read '" + path + "': " + io::status_name(status));
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  tissue::TissueFileInfo info;
  std::string error;
  if (!tissue::read_tissue_file_info(view, info, &error)) return failed(path + ": " + error);
  tissue::TissueFile file;
  tissue::CapabilityFailure capability;
  if (!tissue::read_tissue_file_memory(view, file, &error, &capability))
    return capability.failed() ? capability_failed(path, capability) : failed(path + ": " + error);
  JsonValue out = JsonValue::object();
  out.set("file", JsonValue(path));
  out.set("version", JsonValue(info.header.version));
  out.set("bytes", JsonValue(info.header.total_bytes));
  char hash[32];
  std::snprintf(hash, sizeof(hash), "%016llx",
                static_cast<unsigned long long>(info.header.content_hash));
  out.set("content_hash", JsonValue(hash));
  JsonValue sections = JsonValue::array();
  for (const tissue::TissueFileSectionInfo& s : info.sections) {
    JsonValue e = JsonValue::object();
    e.set("kind", JsonValue(s.kind));
    e.set("kind_name", JsonValue(s.kind_name));
    e.set("name", JsonValue(s.name));
    e.set("element_size", JsonValue(s.element_size));
    e.set("count", JsonValue(s.element_count));
    e.set("offset", JsonValue(s.offset));
    e.set("known", JsonValue(s.known));
    sections.push_back(std::move(e));
  }
  out.set("sections", std::move(sections));
  out.set("definition", outline(file.definition));
  JsonValue warnings = JsonValue::array();
  for (const std::string& w : file.warnings)
    warnings.push_back(JsonValue(w));
  out.set("warnings", std::move(warnings));
  print_json(out);
  return k_exit_ok;
}

struct ValidateArgs {
  bool report = false;
  bool modes = true;
  std::string expect;        // --expect: the fixture mode's declaration
  std::string write_expect;  // --write-expect: where to write the declaration this run matches
  std::string source;        // --source: a derived cage's reference file
};

// A container's bytes and their SHA-256, and the file read from them. `capability` says whether a
// refusal was a capability failure.
bool load_container(const std::string& path, tissue::TissueFile& out, std::string& sha256,
                    std::string& error, tissue::CapabilityFailure& capability) {
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    error = std::string("cannot read it: ") + io::status_name(status);
    return false;
  }
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  sha256 = tissue::sha256_hex(view);
  return tissue::read_tissue_file_memory(view, out, &error, &capability);
}

int validate_command(const std::string& path, const ValidateArgs& args) {
  // The declaration is read before the definition is validated, so a broken one costs nothing.
  tissue::TissueExpectation expectation;
  std::string error;
  if (!args.expect.empty() && !tissue::read_expectation(args.expect, expectation, &error))
    return failed(args.expect + ": " + error);
  // A derived cage's source: read whole, hashed, and handed to the cage rows.
  tissue::TissueFile source;
  std::string source_sha256;
  if (!args.source.empty()) {
    tissue::CapabilityFailure capability;
    if (!load_container(args.source, source, source_sha256, error, capability))
      return capability.failed() ? capability_failed(args.source, capability)
                                 : failed(args.source + ": " + error);
  }
  // A declared failure of a row this build does not implement is a capability the build lacks,
  // not a failure it can match: refused before anything is validated.
  if (!args.expect.empty()) {
    const tissue::Capabilities& capabilities = tissue::build_capabilities();
    tissue::CapabilityFailure unimplemented;
    for (const tissue::ExpectedFailure& f : expectation.failures)
      if (capabilities.row(f.id) == tissue::RowStatus::not_implemented)
        unimplemented.missing.push_back("row " + f.id + ", which the declaration names");
    if (unimplemented.failed()) {
      unimplemented.sentence =
          "a capability failure, not a fixture difference: the declaration names rows this build "
          "does not implement, and a missing capability is never an expected failure";
      return capability_failed(args.expect, unimplemented);
    }
  }
  tissue::TissueFile file;
  tissue::CapabilityFailure capability;
  if (!load(path, file, error, capability))
    return capability.failed() ? capability_failed(path, capability) : failed(path + ": " + error);
  tissue::ValidateOptions options;
  options.compare_modes = args.modes;
  if (!args.source.empty()) {
    options.cage_source = &source;
    options.cage_source_sha256 = source_sha256;
  }
  tissue::TissueReport result;
  tissue::validate_tissue(file, options, result);
  JsonValue out = args.report ? tissue::report_json(result) : tissue::rows_json(result);
  out.set("file", JsonValue(path));
  if (!args.write_expect.empty()) {
    const tissue::TissueExpectation written = tissue::expectation_from_report(
        result, "written by engine-content tissue validate --write-expect from " + path +
                    ": review it before it is a fixture's declaration");
    const io::Status status =
        io::write_file_atomic(args.write_expect, tissue::write_expectation(written));
    if (status != io::Status::Ok)
      return failed("cannot write '" + args.write_expect + "': " + io::status_name(status));
    out.set("expect_written", JsonValue(args.write_expect));
    out.set("expect_failures", JsonValue(static_cast<u64>(written.failures.size())));
    print_json(out);
    return k_exit_ok;
  }
  if (!args.expect.empty()) {
    tissue::ExpectationResult compared;
    tissue::compare_expectation(result, expectation, compared);
    JsonValue e = tissue::expectation_json(compared);
    e.set("file", JsonValue(args.expect));
    out.set("expect", std::move(e));
    print_json(out);
    const std::string text = tissue::expectation_text(compared);
    if (!text.empty()) std::fputs(text.c_str(), stderr);
    return compared.matched() ? k_exit_ok : k_exit_error;
  }
  print_json(out);
  if (!args.report && result.errors > 0) return k_exit_error;
  return k_exit_ok;
}

struct CageArgs {
  u32 nodes = 0;
  bool hero = false;
  std::string region;
  Vector<std::string> omit_states;
};

int cage_command(const std::string& input, const std::string& output, const CageArgs& args) {
  tissue::TissueFile reference;
  std::string sha256;
  std::string error;
  tissue::CapabilityFailure capability;
  // A container is hashed as it is on disk; an interchange as this build encodes it.
  const bool loaded = ends_with(input, ".json")
                          ? tissue::import_interchange(input, reference, &error, &capability)
                          : load_container(input, reference, sha256, error, capability);
  if (!loaded)
    return capability.failed() ? capability_failed(input, capability)
                               : failed(input + ": " + error);
  tissue::CageOptions options;
  options.node_budget = args.nodes;
  options.hero = args.hero;
  options.region = args.region;
  options.omit_states = args.omit_states;
  options.source_sha256 = sha256;
  tissue::TissueFile cage;
  tissue::CageSummary summary;
  if (!tissue::derive_cage(reference, options, cage, summary, &error))
    return failed(input + ": " + error);
  if (!tissue::write_tissue_file(output, cage, &error)) return failed(output + ": " + error);
  JsonValue out = tissue::cage_summary_json(summary);
  out.set("input", JsonValue(input));
  out.set("output", JsonValue(output));
  std::string bytes;
  io::read_file(output, bytes);
  out.set("bytes", JsonValue(static_cast<u64>(bytes.size())));
  out.set("output_sha256", JsonValue(tissue::sha256_hex(std::span<const u8>(
                               reinterpret_cast<const u8*>(bytes.data()), bytes.size()))));
  print_json(out);
  return k_exit_ok;
}

struct SettleArgs {
  std::string state;
  std::string region;
  std::string write;
  bool repeat = false;
  u32 max_steps = 0;
  u32 sub_steps = 0;
  u32 iterations = 0;
};

int settle_command(const std::string& path, const SettleArgs& args) {
  tissue::TissueFile file;
  std::string error;
  tissue::CapabilityFailure capability;
  if (!load(path, file, error, capability))
    return capability.failed() ? capability_failed(path, capability) : failed(path + ": " + error);
  tissue::SoftBodyOptions options;
  options.region = args.region;
  options.load_state = args.state;
  if (args.max_steps != 0) options.max_steps = args.max_steps;
  if (args.sub_steps != 0) options.sub_steps = args.sub_steps;
  if (args.iterations != 0) options.iterations = args.iterations;
  tissue::SettleResult result;
  if (!tissue::settle_soft_body(file, options, result, &error)) return failed(path + ": " + error);
  JsonValue out = tissue::settle_json(file, options, result);
  out.set("file", JsonValue(path));
  if (args.repeat) {
    // A second settle from scratch: the same bytes, or a determinism failure worth an exit code.
    tissue::SettleResult again;
    if (!tissue::settle_soft_body(file, options, again, &error)) return failed(path + ": " + error);
    const bool same = again.steps == result.steps && again.nodes.size() == result.nodes.size() &&
                      std::memcmp(again.nodes.data(), result.nodes.data(),
                                  result.nodes.size() * sizeof(Vec3)) == 0;
    JsonValue repeat = JsonValue::object();
    repeat.set("same_bytes", JsonValue(same));
    repeat.set("wall_ms", JsonValue(again.wall_ms));
    out.set("repeat", std::move(repeat));
    if (!same) {
      print_json(out);
      return failed("two settles of " + path + " gave different bytes");
    }
  }
  if (!args.write.empty()) {
    // The cage with the settled nodes as a state of its own, for validate to read.
    tissue::TissueFile settled = file;
    const tissue::RegionState* load_state = nullptr;
    for (const tissue::RegionState& s : file.definition.states)
      if (s.name == args.state) load_state = &s;
    tissue::RegionState state = *load_state;
    state.name = args.state + ".settled";
    state.nodes = "settled." + args.state;
    state.role = tissue::StateRole::Response;
    state.provenance = tissue::StateProvenance::ForwardFromAuthoredRest;
    state.inverse.reset();
    state.expected_visible.clear();
    state.binding.clear();
    tissue::add_block(settled, state.nodes, tissue::BlockKind::StateNodes,
                      std::span<const Vec3>(result.nodes.data(), result.nodes.size()));
    settled.definition.states.push_back(state);
    if (!tissue::write_tissue_file(args.write, settled, &error))
      return failed(args.write + ": " + error);
    out.set("written", JsonValue(args.write));
  }
  print_json(out);
  if (!result.settled) return failed(path + ": did not settle within the step limit");
  return k_exit_ok;
}

int capabilities_command() {
  JsonValue out = tissue::capabilities_json(tissue::build_capabilities());
  JsonValue build = JsonValue::object();
  build.set("tool", JsonValue("engine-content"));
  build.set("commit", JsonValue(build_stamp::commit()));
  build.set("dirty", JsonValue(build_stamp::dirty()));
  out.set("build", std::move(build));
  print_json(out);
  return k_exit_ok;
}

int example_command(const std::string& directory) {
  std::string error;
  if (io::make_directories(directory) != io::Status::Ok)
    return failed("cannot create '" + directory + "'");
  JsonValue out = JsonValue::object();
  const auto write = [&](const tissue::TissueFile& file, const std::string& stem,
                         JsonValue& entry) {
    if (!tissue::export_interchange(file, directory, stem, &error)) return false;
    const std::string container = io::join_path(directory, stem + ".tissue");
    if (!tissue::write_tissue_file(container, file, &error)) return false;
    entry = JsonValue::object();
    entry.set("interchange", JsonValue(io::join_path(directory, stem + ".json")));
    entry.set("container", JsonValue(container));
    entry.set("blocks", JsonValue(static_cast<u64>(file.blocks.size())));
    entry.set("definition", outline(file.definition));
    return true;
  };
  for (const bool quadratic : {false, true}) {
    tissue::SyntheticOptions options;
    options.quadratic = quadratic;
    const tissue::SyntheticTissue synthetic = tissue::make_synthetic_tissue(options);
    JsonValue e;
    if (!write(synthetic.file, quadratic ? "synthetic-quadratic" : "synthetic", e))
      return failed(error);
    if (quadratic) {
      out.set("quadratic", std::move(e));
    } else {
      for (auto [key, value] : e.as_object())
        out.set(key, value);
    }
  }
  JsonValue slab;
  if (!write(tissue::make_layered_slab(), "layered-slab", slab)) return failed(error);
  out.set("layered_slab", std::move(slab));
  JsonValue fusiform;
  if (!write(tissue::make_layered_fusiform(), "layered-fusiform", fusiform)) return failed(error);
  out.set("layered_fusiform", std::move(fusiform));
  JsonValue tied;
  if (!write(tissue::make_layered_tied_slab(), "layered-tied-slab", tied)) return failed(error);
  out.set("layered_tied_slab", std::move(tied));
  print_json(out);
  return k_exit_ok;
}

#endif

}  // namespace

int tissue_command(int argc, char** argv) {
  if (argc < 3) return usage(nullptr);
  const std::string_view sub = argv[2];
  if (sub == "--help" || sub == "-h" || sub == "help") {
    std::fputs(k_tissue_usage, stdout);
    return k_exit_ok;
  }
#if ENGINE_CONTENT_TISSUE
  std::string positional[2];
  u32 count = 0;
  ValidateArgs args;
  CageArgs cage_args;
  bool cage_flags = false;
  SettleArgs settle_args;
  bool settle_flags = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--state" || a == "--write") {
      if (i + 1 >= argc) return usage("--state and --write take a name and a path");
      (a == "--state" ? settle_args.state : settle_args.write) = argv[++i];
      settle_flags = true;
    } else if (a == "--repeat") {
      settle_args.repeat = true;
      settle_flags = true;
    } else if (a == "--max-steps" || a == "--sub-steps" || a == "--iterations") {
      if (i + 1 >= argc) return usage("--max-steps, --sub-steps and --iterations take a count");
      const std::string_view v = argv[++i];
      u32 parsed = 0;
      for (const char c : v) {
        if (c < '0' || c > '9' || parsed > 10000000u)
          return usage("--max-steps, --sub-steps and --iterations take a count");
        parsed = parsed * 10u + static_cast<u32>(c - '0');
      }
      if (v.empty() || parsed == 0)
        return usage("--max-steps, --sub-steps and --iterations take a count");
      (a == "--max-steps"   ? settle_args.max_steps
       : a == "--sub-steps" ? settle_args.sub_steps
                            : settle_args.iterations) = parsed;
      settle_flags = true;
    } else if (a == "--no-modes") {
      args.modes = false;
    } else if (a == "--expect" || a == "--write-expect" || a == "--source") {
      if (i + 1 >= argc) return usage("--expect, --write-expect and --source take a path");
      (a == "--expect"   ? args.expect
       : a == "--source" ? args.source
                         : args.write_expect) = argv[++i];
    } else if (a == "--nodes") {
      if (i + 1 >= argc) return usage("--nodes takes a count");
      const std::string_view v = argv[++i];
      u32 parsed = 0;
      for (const char c : v) {
        if (c < '0' || c > '9' || parsed > 100000u) return usage("--nodes takes a count");
        parsed = parsed * 10u + static_cast<u32>(c - '0');
      }
      if (v.empty() || parsed == 0) return usage("--nodes takes a count");
      cage_args.nodes = parsed;
      cage_flags = true;
    } else if (a == "--hero") {
      cage_args.hero = true;
      cage_flags = true;
    } else if (a == "--region" || a == "--omit-state") {
      if (i + 1 >= argc) return usage("--region and --omit-state take a name");
      if (a == "--region")
        cage_args.region = argv[++i];
      else
        cage_args.omit_states.push_back(argv[++i]);
      cage_flags = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (count < 2) {
      positional[count++] = std::string(a);
    } else {
      return usage("too many arguments");
    }
  }
  if (sub != "validate" && (!args.expect.empty() || !args.write_expect.empty()))
    return usage("--expect and --write-expect belong to validate");
  if (sub != "validate" && sub != "report" && !args.source.empty())
    return usage("--source belongs to validate and report");
  if (sub == "settle") {
    // `--region` is settle's too: the cage's flags otherwise are not.
    if (cage_args.nodes != 0 || cage_args.hero || !cage_args.omit_states.empty())
      return usage("--nodes, --hero and --omit-state belong to cage");
    if (count != 1) return usage("settle takes one cage file");
    if (settle_args.state.empty()) return usage("settle needs --state <name>");
    settle_args.region = cage_args.region;
    return settle_command(positional[0], settle_args);
  }
  if (settle_flags)
    return usage(
        "--state, --write, --repeat, --max-steps, --sub-steps and --iterations belong to "
        "settle");
  if (sub != "cage" && cage_flags)
    return usage("--nodes, --hero, --region and --omit-state belong to cage");
  if (sub == "cage") {
    if (count != 2) return usage("cage takes a reference file and an output path");
    return cage_command(positional[0], positional[1], cage_args);
  }
  if (!args.expect.empty() && !args.write_expect.empty())
    return usage("--expect compares with a declaration and --write-expect writes one: not both");
  if (sub == "capabilities") {
    if (count != 0 || !args.modes) return usage("capabilities takes nothing");
    return capabilities_command();
  }
  if (sub == "import") {
    if (count != 2) return usage("import takes an interchange JSON and an output path");
    return import_command(positional[0], positional[1]);
  }
  if (count != 1) return usage("the command takes one path");
  if (sub == "info") return info_command(positional[0]);
  if (sub == "validate") return validate_command(positional[0], args);
  if (sub == "report") {
    args.report = true;
    return validate_command(positional[0], args);
  }
  if (sub == "example") return example_command(positional[0]);
  return usage("unknown tissue command");
#else
  (void)sub;
  return failed(
      "this engine-content was built without the tissue capability (ENGINE_WITH_TISSUE=OFF, "
      "ENGINE_WITH_PHYSICS=OFF, or ENGINE_MINIMAL=ON); configure with both on to read tissue "
      "definitions");
#endif
}

}  // namespace engine::content
